#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <semaphore.h>
#include <assert.h>
#include <memory>

class SqlConnPool
{
public:
    static SqlConnPool* getInstance();

    MYSQL* getConn();
    void freeCon(MYSQL* conn);
    int getFreeConnCnt();

    void init(const std::string host, int port, const std::string user, 
        const std::string pwd, const std::string dbname, int connSize);
    
    void destroyConnPool();

private:
    SqlConnPool();
    ~SqlConnPool();


private:
    int maxConnCnt;
    int usingConnCnt;
    int freeConnCnt;

    std::queue<MYSQL*> connQueue;
    std::mutex mtx;
    sem_t semId;


};

#endif