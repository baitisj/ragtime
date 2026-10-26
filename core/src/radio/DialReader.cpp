#include "ragtime/radio/DialReader.h"

#include <cstdlib>
#include <string>

namespace ragtime
{
namespace radio
{

namespace
{

// A frequency line: digits, perhaps with a decimal part ("14074000" or,
// from some Hamlib versions, "14074000.000000").
bool frequencyLine(const std::string& line, uint64_t& hz)
{
    if (line.empty() || line.size() > 24) return false;
    size_t digits = 0;
    bool point = false;
    for (char c : line)
    {
        if (c >= '0' && c <= '9')
        {
            if (!point) digits++;
        }
        else if (c == '.' && !point)
        {
            point = true;
        }
        else
        {
            return false;
        }
    }
    if (digits == 0 || digits > 12) return false;
    double value = std::strtod(line.c_str(), nullptr);
    if (value < 1.0) return false;
    hz = (uint64_t)(value + 0.5);
    return true;
}

} // namespace

void DialReader::connected(uint64_t nowMs)
{
    connected_ = true;
    waiting_ = false;
    partial_.clear();
    out_.clear();
    problem_.clear();
    version_++;
    // First: was rigctld started with --vfo? Asked even while keyed, as
    // it touches nothing but rigctld itself.
    vfo_ = Vfo::Asking;
    out_ += "\\chk_vfo\n";
    waiting_ = true;
    askedMs_ = nowMs;
}

void DialReader::disconnected()
{
    connected_ = false;
    waiting_ = false;
    partial_.clear();
    out_.clear();
    hz_.reset();
    version_++;
}

void DialReader::ask(uint64_t nowMs)
{
    if (!connected_ || keyed_ || vfo_ == Vfo::Asking) return;
    // Frequency queries only: nothing here sets the radio.
    out_ += vfo_ == Vfo::Yes ? "f currVFO\n" : "f\n";
    waiting_ = true;
    askedMs_ = nowMs;
}

std::string DialReader::takeOutput()
{
    std::string out;
    out.swap(out_);
    return out;
}

void DialReader::tick(uint64_t nowMs)
{
    if (!connected_) return;
    if (vfo_ == Vfo::Asking)
    {
        // No answer to \chk_vfo: plain "f" it is.
        if (nowMs >= askedMs_ + ANSWER_MS) vfoAnswer(std::string(), nowMs);
        return;
    }
    if (keyed_) return;
    if (hz_ && nowMs > readMs_ + STALE_MS)
    {
        hz_.reset();
        problem_ = "rigctld hasn't answered for " + std::to_string(STALE_MS / 1000) + " s";
        version_++;
    }
    if (waiting_ ? nowMs >= askedMs_ + ANSWER_MS : nowMs >= askedMs_ + ASK_MS) ask(nowMs);
}

void DialReader::setKeyed(bool keyed, uint64_t nowMs)
{
    if (keyed_ == keyed) return;
    keyed_ = keyed;
    // The reading from before the burst stands through it, and for a
    // moment after while the next answer comes.
    if (!keyed_)
    {
        if (hz_) readMs_ = nowMs;
        if (vfo_ == Vfo::Asking) return;
        waiting_ = false;
        ask(nowMs);
    }
}

void DialReader::bytes(const char* bytes, size_t length, uint64_t nowMs)
{
    for (size_t i = 0; i < length; i++)
    {
        char c = bytes[i];
        if (c == '\n')
        {
            std::string line;
            line.swap(partial_);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            onLine(line, nowMs);
        }
        else if (partial_.size() < 256)
        {
            partial_ += c;
        }
    }
}

void DialReader::vfoAnswer(const std::string& line, uint64_t nowMs)
{
    // "1" or "0"; "CHKVFO 1" from Hamlib 3; an RPRT, or nothing, from a
    // rigctld that doesn't know the question.
    std::string answer = line.compare(0, 7, "CHKVFO ") == 0 ? line.substr(7) : line;
    vfo_ = answer == "1" ? Vfo::Yes : Vfo::No;
    waiting_ = false;
    version_++;
    ask(nowMs);
}

void DialReader::onLine(const std::string& line, uint64_t nowMs)
{
    if (line.empty()) return;
    if (vfo_ == Vfo::Asking)
    {
        vfoAnswer(line, nowMs);
        return;
    }
    uint64_t hz = 0;
    if (frequencyLine(line, hz))
    {
        waiting_ = false;
        readMs_ = nowMs;
        if (hz_ != hz || !problem_.empty()) version_++;
        hz_ = hz;
        problem_.clear();
        return;
    }
    if (line.compare(0, 4, "RPRT") == 0)
    {
        waiting_ = false;
        // "RPRT 0" only follows a set command, which we never send.
        if (line == "RPRT 0") return;
        if (problem_ != "rigctld answered " + line)
        {
            problem_ = "rigctld answered " + line;
            version_++;
        }
    }
}

std::optional<uint64_t> DialReader::dialHz(uint64_t nowMs) const
{
    if (!connected_ || !hz_) return std::nullopt;
    if (!keyed_ && nowMs > readMs_ + STALE_MS) return std::nullopt;
    return hz_;
}

} // namespace radio
} // namespace ragtime
