#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <memory>

class ThreadPool
{
public:
    explicit ThreadPool(int threadCnt = 4);
    ~ThreadPool();

    // 禁用拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    
    template <typename F>
    void addTask(F&& task)
    {
        {
            std::lock_guard<std::mutex> lock(pool->poolMutex);
            pool->tasksQueue.emplace(std::forward<F>(task));
        }
        pool->cond.notify_one();
    }

private:
    struct Pool
    {
        std::mutex poolMutex;
        std::condition_variable cond;
        bool isStop = false;
        std::queue<std::function<void()>> tasksQueue;
    };

    std::shared_ptr<Pool> pool;
    std::vector<std::thread> threads;
};

// 构造函数实现
inline ThreadPool::ThreadPool(int cnt) : pool(std::make_shared<Pool>())
{
    for (int i = 0; i < cnt; ++i)
    {
        threads.emplace_back([_pool = pool]
        {
            std::unique_lock<std::mutex> lock(_pool->poolMutex);
            while (true)
            {
                if (!_pool->tasksQueue.empty())
                {
                    auto task = std::move(_pool->tasksQueue.front());
                    _pool->tasksQueue.pop();
                    lock.unlock();
                    task();
                    lock.lock();
                }
                else if (_pool->isStop)
                    break;
                else
                    _pool->cond.wait(lock);
            }
        });
    }
}

// 析构函数实现
inline ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(pool->poolMutex);
        pool->isStop = true;
    }
    pool->cond.notify_all();

    for (auto& t : threads)
        if (t.joinable())
            t.join();
}

#endif
