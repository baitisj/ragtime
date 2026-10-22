#include "ragtime/link/Reconnect.h"

namespace ragtime
{
namespace link
{

namespace
{

constexpr uint64_t WAITS_MS[] = {2000, 4000, 8000, 15000, 30000};
constexpr int WAIT_COUNT = (int)(sizeof(WAITS_MS) / sizeof(WAITS_MS[0]));

} // namespace

uint64_t Reconnect::waitMs(int failures)
{
    if (failures < 1) return 0;
    return WAITS_MS[failures > WAIT_COUNT ? WAIT_COUNT - 1 : failures - 1];
}

void Reconnect::failed(uint64_t nowMs)
{
    failures_++;
    waiting_ = true;
    atMs_ = nowMs + waitMs(failures_);
}

void Reconnect::connected()
{
    failures_ = 0;
    waiting_ = false;
}

int Reconnect::secondsLeft(uint64_t nowMs) const
{
    if (!waiting_ || nowMs >= atMs_) return 0;
    return (int)((atMs_ - nowMs + 999) / 1000);
}

} // namespace link
} // namespace ragtime
