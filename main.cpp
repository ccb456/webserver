#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <iostream>
#include <memory>
#include <libgen.h>  // 用于dirname

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

void cb_func(HttpConn* httpconn)
{
    httpconn->closeConn();
}

// 新增：计算资源根目录（程序所在目录 + "/resources"）
string getRootPath(const char* argv0) 
{
    // 1. 将程序路径转换为绝对路径（如：/home/ccb/Code/project/webserver/build/Webserver）
    char* absPath = realpath(argv0, nullptr);
    if (!absPath) 
    {
        perror("realpath failed");
        exit(EXIT_FAILURE);
    }

    // 2. 获取程序所在目录（如：/home/ccb/Code/project/webserver/build）
    char* dir = dirname(absPath);
    std::string root(dir);

    // 3. 剔除路径中的"build"目录（关键步骤）
    size_t buildPos = root.find("/build");
    if (buildPos != std::string::npos) 
    {
        // 截取到"build"的前一级目录（如：/home/ccb/Code/project/webserver）
        root = root.substr(0, buildPos);
    }

    // 4. 拼接资源目录（最终：/home/ccb/Code/project/webserver/resources）
    root += "/resources";

    free(absPath);
    return root;
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

    // 关键：计算并设置全局rootPath
    rootPath = getRootPath(argv[0]);
    #ifdef debug
        std::cout << "动态获取的资源根目录: " << rootPath << std::endl;
    #endif

    string ip = argv[1];
    int port = atoi(argv[2]);

    /* 忽略SIGPIPE信号 */
    addsig(SIGPIPE, SIG_IGN);


    /* 创建数据库连接池 */
    SqlConnPool* connPool = SqlConnPool::getInstance();
    connPool->init("localhost", 3306, "ccb", "123456", "webserver", 4);
    
    /* 创建线程池 */
    std::shared_ptr<ThreadPool<HttpConn>> threadsPool(new ThreadPool<HttpConn>(8, 16, connPool));
    
    /* 预先创建HTTP连接 */
    std::vector<HttpConn> users(MAX_FD);
    /* 读取用户表信息 */
    users[0].initMySQLResult(connPool);

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
                socklen_t addrLen = sizeof(clntAddr);
                
                /* listenfd ET 模型*/
                int connfd = -1;
                while(1)
                {
                    connfd = accept(listenFd, (struct sockaddr*)&clntAddr, &addrLen);

                    if(connfd < 0)
                    {
                        // 仅处理非 EAGAIN/EWOULDBLOCK 的错误
                        if(errno != EAGAIN && errno != EWOULDBLOCK)
                        {
                            #ifdef debug
                                cout << "accept error: " << strerror(errno) << endl;  // 打印具体错误
                            #endif
                        }

                        break;
                    }
    
                    if(HttpConn::userCount >= MAX_FD)
                    {
                        #ifdef debug
                            cout << "userCount >= MAX_FD" << endl;
                        #endif
                        break;
                    }

                    #ifdef debug
                        cout << "新连接: connfd = " << connfd << endl;
                    #endif
                    
                    // 分配一个http并初始化连接
                    users[connfd].init(connfd, clntAddr);
    
                    // 设置定时器
                    heapTimer.add(connfd, 3 * TIMESLOT, std::bind(cb_func, &users[connfd]));

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
                // int sig = -1;
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
            }

            /* 处理客户端连接上接到的数据 */
            else if(events[i].events & EPOLLIN)
            {
                if(users[sockfd].readFromClnt())
                {
                    #ifdef debug
                        cout << "deal with the client: " << inet_ntoa(users[sockfd].getAddr()->sin_addr) << endl;
                    #endif
                    
                    // 线程池中加入任务
                    threadsPool->addTask(&users[sockfd]);

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
                    cout << "send data to the client " << inet_ntoa(users[sockfd].getAddr()->sin_addr) << endl;
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


    close(epollfd);
    close(listenFd);
    close(pipefd[1]);
    close(pipefd[0]);
    return 0;
    
}

