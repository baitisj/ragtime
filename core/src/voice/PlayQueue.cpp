#include "ragtime/voice/PlayQueue.h"

#include <algorithm>

namespace ragtime
{
namespace voice
{

std::optional<PlayQueue::Start> PlayQueue::arrived(const std::string& key, bool autoPlay)
{
    if (key.empty() || !seen_.insert(key).second) return std::nullopt;
    if (!autoPlay || key == playing_) return std::nullopt;
    waiting_.push_back(key);
    if (!playing_.empty() || held_) return std::nullopt;
    return next();
}

PlayQueue::Start PlayQueue::play(const std::string& key, uint32_t fromFrame)
{
    seen_.insert(key);
    waiting_.erase(std::remove(waiting_.begin(), waiting_.end(), key), waiting_.end());
    playing_ = key;
    pausedAt_ = 0;
    // Asked for while Push to talk is held: it plays, and letting go
    // later carries on with the queue rather than resume anything.
    return Start{key, fromFrame};
}

void PlayQueue::stop()
{
    playing_.clear();
    pausedAt_ = 0;
    waiting_.clear();
}

std::optional<PlayQueue::Start> PlayQueue::finished(const std::string& key)
{
    if (key.empty() || key != playing_) return std::nullopt;
    // Paused by Push to talk: the sound was stopped for the pause, not
    // played out, so it carries on at release.
    if (held_) return std::nullopt;
    playing_.clear();
    pausedAt_ = 0;
    return next();
}

bool PlayQueue::hold(uint32_t atFrame)
{
    if (held_) return false;
    held_ = true;
    if (playing_.empty()) return false;
    pausedAt_ = atFrame;
    return true;
}

std::optional<PlayQueue::Start> PlayQueue::release()
{
    if (!held_) return std::nullopt;
    held_ = false;
    if (!playing_.empty()) return Start{playing_, pausedAt_};
    return next();
}

void PlayQueue::clearWaiting()
{
    waiting_.clear();
}

bool PlayQueue::forget(const std::string& key)
{
    waiting_.erase(std::remove(waiting_.begin(), waiting_.end(), key), waiting_.end());
    if (key != playing_ || key.empty()) return false;
    playing_.clear();
    pausedAt_ = 0;
    return true;
}

bool PlayQueue::waiting(const std::string& key) const
{
    return std::find(waiting_.begin(), waiting_.end(), key) != waiting_.end();
}

std::optional<PlayQueue::Start> PlayQueue::next()
{
    if (waiting_.empty()) return std::nullopt;
    playing_ = waiting_.front();
    waiting_.pop_front();
    pausedAt_ = 0;
    return Start{playing_, 0};
}

} // namespace voice
} // namespace ragtime
