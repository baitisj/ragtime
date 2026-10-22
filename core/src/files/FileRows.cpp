#include "ragtime/files/FileRows.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "ragtime/session/SessionChat.h"

namespace ragtime
{
namespace files
{

const char* const BROADCAST_PEER = "Broadcast";

namespace
{

// The first line of the log, and its version.
const char* const HEADER = "ragtime-files 1";

const char* const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Tabs, newlines and backslashes inside a field, so a line splits on tabs.
std::string escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out += c;
    }
    return out;
}

std::string unescape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] != '\\' || i + 1 == text.size())
        {
            out += text[i];
            continue;
        }
        char c = text[++i];
        out += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return out;
}

std::vector<std::string> splitTabs(const std::string& line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;)
    {
        size_t tab = line.find('\t', start);
        fields.push_back(unescape(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return fields;
}

bool number(const std::string& text, uint64_t& out)
{
    if (text.empty() || text.size() > 20) return false;
    uint64_t n = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (uint64_t)(c - '0');
    }
    out = n;
    return true;
}

std::string hex(const uint8_t* bytes, size_t length)
{
    static const char* const DIGITS = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < length; i++)
    {
        out += DIGITS[bytes[i] >> 4];
        out += DIGITS[bytes[i] & 15];
    }
    return out;
}

bool unhex(const std::string& text, uint8_t* bytes, size_t length)
{
    if (text.size() != length * 2) return false;
    auto nibble = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (size_t i = 0; i < length; i++)
    {
        int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

const char* resultKey(FinishedFile::Result result)
{
    using R = FinishedFile::Result;
    switch (result)
    {
        case R::Saved:
            return "saved";
        case R::Sent:
            return "sent";
        case R::Declined:
            return "declined";
        case R::Cancelled:
            return "cancelled";
        case R::CancelledThere:
            return "cancelled-there";
        case R::Expired:
            return "expired";
        case R::NotTaken:
            return "not-taken";
        case R::Failed:
            return "failed";
    }
    return "failed";
}

bool resultFromKey(const std::string& key, FinishedFile::Result& result)
{
    using R = FinishedFile::Result;
    for (R r : {R::Saved, R::Sent, R::Declined, R::Cancelled, R::CancelledThere, R::Expired, R::NotTaken, R::Failed})
    {
        if (key == resultKey(r))
        {
            result = r;
            return true;
        }
    }
    return false;
}

// Days since 1970-01-01 to year, month (1-12) and day (Howard Hinnant's
// civil_from_days).
void civil(long long days, long long& year, unsigned& month, unsigned& day)
{
    days += 719468;
    long long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = y + (month <= 2 ? 1 : 0);
}

long long localDay(uint64_t atMs, long utcOffsetS)
{
    long long t = (long long)(atMs / 1000) + utcOffsetS;
    return (t >= 0 ? t : t - 86399) / 86400;
}

std::string monthDay(uint64_t atMs, long utcOffsetS)
{
    long long year = 0;
    unsigned month = 1, day = 1;
    civil(localDay(atMs, utcOffsetS), year, month, day);
    return std::string(MONTHS[(month - 1) % 12]) + " " + std::to_string(day);
}

} // namespace

std::string resultLabel(const FinishedFile& file)
{
    using R = FinishedFile::Result;
    switch (file.result)
    {
        case R::Saved:
            return "Saved";
        case R::Sent:
            return "Sent";
        case R::Declined:
            return file.outgoing ? "Declined by " + file.peer : "Declined";
        case R::Cancelled:
            return "Cancelled";
        case R::CancelledThere:
            return "Cancelled by " + file.peer;
        case R::Expired:
            return "Expired, no answer";
        case R::NotTaken:
            return "Not taken: " + file.peer + " takes no files";
        case R::Failed:
            return file.reason.empty() ? std::string("Failed") : "Failed: " + file.reason;
    }
    return "Failed";
}

std::string bpsText(const FinishedFile& file)
{
    if (file.movedBytes == 0 || file.movedMs < 1000) return std::string();
    double bps = (double)file.movedBytes * 8.0 * 1000.0 / (double)file.movedMs;
    return groupedNumber((uint64_t)(bps + 0.5));
}

//-------------------------------------------------------------------------
// The history
//-------------------------------------------------------------------------

uint64_t FileHistory::add(FinishedFile file)
{
    file.id = nextId_++;
    entries_.push_back(std::move(file));
    while (entries_.size() > MAX_ENTRIES) entries_.pop_front();
    version_++;
    return entries_.back().id;
}

bool FileHistory::remove(uint64_t id)
{
    auto it = std::find_if(entries_.begin(), entries_.end(), [id](const FinishedFile& f) { return f.id == id; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    version_++;
    return true;
}

void FileHistory::clear()
{
    entries_.clear();
    version_++;
}

const FinishedFile* FileHistory::find(uint64_t id) const
{
    for (const FinishedFile& f : entries_)
    {
        if (f.id == id) return &f;
    }
    return nullptr;
}

//-------------------------------------------------------------------------
// The log as text
//
//   ragtime-files 1
//   F <tab> via <tab> at ms <tab> in|out <tab> peer <tab> name <tab> size <tab> result <tab> reason <tab> path
//     [<tab> moved bytes <tab> moved ms]   (added 2026-10-22; older readers ignore them)
//   K <tab> peer <tab> name <tab> size <tab> digest (hex) <tab> path <tab> part <tab> bytes <tab> kept ms <tab> until ms
//-------------------------------------------------------------------------

std::string saveFilesText(const FileHistory& history, const std::vector<KeptPart>& kept,
                          const std::vector<std::string>& other)
{
    std::string out = std::string(HEADER) + "\n";
    for (const FinishedFile& f : history.entries())
    {
        std::vector<std::string> fields = {"F",
                                           f.via == Via::Broadcast ? "broadcast" : "session",
                                           std::to_string(f.atMs),
                                           f.outgoing ? "out" : "in",
                                           f.peer,
                                           f.name,
                                           std::to_string(f.size),
                                           resultKey(f.result),
                                           f.reason,
                                           f.path,
                                           std::to_string(f.movedBytes),
                                           std::to_string(f.movedMs)};
        for (size_t i = 0; i < fields.size(); i++) out += (i ? "\t" : "") + escape(fields[i]);
        out += "\n";
    }
    for (const KeptPart& k : kept)
    {
        std::vector<std::string> fields = {"K",
                                           k.peer,
                                           k.name,
                                           std::to_string(k.size),
                                           hex(k.digest, sizeof(k.digest)),
                                           k.path,
                                           k.part,
                                           std::to_string(k.bytes),
                                           std::to_string(k.keptAtMs),
                                           std::to_string(k.keepUntilMs)};
        for (size_t i = 0; i < fields.size(); i++) out += (i ? "\t" : "") + escape(fields[i]);
        out += "\n";
    }
    for (const std::string& line : other) out += line + "\n";
    return out;
}

bool loadFilesText(const std::string& text, FileHistory& history, std::vector<KeptPart>& kept,
                   std::vector<std::string>* other)
{
    size_t start = 0;
    bool first = true;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first)
        {
            // A later version's log is still read line by line; another
            // file altogether is not.
            if (line.compare(0, 14, "ragtime-files ") != 0) return false;
            first = false;
            continue;
        }
        if (line.empty()) continue;
        std::vector<std::string> f = splitTabs(line);
        if (f[0] == "F" && f.size() >= 10)
        {
            FinishedFile file;
            if (f[1] != "session" && f[1] != "broadcast") continue;
            file.via = f[1] == "broadcast" ? Via::Broadcast : Via::Session;
            if (!number(f[2], file.atMs) || (f[3] != "in" && f[3] != "out")) continue;
            file.outgoing = f[3] == "out";
            file.peer = f[4];
            file.name = f[5];
            if (!number(f[6], file.size) || !resultFromKey(f[7], file.result)) continue;
            file.reason = f[8];
            file.path = f[9];
            if (f.size() >= 12 && (!number(f[10], file.movedBytes) || !number(f[11], file.movedMs)))
                file.movedBytes = file.movedMs = 0;
            history.add(std::move(file));
        }
        else if (f[0] == "K" && f.size() >= 10)
        {
            KeptPart k;
            k.peer = f[1];
            k.name = f[2];
            if (!number(f[3], k.size) || !unhex(f[4], k.digest, sizeof(k.digest))) continue;
            k.path = f[5];
            k.part = f[6];
            if (!number(f[7], k.bytes) || !number(f[8], k.keptAtMs) || !number(f[9], k.keepUntilMs)) continue;
            if (k.peer.empty() || k.part.empty()) continue;
            kept.push_back(std::move(k));
        }
        else if (other != nullptr && f[0] != "F" && f[0] != "K")
        {
            other->push_back(line);
        }
    }
    return !first;
}

//-------------------------------------------------------------------------
// Words and numbers
//-------------------------------------------------------------------------

std::string groupedNumber(uint64_t n)
{
    std::string digits = std::to_string(n);
    std::string out;
    for (size_t i = 0; i < digits.size(); i++)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

std::string sizeText(uint64_t bytes)
{
    char buffer[32];
    if (bytes >= 1000000)
    {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
        return buffer;
    }
    if (bytes >= 10000)
    {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", (double)bytes / 1024.0);
        return buffer;
    }
    return groupedNumber(bytes) + (bytes == 1 ? " byte" : " bytes");
}

std::string progressText(uint64_t done, uint64_t size)
{
    return groupedNumber(done) + " of " + groupedNumber(size);
}

std::string rateText(double bytesPerSecond)
{
    if (!(bytesPerSecond > 0)) return std::string();
    char buffer[32];
    if (bytesPerSecond >= 1000)
        std::snprintf(buffer, sizeof(buffer), "%.1f KB/s", bytesPerSecond / 1024.0);
    else if (bytesPerSecond >= 10)
        std::snprintf(buffer, sizeof(buffer), "%.0f B/s", bytesPerSecond);
    else
        std::snprintf(buffer, sizeof(buffer), "%.1f B/s", bytesPerSecond);
    return buffer;
}

std::string finishText(uint64_t finishAtMs, uint64_t nowMs, long utcOffsetS)
{
    if (finishAtMs == 0) return std::string();
    uint64_t left = finishAtMs > nowMs ? finishAtMs - nowMs : 0;
    if (left >= 24ull * 3600 * 1000) return "ETA " + monthDay(finishAtMs, utcOffsetS);
    return "ETA " + session::clockTime(finishAtMs, utcOffsetS);
}

std::string stateText(const FileRow& row, uint64_t nowMs, long utcOffsetS)
{
    std::string finish = finishText(row.finishAtMs, nowMs, utcOffsetS);
    if (finish.empty()) return row.state;
    if (row.state.empty()) return finish;
    return row.state + (row.state == "QRL" ? " " : ", ") + finish;
}

std::string expiryText(uint64_t atMs, uint64_t nowMs, long utcOffsetS)
{
    if (atMs == 0) return std::string();
    uint64_t left = atMs > nowMs ? atMs - nowMs : 0;
    if (left < 3600 * 1000) return session::clockLength(left);
    if (localDay(atMs, utcOffsetS) == localDay(nowMs, utcOffsetS))
        return session::clockTime(atMs, utcOffsetS) + " (" + session::sessionLength(left) + ")";
    uint64_t days = left / (24ull * 3600 * 1000);
    std::string in = days >= 1 ? std::to_string(days) + (days == 1 ? " day" : " days") : session::sessionLength(left);
    return monthDay(atMs, utcOffsetS) + " (" + in + ")";
}

std::string dateText(uint64_t atMs, uint64_t nowMs, long utcOffsetS)
{
    if (atMs == 0) return std::string();
    std::string clock = session::clockTime(atMs, utcOffsetS);
    if (localDay(atMs, utcOffsetS) == localDay(nowMs, utcOffsetS)) return clock;
    return monthDay(atMs, utcOffsetS) + " " + clock;
}

std::string peerText(bool outgoing, const std::string& peer)
{
    return (outgoing ? "To " : "From ") + peer;
}

//-------------------------------------------------------------------------
// The rate
//-------------------------------------------------------------------------

void RateMeter::sample(uint64_t done, uint64_t nowMs)
{
    if (!samples_.empty())
    {
        if (done < samples_.back().done || nowMs < samples_.back().atMs)
        {
            // Started again from further back: a new transfer.
            samples_.clear();
        }
        else if (done > samples_.back().done)
        {
            lastMovedMs_ = nowMs;
        }
    }
    if (samples_.empty()) lastMovedMs_ = nowMs;
    if (!samples_.empty() && samples_.back().done == done && samples_.size() >= 2
        && samples_[samples_.size() - 2].done == done)
    {
        // Standing still: one sample marks the start of it, one the latest.
        samples_.back().atMs = nowMs;
    }
    else
    {
        samples_.push_back({nowMs, done});
    }
    // Keep one sample at or before the window's start, as its baseline.
    while (samples_.size() >= 2 && samples_[1].atMs + WINDOW_MS <= nowMs) samples_.pop_front();
}

void RateMeter::reset()
{
    samples_.clear();
    lastMovedMs_ = 0;
}

double RateMeter::rate() const
{
    if (samples_.size() < 2) return 0;
    const Sample& last = samples_.back();
    // The baseline, moved up to the window's start.
    Sample first = samples_.front();
    if (last.atMs > WINDOW_MS && first.atMs < last.atMs - WINDOW_MS && samples_.size() >= 2)
    {
        const Sample& next = samples_[1];
        uint64_t start = last.atMs - WINDOW_MS;
        double t = next.atMs > first.atMs ? (double)(start - first.atMs) / (double)(next.atMs - first.atMs) : 1.0;
        first.done = first.done + (uint64_t)std::llround(t * (double)(next.done - first.done));
        first.atMs = start;
    }
    uint64_t span = last.atMs - first.atMs;
    if (span < 3000 || last.done <= first.done) return 0;
    return (double)(last.done - first.done) * 1000.0 / (double)span;
}

uint64_t RateMeter::finishAtMs(uint64_t size, uint64_t nowMs) const
{
    double r = rate();
    if (!(r > 0) || samples_.empty()) return 0;
    uint64_t done = samples_.back().done;
    if (done >= size) return nowMs;
    return nowMs + (uint64_t)std::llround((double)(size - done) * 1000.0 / r);
}

} // namespace files
} // namespace ragtime
