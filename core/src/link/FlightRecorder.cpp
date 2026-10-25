#include "ragtime/link/FlightRecorder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <utility>

#include "ragtime/Version.h"
#include "ragtime/files/SessionFiles.h"

namespace ragtime
{
namespace link
{

namespace
{

constexpr uint64_t DAY_MS = 24ull * 60 * 60 * 1000;
// Opening a file failed: try again this long after.
constexpr uint64_t RETRY_MS = 60 * 1000;
const char* const PREFIX = "flight-";
const char* const SUFFIX = ".log";

// Days since 1970-01-01 and the calendar date, both ways (Howard
// Hinnant's civil-date algorithms), so no gmtime is needed.
void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = (int)(yoe + era * 400) + (m <= 2 ? 1 : 0);
}

int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

struct Clock
{
    int year;
    unsigned month, day, hour, minute, second, ms;
};

Clock clockOf(uint64_t ms)
{
    Clock c{};
    civilFromDays((int64_t)(ms / DAY_MS), c.year, c.month, c.day);
    uint64_t inDay = ms % DAY_MS;
    c.hour = (unsigned)(inDay / 3600000);
    c.minute = (unsigned)(inDay / 60000 % 60);
    c.second = (unsigned)(inDay / 1000 % 60);
    c.ms = (unsigned)(inDay % 1000);
    return c;
}

// "flight-20261025-162542-123.log", which sorts in time order.
std::string fileNameFor(uint64_t ms)
{
    Clock c = clockOf(ms);
    char name[64];
    std::snprintf(name, sizeof name, "%s%04d%02u%02u-%02u%02u%02u-%03u%s", PREFIX, c.year, c.month, c.day, c.hour,
                  c.minute, c.second, c.ms, SUFFIX);
    return name;
}

bool isRecorderFile(const std::string& name)
{
    const size_t prefix = std::char_traits<char>::length(PREFIX);
    const size_t suffix = std::char_traits<char>::length(SUFFIX);
    return name.size() > prefix + suffix && name.compare(0, prefix, PREFIX) == 0
           && name.compare(name.size() - suffix, suffix, SUFFIX) == 0;
}

// The start time in a file's name; false if it has none.
bool startOf(const std::string& name, uint64_t& ms)
{
    // flight-YYYYMMDD-HHMMSS-mmm.log
    const size_t at = std::char_traits<char>::length(PREFIX);
    if (name.size() < at + 19) return false;
    std::string digits;
    for (size_t i = at; i < at + 19; i++)
    {
        char c = name[i];
        if (i == at + 8 || i == at + 15)
        {
            if (c != '-') return false;
            continue;
        }
        if (!std::isdigit((unsigned char)c)) return false;
        digits.push_back(c);
    }
    auto num = [&](size_t from, size_t count) { return (unsigned)std::stoul(digits.substr(from, count)); };
    unsigned month = num(4, 2), day = num(6, 2), hour = num(8, 2), minute = num(10, 2), second = num(12, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) return false;
    int64_t days = daysFromCivil((int)num(0, 4), month, day);
    if (days < 0) return false;
    ms = (uint64_t)days * DAY_MS + hour * 3600000ull + minute * 60000ull + second * 1000ull + num(14, 3);
    return true;
}

std::string hex(const std::string& bytes)
{
    static const char DIGITS[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes)
    {
        out.push_back(DIGITS[b >> 4]);
        out.push_back(DIGITS[b & 15]);
    }
    return out;
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool unhex(const std::string& text, std::vector<uint8_t>& out)
{
    if (text.size() % 2 != 0) return false;
    out.clear();
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2)
    {
        int hi = hexDigit(text[i]), lo = hexDigit(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)(hi * 16 + lo));
    }
    return true;
}

} // namespace

//-------------------------------------------------------------------------
// Recording
//-------------------------------------------------------------------------

void FlightRecorder::setFolder(const std::string& folder, uint64_t nowMs)
{
    if (folder == folder_ && !folder_.empty()) return;
    out_.close();
    fileName_.clear();
    fileDay_ = -1;
    failed_ = false;
    folder_ = folder;
    if (folder_.empty()) return;
    note(std::string("Ragtime ") + versionString() + " opened", nowMs);
}

void FlightRecorder::setLimits(uint64_t fileBytes, uint64_t capBytes)
{
    fileLimit_ = std::max<uint64_t>(1, fileBytes);
    capBytes_ = capBytes;
}

void FlightRecorder::roll(uint64_t nowMs)
{
    std::string name = fileNameFor(nowMs);
    if (out_.is_open() && name == fileName_) return;
    out_.close();
    std::error_code ec;
    std::filesystem::path dir = files::pathFromUtf8(folder_);
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path file = dir / files::pathFromUtf8(name);
    out_.open(file, std::ios::binary | std::ios::app);
    if (!out_)
    {
        out_.close();
        failed_ = true;
        failedMs_ = nowMs;
        return;
    }
    failed_ = false;
    fileName_ = name;
    fileDay_ = (int64_t)(nowMs / DAY_MS);
    uintmax_t size = std::filesystem::file_size(file, ec);
    fileBytes_ = ec ? 0 : (uint64_t)size;
    std::string first = flightTime(nowMs) + " note Ragtime " + versionString()
                        + " flight recorder; > is to data2g-host, < from it\n";
    out_ << first;
    out_.flush();
    fileBytes_ += first.size();
    prune(nowMs);
}

void FlightRecorder::write(const std::string& tag, const std::string& what, uint64_t nowMs)
{
    if (folder_.empty()) return;
    if (failed_ && nowMs < failedMs_ + RETRY_MS) return;
    if (!out_.is_open() || (int64_t)(nowMs / DAY_MS) != fileDay_ || fileBytes_ >= fileLimit_) roll(nowMs);
    if (!out_.is_open()) return;
    std::string line = flightTime(nowMs) + " " + tag;
    if (!what.empty()) line += " " + what;
    line += '\n';
    out_ << line;
    // At once: the lines just before a crash are the ones that matter.
    out_.flush();
    fileBytes_ += line.size();
}

void FlightRecorder::lines(Pieces& pieces, const char* tag, const char* bytes, size_t length, uint64_t nowMs)
{
    for (size_t i = 0; i < length; i++)
    {
        char c = bytes[i];
        if (c == '\r' || c == '\n')
        {
            if (!pieces.pending.empty()) write(tag, escapeCommandLine(pieces.pending), nowMs);
            pieces.pending.clear();
            continue;
        }
        pieces.pending.push_back(c);
        if (pieces.pending.size() >= MAX_PIECE)
        {
            write(tag, escapeCommandLine(pieces.pending), nowMs);
            pieces.pending.clear();
        }
    }
}

void FlightRecorder::frames(Pieces& pieces, const char* tag, const uint8_t* bytes, size_t length, uint64_t nowMs)
{
    for (size_t i = 0; i < length; i++)
    {
        uint8_t b = bytes[i];
        if (b == 0xC0)
        {
            if (!pieces.pending.empty()) write(tag, hex(pieces.pending), nowMs);
            pieces.pending.clear();
            pieces.started = true;
            continue;
        }
        pieces.pending.push_back((char)b);
        if (pieces.pending.size() >= MAX_PIECE)
        {
            write(tag, hex(pieces.pending), nowMs);
            pieces.pending.clear();
        }
    }
}

void FlightRecorder::flushPieces(uint64_t nowMs)
{
    if (!cmdIn_.pending.empty()) write("cmd<", escapeCommandLine(cmdIn_.pending), nowMs);
    if (!cmdOut_.pending.empty()) write("cmd>", escapeCommandLine(cmdOut_.pending), nowMs);
    if (!kissIn_.pending.empty()) write("kiss<", hex(kissIn_.pending), nowMs);
    if (!kissOut_.pending.empty()) write("kiss>", hex(kissOut_.pending), nowMs);
    cmdIn_ = cmdOut_ = kissIn_ = kissOut_ = Pieces();
}

void FlightRecorder::up(const std::string& address, uint64_t nowMs)
{
    cmdIn_ = cmdOut_ = kissIn_ = kissOut_ = Pieces();
    write("up", escapeCommandLine(address), nowMs);
}

void FlightRecorder::down(const std::string& why, uint64_t nowMs)
{
    flushPieces(nowMs);
    write("down", escapeCommandLine(why), nowMs);
}

void FlightRecorder::commandIn(const char* bytes, size_t length, uint64_t nowMs)
{
    lines(cmdIn_, "cmd<", bytes, length, nowMs);
}

void FlightRecorder::commandOut(const char* bytes, size_t length, uint64_t nowMs)
{
    lines(cmdOut_, "cmd>", bytes, length, nowMs);
}

void FlightRecorder::kissIn(const uint8_t* bytes, size_t length, uint64_t nowMs)
{
    frames(kissIn_, "kiss<", bytes, length, nowMs);
}

void FlightRecorder::kissOut(const uint8_t* bytes, size_t length, uint64_t nowMs)
{
    frames(kissOut_, "kiss>", bytes, length, nowMs);
}

void FlightRecorder::dataUp(bool up, uint64_t nowMs)
{
    write(up ? "data-up" : "data-down", std::string(), nowMs);
}

void FlightRecorder::dataIn(size_t length, uint64_t nowMs)
{
    if (length > 0) write("data<", std::to_string(length), nowMs);
}

void FlightRecorder::dataOut(size_t length, uint64_t nowMs)
{
    if (length > 0) write("data>", std::to_string(length), nowMs);
}

void FlightRecorder::note(const std::string& text, uint64_t nowMs)
{
    write("note", escapeCommandLine(text), nowMs);
}

std::vector<std::string> FlightRecorder::fileNames() const
{
    std::vector<std::string> names;
    if (folder_.empty()) return names;
    std::error_code ec;
    std::filesystem::directory_iterator it(files::pathFromUtf8(folder_), ec);
    if (ec) return names;
    for (const auto& entry : it)
    {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc)) continue;
        std::string name = files::utf8FromPath(entry.path().filename());
        if (isRecorderFile(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void FlightRecorder::prune(uint64_t nowMs)
{
    std::filesystem::path dir = files::pathFromUtf8(folder_);
    std::vector<std::pair<std::string, uint64_t>> kept;   // name, size
    uint64_t total = 0;
    for (const std::string& name : fileNames())
    {
        std::filesystem::path file = dir / files::pathFromUtf8(name);
        std::error_code ec;
        uint64_t start = 0;
        // A file runs at most to the end of the UTC day it began on.
        if (name != fileName_ && startOf(name, start) && (start / DAY_MS + 1) * DAY_MS + KEEP_MS <= nowMs)
        {
            std::filesystem::remove(file, ec);
            continue;
        }
        uintmax_t size = std::filesystem::file_size(file, ec);
        kept.emplace_back(name, ec ? 0 : (uint64_t)size);
        total += kept.back().second;
    }
    for (const auto& [name, size] : kept)
    {
        if (total <= capBytes_) break;
        if (name == fileName_) continue;
        std::error_code ec;
        if (std::filesystem::remove(dir / files::pathFromUtf8(name), ec)) total -= size;
    }
}

//-------------------------------------------------------------------------
// Reading back
//-------------------------------------------------------------------------

std::string flightTime(uint64_t ms)
{
    Clock c = clockOf(ms);
    char text[40];
    std::snprintf(text, sizeof text, "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", c.year, c.month, c.day, c.hour, c.minute,
                  c.second, c.ms);
    return text;
}

bool parseFlightTime(const std::string& text, uint64_t& ms)
{
    size_t at = 0;
    auto number = [&](size_t digits, unsigned& value) {
        if (at + digits > text.size()) return false;
        value = 0;
        for (size_t i = 0; i < digits; i++)
        {
            char c = text[at + i];
            if (!std::isdigit((unsigned char)c)) return false;
            value = value * 10 + (unsigned)(c - '0');
        }
        at += digits;
        return true;
    };
    auto expect = [&](char c) {
        if (at < text.size() && text[at] == c)
        {
            at++;
            return true;
        }
        return false;
    };
    unsigned year, month, day, hour, minute, second = 0, millis = 0;
    if (!number(4, year) || !expect('-') || !number(2, month) || !expect('-') || !number(2, day)) return false;
    if (!expect('T') && !expect(' ')) return false;
    if (!number(2, hour) || !expect(':') || !number(2, minute)) return false;
    if (expect(':'))
    {
        if (!number(2, second)) return false;
        if (expect('.'))
        {
            size_t digits = 0;
            while (at < text.size() && std::isdigit((unsigned char)text[at]) && digits < 3)
            {
                millis = millis * 10 + (unsigned)(text[at++] - '0');
                digits++;
            }
            if (digits == 0) return false;
            for (; digits < 3; digits++) millis *= 10;
        }
    }
    expect('Z');
    if (at != text.size()) return false;
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        return false;
    ms = (uint64_t)daysFromCivil((int)year, month, day) * DAY_MS + hour * 3600000ull + minute * 60000ull
         + second * 1000ull + millis;
    return true;
}

std::string escapeCommandLine(const std::string& line)
{
    std::string out;
    out.reserve(line.size());
    for (unsigned char c : line)
    {
        if (c == '\\')
        {
            out += "\\\\";
        }
        else if (c >= 0x20 && c < 0x7F)
        {
            out.push_back((char)c);
        }
        else
        {
            char code[8];
            std::snprintf(code, sizeof code, "\\x%02x", (unsigned)c);
            out += code;
        }
    }
    return out;
}

std::string unescapeCommandLine(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == '\\')
        {
            out.push_back('\\');
            i++;
        }
        else if (c == '\\' && i + 3 < text.size() && text[i + 1] == 'x' && hexDigit(text[i + 2]) >= 0
                 && hexDigit(text[i + 3]) >= 0)
        {
            out.push_back((char)(hexDigit(text[i + 2]) * 16 + hexDigit(text[i + 3])));
            i += 3;
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

bool parseFlightLine(const std::string& line, FlightLine& out)
{
    std::string text = line;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    size_t space = text.find(' ');
    if (space == std::string::npos) return false;
    FlightLine parsed;
    if (!parseFlightTime(text.substr(0, space), parsed.ms)) return false;
    size_t tagEnd = text.find(' ', space + 1);
    std::string tag = text.substr(space + 1, tagEnd == std::string::npos ? std::string::npos : tagEnd - space - 1);
    std::string what = tagEnd == std::string::npos ? std::string() : text.substr(tagEnd + 1);

    using Kind = FlightLine::Kind;
    if (tag == "up" || tag == "down" || tag == "cmd<" || tag == "cmd>" || tag == "note")
    {
        parsed.kind = tag == "up"     ? Kind::Up
                      : tag == "down" ? Kind::Down
                      : tag == "cmd<" ? Kind::CommandIn
                      : tag == "cmd>" ? Kind::CommandOut
                                      : Kind::Note;
        parsed.text = unescapeCommandLine(what);
    }
    else if (tag == "kiss<" || tag == "kiss>")
    {
        parsed.kind = tag == "kiss<" ? Kind::KissIn : Kind::KissOut;
        if (what.empty() || !unhex(what, parsed.bytes)) return false;
    }
    else if (tag == "data<" || tag == "data>")
    {
        parsed.kind = tag == "data<" ? Kind::DataIn : Kind::DataOut;
        if (what.empty() || !std::all_of(what.begin(), what.end(), [](char c) { return std::isdigit((unsigned char)c); }))
            return false;
        parsed.count = (size_t)std::strtoull(what.c_str(), nullptr, 10);
    }
    else if (tag == "data-up" || tag == "data-down")
    {
        parsed.kind = tag == "data-up" ? Kind::DataUp : Kind::DataDown;
    }
    else
    {
        return false;
    }
    out = std::move(parsed);
    return true;
}

std::vector<FlightLine> parseFlight(const std::string& text)
{
    std::vector<FlightLine> out;
    std::istringstream in(text);
    std::string line;
    FlightLine parsed;
    while (std::getline(in, line))
    {
        if (parseFlightLine(line, parsed)) out.push_back(parsed);
    }
    return out;
}

bool reportableSetting(const std::string& path)
{
    std::string lower;
    lower.reserve(path.size());
    for (char c : path) lower.push_back((char)std::tolower((unsigned char)c));
    static const char* const SECRET[] = {"private", "secret", "password", "passphrase", "passwd",
                                         "token",   "seed",   "recovery", "mnemonic"};
    for (const char* word : SECRET)
    {
        if (lower.find(word) != std::string::npos) return false;
    }
    return true;
}

} // namespace link
} // namespace ragtime
