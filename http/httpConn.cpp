#include "httpConn.h"

int HttpConn::epollfd = 0;
int HttpConn::userCount = 0;

void HttpConn::init()
{
        m_method = "";
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
    
}