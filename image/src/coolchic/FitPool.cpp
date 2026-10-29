#include "FitPool.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ragtime
{
namespace image
{
namespace coolchic
{

namespace
{

void share(int n, int threads, int index, int& begin, int& end)
{
    begin = (int)((int64_t)n * index / threads);
    end = (int)((int64_t)n * (index + 1) / threads);
}

} // namespace

Pool::Pool(int threads)
{
    if (threads <= 0)
        threads = (int)std::max(1u, std::thread::hardware_concurrency());
    threads_ = std::min(threads, 64);
    for (int i = 1; i < threads_; i++)
        workers_.emplace_back([this, i]() { worker(i); });
}

Pool::~Pool()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    start_.notify_all();
    for (std::thread& t : workers_)
        t.join();
}

void Pool::run(int n, const std::function<void(int, int, int)>& f)
{
    if (n <= 0)
        return;
    if (threads_ == 1 || n == 1)
    {
        f(0, n, 0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_ = &f;
        n_ = n;
        pending_ = threads_ - 1;
        generation_++;
    }
    start_.notify_all();
    int begin, end;
    share(n, threads_, 0, begin, end);
    if (begin < end)
        f(begin, end, 0);
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this]() { return pending_ == 0; });
    job_ = nullptr;
}

void Pool::worker(int index)
{
    uint64_t seen = 0;
    for (;;)
    {
        const std::function<void(int, int, int)>* job;
        int n;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            start_.wait(lock, [&]() { return stop_ || generation_ != seen; });
            if (stop_)
                return;
            seen = generation_;
            job = job_;
            n = n_;
        }
        int begin, end;
        share(n, threads_, index, begin, end);
        if (begin < end)
            (*job)(begin, end, index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_--;
            if (pending_ == 0)
                done_.notify_one();
        }
    }
}

} // namespace coolchic
} // namespace image
} // namespace ragtime
