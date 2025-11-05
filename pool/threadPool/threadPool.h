#ifndef THREADPOOL_H
#define THREADpOOL_H

#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unistd.h>
#include <thread>
#include <vector>

#include "../../constance.h"
#include "../sqlConnPool/connPoolRAII.h"

using std::cout;
using std::endl;

template<typename T>
class ThreadPool
{

public:
    ThreadPool(int min, int max, SqlConnPool* connPool);
    ~ThreadPool();

    /* 任务队列有关函数 */
    T* getTask();
    bool addTask(T *);
    int getSize();

    /* 工作线程有关函数 */
    int getBusyNum();
    int getAliveNum();
    int getExitNum();


private:
    /* 线程函数 */
    static void work(void*);
    static void manager(void*);

private:

    /* 线程池参数*/
    int maxNum;     // 最大线程数量
    int minNum;     // 最小线程数量
    int busyNum;    // 工作的数量
    int aliveNum;   // 存在的线程数量
    int exitNum;    // 退出的线程数量

    /* 队列参数 */
    std::mutex queueMutex;      // 队列锁
    std::queue<T*> taskQueue;    // 任务队列

    /* 线程 */
    std::vector<std::thread> workThread;    // 工作线程
    std::thread managerThread;              // 管理者线程

    /* 锁与条件变量 */
    std::mutex poolMutex;               // 池锁
    std::condition_variable notEmpty;   // 任务队列

    bool isStop;

    static const int STEP = 2;  // 每次增加线程的个数
    SqlConnPool* connPool;

};

template<typename T>
ThreadPool<T>::ThreadPool(int min, int max, SqlConnPool* connPool)
{
    
    #ifdef debug
        cout << "Thread pool startup !!!" << endl;
    #endif
    

    maxNum = max;
    busyNum = 0;
    exitNum = 0;
    isStop = false;
    minNum = min;
    this->connPool = connPool;

    for(int i = 0; i < min; i++)
    {
        workThread.emplace_back(&ThreadPool<T>::work, this);
    }

    aliveNum = minNum;

    managerThread = std::thread(&ThreadPool<T>::manager, this);

}

template<typename T>
ThreadPool<T>::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // 清理任务队列
        while (!taskQueue.empty()) {
            delete taskQueue.front();
            taskQueue.pop();
        }
    }

    isStop = true;
    notEmpty.notify_all();

    for(auto& td : workThread)
    {
        if(td.joinable())
        {
            td.join();
        }
    }

    // 等待管理者进程结束
    managerThread.join();

    workThread.clear();

    #ifdef debug
        cout << "thread pool finish !!!" << endl;
    #endif


}

template<typename T>
T* ThreadPool<T>::getTask()
{
    if(getSize() == 0)
        return nullptr;
    
    std::lock_guard<std::mutex> lock(queueMutex);
    T* task = taskQueue.front();
    taskQueue.pop();

    return task;
}

template<typename T>
bool ThreadPool<T>::addTask(T* task)
{
    if(!isStop)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        taskQueue.push(task);
        notEmpty.notify_one();
    
        return true;
    }

    return false;
}

template<typename T>
int ThreadPool<T>::getSize()
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return taskQueue.size();
}

template<typename T>
int ThreadPool<T>::getBusyNum()
{
    std::lock_guard<std::mutex> lock(poolMutex);
    return busyNum;
}

template<typename T>
int ThreadPool<T>::getAliveNum()
{
    std::lock_guard<std::mutex> lock(poolMutex);
    return aliveNum;
}

template<typename T>
int ThreadPool<T>::getExitNum()
{
    std::lock_guard<std::mutex> lock(poolMutex);
    return exitNum;
}

template<typename T>
void ThreadPool<T>::work(void* arg)
{
    ThreadPool<T>* pool = static_cast<ThreadPool<T>*>(arg);

    while(true)
    {
        int aliveCnt = pool->getAliveNum();
        int busyCnt = pool->getBusyNum();
        int exitCnt = pool->getExitNum();

        while(pool->getSize() == 0 && !pool->isStop)
        {
            std::unique_lock<std::mutex> lock(pool->poolMutex);
            pool->notEmpty.wait(lock);
            if(exitCnt > 0 && !pool->isStop)
            {
                if(aliveCnt - 1 > pool->minNum)
                {
                    #ifdef debug
                        cout << "thread: " << std::this_thread::get_id() << " closed !!!" << endl;
                    #endif

                    --pool->exitNum;
                    --pool->aliveNum;

                    lock.unlock();
                    return;
                }
            }
            
        }

        if(pool->isStop)
        {
            #ifdef debug
                cout << "thread: " << std::this_thread::get_id() << " closed !!!" << endl;
            #endif
            return;
        }

        T* task = pool->getTask();
        
        pool->poolMutex.lock();
        ++pool->busyNum;
        pool->poolMutex.unlock();

        SqlConnRAII sqlConn(&task->mysql, connPool);
        // 任务的执行函数
        task->process();

        pool->poolMutex.lock();
        --pool->busyNum;
        pool->poolMutex.unlock();
    }

}

template<typename T>
void ThreadPool<T>::manager(void* arg)
{
    ThreadPool<T>* pool = static_cast<ThreadPool<T>*>(arg);

    while(!pool->isStop)
    {
        // 每隔3s检查一下
        std::this_thread::sleep_for(std::chrono::seconds(3));

        int busy = pool->getBusyNum();
        int alive = pool->getAliveNum();

        if(pool->getSize() > busy && alive < pool->maxNum && !pool->isStop)
        {
            std::lock_guard<std::mutex> lock(pool->poolMutex);
            for(int i = 0; i < STEP && pool->aliveNum < pool->maxNum; ++i)
            {
                ++pool->aliveNum;
                pool->workThread.emplace_back(&ThreadPool<T>::work, pool);
            }

            #ifdef debug
                cout << "add thread !!!" << endl;
            #endif
        }
        else if(busy * 2 < alive && !pool->isStop)
        {
            std::lock_guard<std::mutex> lock(pool->poolMutex);
            for(int i = 0; i < STEP && pool->aliveNum > pool->minNum; ++i)
            {
                ++pool->exitNum;
                pool->notEmpty.notify_one();
            }
        }

    }

}

#endif
