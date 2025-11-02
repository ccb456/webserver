#include "sqlConnPool.h"

inline SqlConnPool::SqlConnPool() :usingConnCnt(0), freeConnCnt(0), maxConnCnt(8)
{    
}

SqlConnPool* SqlConnPool::getInstance()
{
    static SqlConnPool connPool;
    return &connPool;
}

void SqlConnPool::init(const std::string host, int port, const std::string user, 
        const std::string pwd, const std::string dbname, int connSize)
{
    assert(connSize > 0);

    for(int i = 0; i < connSize; ++i)
    {
        MYSQL* mysql = nullptr;
        mysql = mysql_init(mysql);

        if(!mysql)
        {
            /* 创建mysql连接失败 */
            assert(mysql);
        }

        /* 连接具体的数据库 */
        mysql = mysql_real_connect(mysql, host.c_str(), user.c_str(), pwd.c_str(), dbname.c_str(), port, nullptr, 0);

        if(!mysql)
        {
            /* 连接失败 */
            assert(mysql);
        }

        connQueue.push(mysql);
    }

    maxConnCnt = connSize;
    sem_init(&semId, 0, maxConnCnt);
}

MYSQL* SqlConnPool::getConn()
{
    MYSQL* mysql = nullptr;
    if(connQueue.empty())
    {
        return nullptr;
    }
    sem_wait(&semId);

    {
        std::lock_guard<std::mutex> locker(mtx);
        mysql = connQueue.front();
        connQueue.pop();
    }

    return mysql;
}

void SqlConnPool::freeCon(MYSQL* mysql)
{
    assert(mysql);
    std::lock_guard<std::mutex> locker(mtx);
    connQueue.push(mysql);
    sem_post(&semId);
}

void SqlConnPool::destroyConnPool()
{
    std::lock_guard<std::mutex> locker(mtx);

    while(!connQueue.empty())
    {
        auto item = connQueue.front();
        connQueue.pop();
        mysql_close(item);
    }

    mysql_library_end();
}

int SqlConnPool::getFreeConnCnt()
{
    std::lock_guard<std::mutex> locker(mtx);
    return connQueue.size();
}

SqlConnPool::~SqlConnPool()
{
    destroyConnPool();
}