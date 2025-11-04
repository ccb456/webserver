#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <iostream>
#include <memory>

#include "./http/httpConn.h"
#include "./pool/threadPool/threadPool.h"
#include "./pool/sqlConnPool/connPoolRAII.h"
#include "./timer/timerHeap.h"
#include "constance.h"

using std::cout;
using std::endl;

extern void addFd(int epollfd, int fd, bool isOneShot);
extern int setnoblocking(int fd);
extern void removeFd(int epollfd, int fd);
extern void modfd(int epollfd, int fd, int ev);


// 设置定时器相关信息
static int pipefd[2];
static int epollfd = 0;
static HeapTimer heapTimer;

// 信号处理函数
void sig_handler(int sig)
{
    int saveErrno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = saveErrno;
}


// 添加信号
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }

    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, nullptr) != -1);
}

// 定时任务的处理函数
void timer_handler()
{
    heapTimer.tick();
    alarm(TIMESLOT);
}


int main(int argc, char** argv)
{
    if(argc <= 2)
    {
        #ifdef debug
            cout << "usage: <ip> <port>" << endl;
        #endif
        return 1;
    }

    string ip = argv[1];
    int port = atoi(argv[2]);

    /* 忽略SIGPIPE信号 */
    addsig(SIGPIPE, SIG_IGN);

    /* 创建线程池 */
    ThreadPool* threadsPool = new ThreadPool(4);

    /* 预先创建HTTP连接 */
    std::vector<HttpConn> users(MAX_FD);

    /* 套接字 */
    int listenFd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenFd >= 0);
    
    /* 绑定地址*/
    sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr);

    int ret = bind(listenFd, (struct sockaddr*)(&serverAddr), sizeof(serverAddr));
    assert(ret >= 0);
    ret = listen(listenFd, 8);
    assert(ret >= 0);

    // 统一事件源
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd >= 0);

    addFd(epollfd, listenFd, false);
    HttpConn::epollfd = epollfd;

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnoblocking(pipefd[1]);
    addFd(epollfd, pipefd[0], false);

    // 设置时钟，终止信号处理函数
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    bool stopServer = false;

    bool timeout = false;

    alarm(TIMESLOT);

    int numbers = -1;
    while(!stopServer)
    {
        numbers = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(numbers < 0 && errno != EINTR)
        {
            #ifdef debug
                cout << "epoll_wait failed" << endl;
            #endif
            break;
        }

        for(int i = 0; i < numbers; ++i)
        {
            int sockfd = events[i].data.fd;

            /* 说明有新连接 */
            if(sockfd == listenFd)
            {
                sockaddr_in clntAddr;
                socklen_t addrLen = 0;
                
                /* listenfd ET 模型*/
                int connfd = -1;
                while(1)
                {
                    connfd = accept(listenFd, (struct sockaddr*)&clntAddr, &addrLen);
                    if(connfd < 0)
                    {
                        #ifdef debug
                            cout << "accept error" << endl;
                        #endif
                        break;
                    }
    
                    if(HttpConn::userCount >= MAX_FD)
                    {
                        #ifdef debug
                            cout << "userCount >= MAX_FD" << endl;
                        #endif
                        break;
                    }
                    
                    // 分配一个http并初始化连接
                    users[connfd].init(connfd, clntAddr);
    
                    // 设置定时器
                    heapTimer.add(connfd, 3 * TIMESLOT, timer_handler);

                }

                continue;   // 不用继续if判断了
            }
            /* 关闭连接 */
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 删除定时器
                heapTimer.doWork(sockfd);

            }
            /* 处理信号 */
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig = -1;
                char signals[1024];
                ret = recv(sockfd, signals, sizeof(signals), 0);
                if(ret <= 0)
                {
                    continue;
                }
                else if(ret > 0)
                {
                    for(int i = 0; i < ret; ++i)
                    {
                        switch(signals[i])
                        {
                            case SIGALRM :
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stopServer = true;
                                break;
                            }

                            default:
                                break;
                        }
                    }
                }

                /* 处理客户端连接上接到的数据 */
                else if(events[i].events & EPOLLIN)
                {
                    if(users[sockfd].readFromClnt())
                    {
                        #ifdef debug
                            cout << "deal with the client: " << inet_ntoa(users[sockfd].getAddr()->sin_addr);
                        #endif

                        // 线程池中加入任务
                        threadsPool->addTask(users[sockfd].process);

                        // 调整定时器
                        heapTimer.adjust(sockfd, 3 * TIMESLOT);
                    }
                    else // 读失败
                    {
                        heapTimer.doWork(sockfd);
                    }
                }
                /* 向客户端写数据 */
                else if(events[i].events & EPOLLOUT)
                {
                    if(users[sockfd].writeToClnt())
                    {
                        #ifdef debug
                        cout << "send data to the client" << inet_ntoa(users[sockfd].getAddr()->sin_addr) << endl;
                        #endif

                        // 调整定时器
                        heapTimer.adjust(sockfd, 3 * TIMESLOT);
                    }
                    else
                    {
                        heapTimer.doWork(sockfd);
                    }
                }


                // 出现超时信号了
                if(timeout)
                {
                    timer_handler();
                    timeout = false;
                }

            } 

        }
    }


    close(epollfd);
    close(listenFd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete threadsPool;
    return 0;
    
}

