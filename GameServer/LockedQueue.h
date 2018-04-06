#pragma once

#include <deque>
#include <mutex>

namespace Adoter
{

template <class T, typename StorageType = std::deque<T> >

class LockedQueue
{
private:

    std::mutex _lock;

    StorageType _queue;

    volatile bool _canceled;

public:

    LockedQueue() : _canceled(false)
    {
    }

    virtual ~LockedQueue()
    {
    }

    void Add(const T& item)
    {
        Lock();

        _queue.push_back(item);

        Unlock();
    }

    template<class Iterator>
    void AddRange(Iterator begin, Iterator end)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _queue.insert(_queue.begin(), begin, end);
    }

    bool Next(T& result)
    {
        std::lock_guard<std::mutex> lock(_lock);

        if (_queue.empty()) return false;

        result = _queue.front();
        _queue.pop_front();

        return true;
    }

    template<class Checker>
    bool Next(T& result, Checker& check)
    {
        std::lock_guard<std::mutex> lock(_lock);

        if (_queue.empty()) return false;

        result = _queue.front();

        if (!check.Process(result)) return false;

        _queue.pop_front();
        return true;
    }

    T& Peek(bool auto_unlock = false)
    {
        Lock();

        T& result = _queue.front();

        if (auto_unlock) Unlock();

        return result;
    }

    void Cancel()
    {
        std::lock_guard<std::mutex> lock(_lock);

        _canceled = true;
    }

    bool Cancelled()
    {
        std::lock_guard<std::mutex> lock(_lock);

        return _canceled;
    }

    void Lock()
    {
        this->_lock.lock();
    }

    void Unlock()
    {
        this->_lock.unlock();
    }

    void PopFront()
    {
        std::lock_guard<std::mutex> lock(_lock);
        _queue.pop_front();
    }

    bool Empty()
    {
        std::lock_guard<std::mutex> lock(_lock);
        return _queue.empty();
    }
};

}
