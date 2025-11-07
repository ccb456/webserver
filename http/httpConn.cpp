#include "httpConn.h"

// 定义http响应的一些状态信息
const string ok_200_title = "OK";
const string error_400_title = "Bad Request";
const string error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const string error_403_title = "Forbidden";
const string error_403_form = "You do not have permission to get file form this server.\n";
const string error_404_title = "Not Found";
const string error_404_form = "The requested file was not found on this server.\n";
const string error_500_title = "Internal Error";
const string error_500_form = "There was an unusual problem serving the request file.\n";


int HttpConn::epollfd = -1;
std::atomic_int HttpConn::userCount(0);

string rootPath;

/* 保存数据库中的用户信息 */
std::unordered_map<string, string> usersInfo;
std::mutex connMutex;


int setnoblocking(int fd)
{
    int oldOpt = fcntl(fd, F_GETFL);
    int newOpt = oldOpt | O_NONBLOCK;
    fcntl(fd, F_SETFL, newOpt);

    return oldOpt;
}

void addFd(int epollfd, int fd, bool isOneShot = false)
{
    epoll_event ev;
    ev.data.fd = fd;

    ev.events = EPOLLET | EPOLLIN | EPOLLRDHUP;
    if(isOneShot)
    {
        ev.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    setnoblocking(fd);
}

void removeFd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
    #ifdef debug
        std::cout << "fd: " << fd << " closed !!!" << std::endl;
    #endif
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);

}

void HttpConn::init()
{
        m_url = "";
        m_method = GET;
        m_version = "";
        m_host = "";
        content_length = 0;
        isKeepLive = false;
        isCGI = false;
       
        memset(readBuffer, 0, READ_BUFF_SIZE);
        memset(writeBuffer, 0, WRITE_BUFF_SIZE);

        readIdx = 0;
        writeIdx = 0;
        curIdx = 0;

        lineIdx = 0;

       
        filePath = "";        
        fileAddr = nullptr;  
        m_mysql = nullptr; 

        iovCount = 0;
        bytesHaveSend = 0;
        bytesToSend = 0;

        curState = CHECK_REQUESTLINE;
}

void HttpConn::init(const int m_sockfd, const sockaddr_in addr) 
{
    sockfd = m_sockfd;
    clntAddr = addr;

    addFd(epollfd, sockfd, true);
    ++userCount;

    #ifdef debug
        std::cout << "connection the client: " << inet_ntoa(addr.sin_addr) << std::endl;
    #endif

    init();
}

/* 初始化mysql结果 */
void HttpConn::initMySQLResult(SqlConnPool* connPool)
{
    SqlConnRAII conn(&m_mysql, connPool);
    string sql = "select username,passwd FROM user";
    mysql_query(m_mysql, sql.c_str());

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(m_mysql);

    //返回结果集中的列数
    // int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        usersInfo[temp1] = temp2;
    }

    // 释放资源
    mysql_free_result(result);
    
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

        if(readBuffer[curIdx] == '\r')
        {
            /* 以\r 结尾 */
            if(curIdx + 1 == readIdx)
            {
                return LINE_OPEN;
            }
            else if(readBuffer[curIdx + 1] == '\n')
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
            if(curIdx > 1 && (readBuffer[curIdx - 1] == '\r'))
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

HttpConn::HTTP_CODE HttpConn::paraseRequestLine(string text)
{
    /**
     * 处理请求行
     * 请求行格式如下：
     *  GET /api/user/1001 HTTP/1.1
     */

    auto idx = text.find_first_of(" ");
    if(idx == string::npos)
    {
        return BAD_REQUEST;
    }

    string method = text.substr(0, idx);
    if (method.empty()) 
    {
        return BAD_REQUEST;
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
        return BAD_REQUEST; // 跳过空格后无内容（无URL），格式错误
    }

    // 查找URL结束位置（下一个空格）
    auto urlEnd = text.find_first_of(" ", idx);
    // 提取URL：如果没有下一个空格，说明URL到结尾（但HTTP请求行必须有版本，此处应视为错误）
    if (urlEnd == string::npos) 
    {
        return BAD_REQUEST; // 无版本信息，格式错误
    }
    m_url = text.substr(idx, urlEnd - idx);
    if (m_url.empty()) 
    {
        return BAD_REQUEST;
    }
    
    if(m_url == "/")
    {
        m_url = "/index.html";
    }

    // 跳过空格，定位到版本起始位置
    idx = text.find_first_not_of(" ", urlEnd + 1);
    if (idx == string::npos) 
    {
        return BAD_REQUEST; // 跳过空格后无版本内容，格式错误
    }

    // 提取版本（到字符串结尾）
    m_version = text.substr(idx);
    std::transform(m_version.begin(), m_version.end(), m_version.begin(), ::toupper);

    if(m_version != "HTTP/1.1" && m_version != "HTTP/1.0")
    {
        return BAD_REQUEST;
    }


    curState = CHECK_HEADER;
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::paraseRequestHeader(string text)
{
    /**
     * 处理请求头，请求头格式如下：
     * 
     */
    size_t idx = 0;
    
    if(text.empty())
    {
        if(content_length > 0)
        {
            curState = CHECK_CONTENT;

            return NO_REQUEST;
        }

        return GET_REQUEST;
    }

    idx = text.find_first_of(":");
    if(idx == string::npos)
    {
        return BAD_REQUEST;
    }

    string key = text.substr(0, idx);
    string value = text.substr(idx + 2);


    if(key =="Host")
    {
        m_host = value;
    }
    else if(key == "Content-Length")
    {

        content_length = std::stoi(value);
        
        if(content_length < 0)
        {
            return BAD_REQUEST;
        }
    }
    else if (key == "Connection") 
    {
        isKeepLive = (value == "keep-alive" ? true : false);
    }
    else
    {
        // 其余字段不做处理
    }
    
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::paraseRequestContent(string text)
{
    // 处理post提交的内容
    if(readIdx >= (curIdx + content_length))
    {
        if(!text.empty())
        {
            requestBody = text;
            return GET_REQUEST;
        }
    }

    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::do_request()
{
    char flag = 'a';
    string fileName = "";
    auto idx = m_url.find_last_of("/");

    if(idx == string::npos)
    {
        fileName = m_url;  
    }
    else 
    {
        if(idx + 1 < m_url.size())
        {
            flag = m_url[idx + 1];
        }
        // POST请求
        if((flag == '2' || flag == '3'))
        {

            // 将user=123&passwd=123提取出来
            auto nameIdx = requestBody.find_first_of("=");
            auto split = requestBody.find_first_of("&");
            auto pwdIdx = requestBody.find_last_of("=");
            string name = requestBody.substr(nameIdx + 1, split - nameIdx);
            string pwd = requestBody.substr(pwdIdx + 1);
            
            /* 注册 */
            if(flag == '3')
            {
                // 修正后的SQL拼接
                string sql = "insert into user(username, passwd) values('";
                sql += name + "', '" + pwd + "')";  // 补充逗号和单引号

                // 用户名已经存在了
                if(usersInfo.count(name))
                {
                    m_url = "/registerError.html";
                }
                /* 新用户 */
                else
                {
                    std::lock_guard<std::mutex> locker(connMutex);
                    int ret = mysql_query(m_mysql, sql.c_str());
                    usersInfo.insert(std::make_pair(name, pwd));

                    if(!ret)
                    {
                        m_url = "/log.html";
                    }
                    else
                    {
                        #ifdef debug
                            std::cout << " mysql_errno(m_mysql) : " << mysql_errno(m_mysql) << std::endl;
                        #endif
                        m_url = "/registerError.html";
                    }
                }
            }

            /* 登录 */
            else 
            {
                auto m_pwd = usersInfo.find(name);
                if(m_pwd != usersInfo.end() && m_pwd->second == pwd)
                {
                    m_url = "/welcome.html";
                }
                else
                {
                    m_url = "/logError.html";
                }
            }
        }

        // GET 请求
        switch (flag)
        {
            case '0':   // 注册界面   
            {
                fileName = "/register.html";
                break;
            }

            case '1':   // 登录界面
            {
                fileName = "/log.html";
                break;
            }
            case '5':
            {
                fileName = "/picture.html";
                break;
            }

            case '6' :
            {
                fileName = "/video.html";
                break;
            }

            case '7':
            {
                fileName = "/fans.html";
                break;
            }
                
            default:
            {
                fileName = m_url;
                break;
            }
        }

    }


    filePath = rootPath + fileName;
    #ifdef debug
        std::cout << "filePath: " << filePath << std::endl;
    #endif

    int ret = stat(filePath.c_str(), &fileInfo);
    if(ret == -1)
    {
        return NO_RESOURCE;
    }

    // 检查是否为普通文件（非目录、管道等）
    if (!S_ISREG(fileInfo.st_mode)) 
    {
        return BAD_REQUEST; // 不是普通文件
    }

    // 检查权限
    if(!(fileInfo.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }


    // 获得文件描述符
    int fd = open(filePath.c_str(), O_RDONLY);
    if(fd == -1)
    {
        return INTERNAL_ERROR;
    }

    // 把文件内容映射到内存中
    fileAddr = static_cast<char*>(mmap(nullptr, fileInfo.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    
    // 关闭文件描述符
    close(fd);

    return FILE_REQUETS;
}

void HttpConn::unmap()
{
    if(fileAddr)
    {
        munmap(fileAddr, fileInfo.st_size);
        fileAddr = nullptr;
    }
}

bool HttpConn::addResponse(const char* format, ...)
{
    if(writeIdx >= WRITE_BUFF_SIZE) return false;

    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(writeBuffer + writeIdx, WRITE_BUFF_SIZE - writeIdx - 1, format, arg_list);
    if(len >= (WRITE_BUFF_SIZE - writeIdx - 1))
    {
        va_end(arg_list);
        return false;
    }

    writeIdx += len;
    va_end(arg_list);

    return true;
}

bool HttpConn::addStatuLine(int code, string text)
{
    return addResponse("%s %d %s\r\n", m_version.c_str(), code, text.c_str());
}

bool HttpConn::addHeader(int len)
{
    return  addContentLen(len) && addIsKeepLive() && addBlankLine();
}

bool HttpConn::addContentLen(int len)
{
    return addResponse("Content-Length:%d\r\n", len);
}

bool HttpConn::addContentType()
{
    return addResponse("Content-Type:%s\r\n", "text/html; charset=UTF-8");
}

bool HttpConn::addIsKeepLive()
{
    string conn = (isKeepLive ? "keep-alive" : "closed");
    return addResponse("Connection: %s\r\n", conn.c_str());
}

bool HttpConn::addBlankLine()
{
    return addResponse("%s","\r\n");
}

bool HttpConn::addContent(string text)
{
    return addResponse("%s", text.c_str());
}


bool HttpConn::readFromClnt()
{
    if(readIdx >= READ_BUFF_SIZE) return false;

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
            // 客户端关闭连接了
            return false;
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
    int ret = 0;
    if(bytesToSend == 0)
    {
        modfd(epollfd, sockfd, EPOLLIN);
        init();
        return true;
    }

    while(true)
    {
        ret = writev(sockfd, iov, iovCount);
        if(ret == -1)
        {
            if(errno == EAGAIN)
            {
                modfd(epollfd, sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytesHaveSend += ret;
        bytesToSend -= ret;

        if(bytesHaveSend >= iov[0].iov_len)
        {
            iov[0].iov_len = 0;
            iov[1].iov_base = fileAddr + (bytesHaveSend - writeIdx);
            iov[1].iov_len = bytesToSend;
        }
        else
        {
            iov[0].iov_base = writeBuffer + bytesHaveSend;
            iov[0].iov_len = iov[0].iov_len - bytesHaveSend;
        }

        if(bytesToSend <= 0)
        {
            unmap();
            modfd(epollfd, sockfd, EPOLLIN);
            if(isKeepLive)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    }


    return true;
}


void HttpConn::closeConn(bool isClose)
{
    if(isClose && sockfd != -1)
    {
        removeFd(epollfd, sockfd);
        sockfd = -1;
        --userCount;
    }

}

HttpConn::HTTP_CODE HttpConn::processRead()
{
    HTTP_CODE code = NO_REQUEST;
    LINE_STATUS curLineStatu = LINE_OK;
    
    char* text = nullptr;

    while((curState == CHECK_CONTENT && curLineStatu == LINE_OK) || ((curLineStatu = paraseLine()) == LINE_OK)) 
    {
        text = getOneLine();
        lineIdx = curIdx;

        switch (curState)
        {
            case CHECK_REQUESTLINE :
            {
                code = paraseRequestLine(string(text));
                if(code == BAD_REQUEST)
                {
                    return BAD_REQUEST;

                }
 
                break;
            }

            case CHECK_HEADER :
            {
                code = paraseRequestHeader(string(text));
                
                if(code == GET_REQUEST)
                {
                    return do_request();
                }
                else if(code == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }

                break;
            }

            case CHECK_CONTENT :
            {
                code = paraseRequestContent(string(text));
                
                if(code == GET_REQUEST)
                {
                    return do_request();
                }
                curLineStatu = LINE_OPEN;

                break;

            }

            default :
            {
                return INTERNAL_ERROR;
            }
        }

    }

    return NO_REQUEST;
}

void HttpConn::process()
{
    HTTP_CODE code = processRead();

    if(code == NO_REQUEST)
    {
        modfd(epollfd, sockfd, EPOLLIN);
        return;
    }

    bool ret = processWrite(code);
    if(!ret)
    {
        closeConn();
    }

    modfd(epollfd, sockfd, EPOLLOUT);
}

bool HttpConn::processWrite(HTTP_CODE code)
{
    switch (code)
    {
    case INTERNAL_ERROR:
    {
        addStatuLine(500, error_500_title);
        addHeader(error_500_form.size());
        if(!addContent(error_500_form))
        {
            return false;
        }
        break;
    }

    case BAD_REQUEST:
    {
        addStatuLine(404, error_404_title);
        addHeader(error_404_form.size());
        if(!addContent(error_404_form))
        {
            return false;
        }

        break;
    }

    case FORBIDDEN_REQUEST:
    {
        addStatuLine(403, error_403_title);
        addHeader(error_403_form.size());
        if(!addContent(error_403_form))
        {
            return false;
        }

        break;
    }

    case FILE_REQUETS:
    {
        addStatuLine(200, ok_200_title);
        if(fileInfo.st_size != 0)
        {

            addHeader(fileInfo.st_size);
    
            iov[0].iov_base = writeBuffer;
            iov[0].iov_len = writeIdx;
    
            iov[1].iov_base = fileAddr;
            iov[1].iov_len = fileInfo.st_size;
            iovCount = 2;
            bytesToSend = writeIdx + fileInfo.st_size;
            return true;
        }
        else
        {
            const string ok_string = "<html><body></body></html>";
            addHeader(ok_string.size());
            if(!addContent(ok_string))
            {
                return false;
            }
        }
    }
    
    default:
        {
            return false;
        }
    }

    iov[0].iov_base = writeBuffer;
    iov[0].iov_len = writeIdx;
    iovCount = 1;
    bytesToSend = writeIdx;
    return true;
}