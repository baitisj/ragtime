#include "ragtime/radio/PttFollower.h"

namespace ragtime
{
namespace radio
{

void PttFollower::setEnabled(bool enabled)
{
    if (enabled == enabled_) return;
    enabled_ = enabled;
    held_ = false;
    if (!locked_) why_.clear();
    if (!enabled_) set(false);
}

void PttFollower::host(bool on, uint64_t nowMs)
{
    if (!on)
    {
        held_ = false;
        if (!locked_) why_.clear();
        set(false);
        return;
    }
    if (!enabled_ || held_ || locked_ || keyed_) return;
    keyedMs_ = nowMs;
    why_.clear();
    set(true);
}

void PttFollower::hostLost()
{
    // A host that comes back starts unkeyed: no PTT OFF to wait for.
    held_ = false;
    if (!keyed_ || locked_) return;
    why_ = "the connection to data2g-host dropped";
    set(false);
}

void PttFollower::rigFailed(const std::string& why)
{
    if (!keyed_) return;
    held_ = true;
    why_ = why.empty() ? std::string("the rig failed") : why;
    set(false);
}

void PttFollower::tick(uint64_t nowMs)
{
    if (!keyed_ || nowMs < keyedMs_ + MAX_KEYED_MS) return;
    held_ = true;
    why_ = "data2g-host kept PTT on for " + std::to_string(MAX_KEYED_MS / 1000) + " s";
    set(false);
}

void PttFollower::lockOut(const std::string& why)
{
    locked_ = true;
    why_ = why;
    set(false);
    version_++;
}

void PttFollower::clearLockOut()
{
    if (!locked_) return;
    locked_ = false;
    why_.clear();
    version_++;
}

void PttFollower::set(bool keyed)
{
    if (keyed == keyed_) return;
    keyed_ = keyed;
    version_++;
}

} // namespace radio
} // namespace ragtime
