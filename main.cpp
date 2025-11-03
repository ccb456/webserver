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
        cout << "usage: <ip> <port>" << endl;
        return 1;
    }

    string ip = argv[1];
    int port = atoi(argv[2]);

    /* 忽略SIGPIPE信号 */
    addsig(SIGPIPE, SIG_IGN);

    /* 创建线程池 */
    std::shared_ptr<ThreadPool> threadsPool(new ThreadPool(4));

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
    epoll_event events[MAX_EVENT_NUMBER];
    

   
    







}

