#ifndef SQLCONNPOOLRAII_H
#define SQLCONNPOOLRAII_H

#include "sqlConnPool.h"

class SqlConnRAII
{
public:
    SqlConnRAII(MYSQL** sql, SqlConnPool* connpool)
    {
        *sql = connpool->getConn();
        mysql = *sql;
        connPool = connpool; 
    }
    ~SqlConnRAII()
    {
        if(mysql)
        {
            connPool->freeCon(mysql);
        }
    }

private:
    MYSQL* mysql;
    SqlConnPool* connPool;

};

#endif
