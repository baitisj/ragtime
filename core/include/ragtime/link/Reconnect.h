// When to try data2g-host again after a connection fails or drops: 2 s,
// then 4, 8, 15 and 30 s, and every 30 s from then on, so the connection
// bar can say "Reconnecting in 8 s" and a host that comes back is found
// quickly without a busy loop while it is away.
#ifndef RAGTIME_LINK_RECONNECT_H
#define RAGTIME_LINK_RECONNECT_H

#include <cstdint>

namespace ragtime
{
namespace link
{

class Reconnect
{
public:
    // A try failed, or a connection dropped: the next try waits.
    void failed(uint64_t nowMs);
    // Connected: the waits start again from the shortest.
    void connected();

    // A try is waiting, and its time has come.
    bool due(uint64_t nowMs) const { return waiting_ && nowMs >= atMs_; }
    bool waiting() const { return waiting_; }
    // Whole seconds until the next try, rounded up; 0 when due or none waits.
    int secondsLeft(uint64_t nowMs) const;
    // Tries that have failed in a row.
    int failures() const { return failures_; }
    // Taken: the try is under way.
    void trying() { waiting_ = false; }

    static uint64_t waitMs(int failures);

private:
    int failures_ = 0;
    bool waiting_ = false;
    uint64_t atMs_ = 0;
};

} // namespace link
} // namespace ragtime

#endif
