#ifndef THREADPOOL_H
#define THREADPOOL_H

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
    SqlConnPool* connsPool;

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
    connsPool = connPool;

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
    // 现在 getTask 只在持有 queueMutex 的情况下被调用（或者自己加锁）
    std::lock_guard<std::mutex> lock(queueMutex);
    if (taskQueue.empty())
        return nullptr;
    T* task = taskQueue.front();
    taskQueue.pop();
    return task;
}


template<typename T>
bool ThreadPool<T>::addTask(T* task)
{
    if (isStop) return false;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        taskQueue.push(task);
    }
    // notify after releasing queueMutex (notify can be outside, but safe either way)
    notEmpty.notify_one();
    return true;
}

template<typename T>
int ThreadPool<T>::getSize()
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return (int)taskQueue.size();
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

    while (true)
    {
        T* task = nullptr;
        {   // 作用域：操作队列的临界区
            std::unique_lock<std::mutex> qlock(pool->queueMutex);

            // 等待：队列非空 OR 线程池停止 OR 要求缩容（exitNum>0）
            pool->notEmpty.wait(qlock, [&]() {
                return !pool->taskQueue.empty() || pool->isStop || pool->getExitNum() > 0;
            });

            // 如果要停止，直接退出
            if (pool->isStop) 
            {
#ifdef debug
                cout << "thread: " << std::this_thread::get_id() << " closed (stop) !!!" << endl;
#endif
                return;
            }

            // 缩容逻辑：若管理线程设置了 exitNum，并且当前线程数 > minNum，则自己退出
            if (pool->getExitNum() > 0) 
            {
                std::lock_guard<std::mutex> lock(pool->poolMutex);
                if (pool->aliveNum > pool->minNum) {
                    --pool->exitNum;
                    --pool->aliveNum;
#ifdef debug
                    cout << "thread: " << std::this_thread::get_id() << " closed (shrink) !!!" << endl;
#endif
                    return;
                }
            }

            // 从队列取一个任务（确保在 queueMutex 下）
            if (!pool->taskQueue.empty()) 
            {
                task = pool->taskQueue.front();
                pool->taskQueue.pop();
            }
        } // 释放 queueMutex

        if (task == nullptr) 
        {
            // 可能由于缩容/stop 等原因，没有任务，继续循环
            continue;
        }

        {   // 增加 busyNum（保护 poolMutex）
            std::lock_guard<std::mutex> lock(pool->poolMutex);
            ++pool->busyNum;
        }

        // 执行任务：在此期间不应持有任何线程池级锁
        {
            // 防御性：确保 task->m_mysql 是可用地址，SqlConnRAII 构造内会 assert
            SqlConnRAII sqlConn(&task->m_mysql, pool->connsPool);
            task->process();
        }

        {   // 任务完成，减少 busyNum
            std::lock_guard<std::mutex> lock(pool->poolMutex);
            --pool->busyNum;
        }
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
