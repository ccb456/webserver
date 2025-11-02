#include "timerHeap.h"

inline HeapTimer::HeapTimer()
{
    m_heap.resize(64);
}

inline HeapTimer::~HeapTimer()
{
    clear();
}

void HeapTimer::shiftUp(size_t i)
{
    assert(i >= 0 && i < m_heap.size());

    size_t j = (i - 1) / 2;

    while(j >= 0)
    {
        if(m_heap[j] < m_heap[i])
        {
            break;
        }

        swapNode(i, j);
        i = j;
        j = (i - 1) / 2;
    }

}

void HeapTimer::swapNode(size_t i, size_t j)
{
    assert(i >= 0 && i < m_heap.size());
    assert(j >= 0 && j < m_heap.size());

    std::swap(m_heap[i], m_heap[j]);
    ref[m_heap[i].id] = i;
    ref[ m_heap[j].id] = j;
}

bool HeapTimer::shitfDown(size_t idx, size_t n)
{
    assert(idx >= 0 && idx < m_heap.size());
    assert(n >= 0 && n < m_heap.size());

    size_t i = idx;
    size_t j = i * 2 + 1;
    
    while(j < n)
    {
        if(j + 1 < n && m_heap[j + 1] < m_heap[j]) ++j;

        if(m_heap[i] < m_heap[j]) break;
        swapNode(i, j);
        i = j;
        j = i * 2 + 1;
    }

    return i > idx;
}

void HeapTimer::add(int id, int timeout, const TimeoutCallBack& cb)
{
    assert(id >= 0);

    size_t n;

    /* 新的定时任务 */
    if(ref.count(id) == 0)
    {
        n = m_heap.size();
        ref[id] = n;
        m_heap.emplace_back(id, Clock::now() + MS(timeout), cb);
        shiftUp(n);
    }
    else
    {
        /* 旧的定时任务，调整堆 */
        n = ref[id];
        m_heap[n].expires = Clock::now() + MS(timeout);
        m_heap[n].cb = cb;

        if(!shitfDown(n, m_heap.size()))
        {
            shiftUp(n);
        }
    }
}

void HeapTimer::doWork(int id)
{
    /* 删除指定的定时器， 并执行回调函数 */
    if(m_heap.empty() || ref.count(id) == 0)
    {
        return;
    }

    size_t i = ref[id];
    TimerNode node = m_heap[i];
    node.cb();
    _del(i);
}

void HeapTimer::_del(size_t idx)
{
    assert(!m_heap.empty() && idx >= 0 && idx < m_heap.size());

    size_t i = idx;
    size_t n = m_heap.size() - 1;
    assert(i <= n);
    if(i < n)
    {
        swapNode(i, n);
        if(!shitfDown(i, n))
        {
            shiftUp(i);
        }
    }

    ref.erase(m_heap.back().id);
    m_heap.pop_back();
}

void HeapTimer::adjust(int id, int timeout)
{
    assert(!m_heap.size() && ref.count(id) > 0);
    m_heap[ref[id]].expires = Clock::now() + MS(timeout);

    shitfDown(ref[id], m_heap.size());
    
}

void HeapTimer::tick()
{
    /* 清除超时节点 */
    if(m_heap.empty())
    {
        return;
    }
    TimerNode node;
    while(!m_heap.empty())
    {
        node = m_heap.front();
        if(std::chrono::duration_cast<MS>(node.expires - Clock::now()).count() > 0)
        {
            break;
        } 

        node.cb();
        pop();
    }
}

void HeapTimer::pop()
{
    assert(!m_heap.empty());
    _del(0);
}

void HeapTimer::clear()
{
    ref.clear();
    m_heap.clear();
}

int HeapTimer::getNextTick()
{
    tick();
    int res = -1;
    if(!m_heap.empty())
    {
        res = std::chrono::duration_cast<MS>(m_heap.front().expires - Clock::now()).count();
        if(res < 0) 
        {
            res = 0;
        }
    }

    return res;
}