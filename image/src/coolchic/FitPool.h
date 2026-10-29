// A small pool of threads for the encoder's loops over rows: run(n, f)
// splits 0..n into one contiguous share per thread and calls
// f(begin, end, thread) on each, the caller's thread taking the first.
#ifndef RAGTIME_IMAGE_COOLCHIC_FIT_POOL_H
#define RAGTIME_IMAGE_COOLCHIC_FIT_POOL_H

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

class Pool
{
public:
    // threads <= 0: one per core.
    explicit Pool(int threads = 0);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    int threads() const { return threads_; }
    void run(int n, const std::function<void(int, int, int)>& f);

private:
    void worker(int index);

    int threads_ = 1;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable start_;
    std::condition_variable done_;
    const std::function<void(int, int, int)>* job_ = nullptr;
    int n_ = 0;
    uint64_t generation_ = 0;
    int pending_ = 0;
    bool stop_ = false;
};

} // namespace coolchic
} // namespace image
} // namespace ragtime

#endif
