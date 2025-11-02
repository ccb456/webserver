/** 
 * HTTP连接类
 */

#ifndef HTTPCONN_H
#define HTTPCONN_H 

#include <string>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <algorithm> 
#include <sys/mman.h>
#include <errno.h>
#include <sys/uio.h>
#include <atomic>
#include <cstdarg>
#include "../constance.h"

using std::string;


class HttpConn
{

    public:
        /* 主状态机的两种状态 */
        enum CHECK_STATE 
        {
            CHECK_REQUESTLINE = 0,  // 检查请求行
            CHECK_HEADER,            // 检查请求头
            CHECK_CONTENT           // 检查消息体
        };
        /* 从状态机的状态 */
        enum LINE_STATUS 
        {
            LINE_OK,            // 读到完整的一行
            LINE_OPEN,          // 没有读到完整的一行
            LINE_BAD            // 行出错
        };

        /* 处理HTTP的结果*/
        enum HTTP_CODE 
        {
            NO_REQUEST,         // 请求不完整，需要继续读取数据
            GET_REQUEST,        // 获得一个完整的请求
            BAD_REQUEST,        // 请求格式错误
            NO_RESOURCE,        // 没有这个资源
            FILE_REQUETS,       // 文件资源
            FORBIDDEN_REQUEST,  // 客户对资源没有权限
            INTERNAL_ERROR,     // 服务器内部错误
            CLOSED_CONNECTION   // 客户端已经关闭连接

        };

        /* HTTP请求方法 */
        enum METHOD {
            GET,
            POST
        };

    public:
        static int epollfd;
        static std::atomic<int> userCount;

    public:
        HttpConn() {};
        ~HttpConn(){};

    public:
        void init(const int sockfd, const sockaddr_in& addr);
        bool writeToClnt();     // 向客户端发送信息
        bool readFromClnt();    // 读一次数据
        void process();         // 运行

        void closeConn(bool isClose = true);
        sockaddr_in* getAddr() 
        {
            return &clntAddr;
        }
    
    private:
        void init();

        /* 处理读到的内容 */
        HTTP_CODE processRead();

        /* 获得一行 */
        char* getOneLine() { return readBuffer + lineIdx;}
        /* 解析一行 */
        LINE_STATUS paraseLine();
        /* 解析请求行 */ 
        HTTP_CODE paraseRequestLine(string text);
        /* 解析请求头 */
        HTTP_CODE paraseRequestHeader(string text);
        /* 解析请求内容 */
        HTTP_CODE paraseRequestContent(string text);

        /* 执行请求 */
        HTTP_CODE do_request();

        /* 释放映射内存 */
        void unmap();

        /* 填写回复内容 */
        bool addResponse(const char*, ...);
        
        /* 填写状态行 */
        bool addStatuLine(int, string);
        /* 填写消息头*/
        bool addHeader(int contentLength);
        /* 添加内容长度 */
        bool addContentLen(int contentLen);
        /* 添加内容类型 */
        bool addContentType();
        /* 添加是否保持连接 */
        bool addIsKeepLive();
        /* 填写空白行 */
        bool addBlankLine();
        /* 填写内容 */
        bool addContent(string text);

        /* 处理写的内容 */
        bool processWrite(HTTP_CODE code); 


    private:

        /* 请求行相关信息 */
        METHOD m_method;
        string m_url;
        string m_version;

        /* 请求头相关信息 */
        string m_host;
        int content_length;
        bool isKeepLive;

        /* 请求体相关信息 */
        string requestBody;

        /* 请求客户端的信息 */
        int sockfd;
        sockaddr_in clntAddr;

        /* 读写缓冲区相关信息 */
        char readBuffer[READ_BUFF_SIZE];
        char writeBuffer[WRITE_BUFF_SIZE];
        int readIdx;
        int writeIdx;
        int curIdx;

        /* 处理行号*/
        int lineIdx;

        /* 请求文件相关信息 */
        string filePath;            // 请求文件的路径
        char* fileAddr;             // map后的映射地址
        struct stat fileInfo;       // 文件详情

        /* 分散内存 */
        struct iovec iov[2];
        int iovCount;

        /* 主状态机的状态 */
        CHECK_STATE curState;

        /* 写入数据的时候用到*/
        int bytesToSend;
        int bytesHaveSend;
        
};

 #endif