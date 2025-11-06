#ifndef TIMERHEAP_H
#define TIMERHEAP_H

#include <queue>
#include <unordered_map>
#include <ctime>
#include <algorithm>
#include <functional>
#include <cassert>
#include <chrono>

using TimeoutCallBack = std::function<void()>;
using Clock = std::chrono::high_resolution_clock;
using MS = std::chrono::milliseconds;
using TimeStamp = Clock::time_point;


// 定时器节点类型
struct TimerNode
{
    int id;
    TimeStamp expires;
    TimeoutCallBack cb;

    bool operator < (const TimerNode& t)
    {
        return expires < t.expires;
    }
};

// 定时器类型
class HeapTimer
{
public:
    HeapTimer();
    ~HeapTimer();

    void adjust(int id, int newExpires);
    void add(int id, int timeout, const TimeoutCallBack& cb);
    void doWork(int id);
    void clear();
    void tick();
    void pop();
    int getNextTick();

private:
    void _del(size_t i);
    void shiftUp(size_t i);
    bool shiftDown(size_t idx, size_t n);
    void swapNode(size_t i, size_t j);

private:
    std::vector<TimerNode> m_heap;
    std::unordered_map<int, size_t> ref;
};


#endif // TIMERHEAP_H
