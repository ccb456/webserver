#include "httpConn.h"

int HttpConn::epollfd = 0;
int HttpConn::userCount = 0;

string rootPath = "/home/ccb/code/Webserver/resources/";


int setnoblocking(int fd)
{
    int oldOpt = fcntl(fd, F_GETFL);
    int newOpt = oldOpt | O_NONBLOCK;
    fcntl(fd, F_SETFL, newOpt);

    return oldOpt;
}

int addFd(int epollfd, int fd, epoll_event* ev, bool isOneShot = false)
{
    ev->events |= EPOLLET | EPOLLIN;
    if(isOneShot)
    {
        ev->events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, ev);
}

int removeFd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
}

void HttpConn::init()
{
        m_url = "";
        m_version = "";
        m_host = "";
        content_length = 0;
        isKeepLive = true;
       
        memset(readBuffer, 0, READ_BUFF_SIZE);
        memset(writeBuffer, 0, WRITE_BUFF_SIZE);

        readIdx = 0;
        writeIdx = 0;
        curIdx = 0;

        lineIdx = 0;

       
        string flleName = "";        
        fileAddr = nullptr;   

        iovCount = 0;

        
        curState = CHECK_REQUESTLINE;
}

void HttpConn::init(const int m_sockfd, const sockaddr_in& addr) 
{
    sockfd = m_sockfd;
    clntAddr = addr;
    init();
}

HttpConn::LINE_STATUS HttpConn::paraseLine()
{
    /**
     * 现在所有数据已经读入到readBuffer中，我们要从所有数据中解析出一行。
     *  一行结束的标志为\r\n
     * */ 

    // LINE_STATUS  lineState = LINE_OPEN;
    for(; curIdx < readIdx; ++curIdx)
    {

        if(curIdx + 1 >= readIdx)
        {
            return LINE_OPEN;
        }

        if(readBuffer[curIdx] == '\r')
        {
            if(readBuffer[curIdx + 1] == '\n')
            {
                /* 设置结束标志 */
                readBuffer[curIdx++] = '\0';
                readBuffer[curIdx++] = '\0';

                return LINE_OK;
            }

            return LINE_BAD;
        }
        else if(readBuffer[curIdx] == '\n')
        {
            if(curIdx > 0 && (readBuffer[curIdx - 1] == '\r'))
            {
                /* 设置结束标志 */
                readBuffer[curIdx - 1] = '\0';
                readBuffer[curIdx++] = '\0';

                return LINE_OK;
            }

            return LINE_BAD;
        }
    }

    return LINE_OPEN;
}

HttpConn::LINE_STATUS HttpConn::paraseRequestLine(string text)
{
    /**
     * 处理请求行
     * 请求行格式如下：
     *  GET /api/user/1001 HTTP/1.1
     */

    auto idx = text.find_first_of(" ");
    if(idx == string::npos)
    {
        return LINE_BAD;
    }

    string method = text.substr(0, idx);
    if (method.empty()) 
    {
        return LINE_BAD;
    }
    // 统一转为大写（原地修改method）
    std::transform(method.begin(), method.end(), method.begin(), ::toupper);

    // 简化判断：只需要检查大写形式
    if (method == "GET") 
    {
        m_method = GET;
    } 
    else if (method == "POST") 
    {
        m_method = POST;
    } 
    else 
    {
        // 处理其他方法（如PUT、DELETE等）或默认值
        m_method = GET; // 或返回错误
    }

    // 跳过空格，定位到URL起始位置
    idx = text.find_first_not_of(" ", idx + 1);
    if (idx == string::npos) 
    {
        return LINE_BAD; // 跳过空格后无内容（无URL），格式错误
    }

    // 查找URL结束位置（下一个空格）
    auto urlEnd = text.find_first_of(" ", idx);
    // 提取URL：如果没有下一个空格，说明URL到结尾（但HTTP请求行必须有版本，此处应视为错误）
    if (urlEnd == string::npos) 
    {
        return LINE_BAD; // 无版本信息，格式错误
    }
    m_url = text.substr(idx, urlEnd - idx);
    if (m_url.empty()) 
    {
        return LINE_BAD;
    }   

    // 跳过空格，定位到版本起始位置
    idx = text.find_first_not_of(" ", urlEnd + 1);
    if (idx == string::npos) 
    {
        return LINE_BAD; // 跳过空格后无版本内容，格式错误
    }

    // 提取版本（到字符串结尾）
    m_version = text.substr(idx);
    std::transform(m_version.begin(), m_version.end(), m_version.begin(), ::toupper);

    if(m_version != "HTTP/1.1" && m_version != "HTTP/1.0")
    {
        return LINE_BAD;
    }
    
    return LINE_OK;
}

HttpConn::LINE_STATUS HttpConn::paraseRequestHeader(string text)
{
    /**
     * 处理请求头，请求头格式如下：
     * 
     */
    size_t idx = 0;
    
    if(text.empty())
    {
        return LINE_OK;
    }
    else if((idx = text.find("Host:")) != string::npos)
    {
        idx = text.find_first_not_of(" ", idx + 1);

        if(idx == string::npos)
            return LINE_BAD;

        m_host = text.substr(idx);

    }
    else if((idx = text.find("Content-Length:")) != string::npos)
    {
        idx = text.find_first_not_of(" ", idx + 1);

        if(idx == string::npos)
            return LINE_BAD;

        content_length = std::stoi(text.substr(idx));
        if(content_length < 0)
        {
            return LINE_BAD;
        }
    }
    else if ((idx = text.find("Connection:")) != string::npos) 
    {
        idx = text.find_first_not_of(" ", idx + 1);

        if(idx == string::npos)
            return LINE_BAD;

        string tmp = text.substr(idx);
        isKeepLive = tmp == "keep-alive" ? true : false;
    }
    else
    {
        // 其余字段不做处理
    }
    
    return LINE_OK;
}

HttpConn::LINE_STATUS HttpConn::paraseRequestContent(string text)
{
    // 处理post提交的内容
    if(!text.empty())
        return LINE_OK;
}

HttpConn::HTTP_CODE HttpConn::do_request()
{
    auto idx = m_url.find_last_of("/");
    if(idx == string::npos)
    {
        return BAD_REQUEST;
    }

    if(m_url == "/")
    {
        fileName = "index.html";
    }
    else
    {
        fileName = m_url.substr(idx + 1);
    }

    string realPath = rootPath + fileName;

    // 检查是否是文件，并且具有权限可以读。
    int ret = stat(realPath.c_str(), &fileInfo);
    if(ret == -1)
    {
        return BAD_REQUEST;
    }

    // 检查是否为普通文件（非目录、管道等）
    if (!S_ISREG(fileInfo.st_mode)) 
    {
        return BAD_REQUEST; // 不是普通文件
    }

    // 获得文件描述符
    int fd = open(realPath.c_str(), O_RDONLY);
    if(fd == -1)
    {
        return INTERNAL_ERROR;
    }

    // 把文件内容映射到内存中
    fileAddr = static_cast<char*>(mmap(nullptr, fileInfo.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    
    // 关闭文件描述符
    close(fd);

    return GET_REQUEST;
}

void HttpConn::unmap()
{
    if(fileAddr)
    {
        munmap(fileAddr, fileInfo.st_size);
    }
}

bool HttpConn::addResponse(const char* format, ...)
{
    va_list list;
    
}

bool HttpConn::addStatuLine(HTTP_CODE code)
{
    if(code == GET_REQUEST)
    {
        return addResponse("%s %d %s\r\n", m_version, 200, "OK");
    }
    else if (code == BAD_REQUEST)
    {
        return addResponse("%s %d %s\r\n", m_version, 404, "Not Found");
    }
    else if (code == FORBIDDEN_REQUEST)
    {
        return addResponse("%s %d %s\r\n", m_version, 403, "");
    }
    else
    {

    }
    
    return false;
}

bool HttpConn::addHeader(int len)
{
    addContentLen(len);
    addIsKeepLive();
    addBlankLine();
}

bool HttpConn::addContentLen(int len)
{
    return addResponse("Content-Length: %d\r\n", fileInfo.st_size);
}

bool HttpConn::addIsKeepLive()
{
    string conn = (isKeepLive ? "keep-alive" : "closed");
    return addResponse("Connection: %s\r\n", conn);
}

bool HttpConn::addBlankLine()
{
    return addResponse("\r\n");
}


bool HttpConn::readFromClnt()
{
    int len = 0;
    while(true)
    {
        len = recv(sockfd, readBuffer + readIdx, READ_BUFF_SIZE - readIdx, 0);

        if(len == -1)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 读空了
                break;
            }
            return false;
        }
        else if(len == 0)
        {
            // 关闭连接
            closeConn();
            break;
        }
        else if(len > 0)
        {
            readIdx += len;
        }
    }

    return true;
}

bool HttpConn::writeToClnt()
{
    int ret = writev(sockfd, iov, iovCount);
    if(ret == -1)
    {
        return false;
    }

    return true;
}


void HttpConn::closeConn(bool isClose)
{
    if(isClose)
    {
        removeFd(epollfd, sockfd);
        close(sockfd);
    }

    init();
}

HttpConn::HTTP_CODE HttpConn::processRead()
{
    
    curState = CHECK_REQUESTLINE;
    LINE_STATUS curLineStatu = LINE_OPEN;
    
    char* text = nullptr;

    while(curState != CHECK_CONTENT || (curLineStatu = paraseLine()) == LINE_OK)
    {
        text = getOneLine();
        switch (curState)
        {
            case CHECK_REQUESTLINE :
            {
                curLineStatu = paraseRequestLine(string(text));
                if(curLineStatu == LINE_OK)
                {
                    curState = CHECK_HEADER;
                }
                else if(curLineStatu == LINE_BAD)
                {
                    return BAD_REQUEST;
                }

            }

            case CHECK_HEADER :
            {
                curLineStatu = paraseRequestHeader(string(text));
                
                if(curLineStatu == LINE_OK)
                {
                    curState = CHECK_CONTENT;
                    return do_request();
                }
                else if(curLineStatu == LINE_BAD)
                {
                    return BAD_REQUEST;
                }

            }

            case CHECK_CONTENT :
            {
                curLineStatu = paraseRequestContent(string(text));
                
                if(curLineStatu == LINE_OK)
                {
                    curState = CHECK_HEADER;

                    return do_request();
                }
                else if(curLineStatu == LINE_BAD)
                {
                    return BAD_REQUEST;
                }
            }
        }

    }

    return BAD_REQUEST;
}

void HttpConn::process()
{
    HTTP_CODE code = processRead();

    processWrite(code);
}

bool HttpConn::processWrite(HTTP_CODE code)
{
    addStatuLine(code);
    addHeader(fileInfo.st_size);
    iov[0].iov_base = writeBuffer;
    iov[0].iov_len = writeIdx;
    iov[1].iov_base = fileAddr;
    iov[1].iov_len = fileInfo.st_size;
    iovCount = 2;

    return true;

}