#include "ragtime/protocol/Fields.h"

#include <algorithm>
#include <cctype>

#include "ragtime/files/Sha256.h"

namespace ragtime
{
namespace protocol
{

namespace
{

constexpr int RADIX = 40;
constexpr int END_MARK = 0;

int callsignCode(char c)
{
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    if (c == '/') return 37;
    if (c == '-') return 38;
    return -1;
}

char callsignChar(int code)
{
    if (code >= 1 && code <= 26) return (char)('A' + code - 1);
    if (code >= 27 && code <= 36) return (char)('0' + code - 27);
    if (code == 37) return '/';
    if (code == 38) return '-';
    return '\0';
}

} // namespace

Digest digestOf(const uint8_t* bytes, size_t length)
{
    files::Sha256::Digest full = files::Sha256::of(bytes, length);
    Digest digest{};
    std::copy(full.begin(), full.begin() + DIGEST_BYTES, digest.begin());
    return digest;
}

std::string hex(const Digest& digest)
{
    static const char* const DIGITS = "0123456789abcdef";
    std::string out;
    for (uint8_t b : digest)
    {
        out.push_back(DIGITS[b >> 4]);
        out.push_back(DIGITS[b & 0x0F]);
    }
    return out;
}

void putNumber(std::vector<uint8_t>& out, uint64_t value, int bytes)
{
    for (int shift = 8 * (bytes - 1); shift >= 0; shift -= 8) out.push_back((uint8_t)(value >> shift));
}

bool Reader::number(uint64_t& value, int bytes)
{
    if (left() < (size_t)bytes) return false;
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v = (v << 8) | at_[i];
    at_ += bytes;
    value = v;
    return true;
}

bool Reader::u8(uint8_t& value)
{
    uint64_t v = 0;
    if (!number(v, 1)) return false;
    value = (uint8_t)v;
    return true;
}

bool Reader::u16(uint16_t& value)
{
    uint64_t v = 0;
    if (!number(v, 2)) return false;
    value = (uint16_t)v;
    return true;
}

bool Reader::u24(uint32_t& value)
{
    uint64_t v = 0;
    if (!number(v, 3)) return false;
    value = (uint32_t)v;
    return true;
}

bool Reader::u32(uint32_t& value)
{
    uint64_t v = 0;
    if (!number(v, 4)) return false;
    value = (uint32_t)v;
    return true;
}

bool Reader::digest(Digest& value)
{
    if (left() < DIGEST_BYTES) return false;
    std::copy(at_, at_ + DIGEST_BYTES, value.begin());
    at_ += DIGEST_BYTES;
    return true;
}

bool Reader::callsign(std::string& value)
{
    std::string call;
    const uint8_t* at = at_;
    for (;;)
    {
        if (end_ - at < 2) return false;
        int word = (at[0] << 8) | at[1];
        at += 2;
        if (word >= RADIX * RADIX * RADIX) return false;
        const int codes[3] = {word / (RADIX * RADIX), (word / RADIX) % RADIX, word % RADIX};
        bool ended = false;
        for (int code : codes)
        {
            if (ended)
            {
                // Only end marks after the end.
                if (code != END_MARK) return false;
                continue;
            }
            if (code == END_MARK)
            {
                ended = true;
                continue;
            }
            char c = callsignChar(code);
            if (c == '\0' || call.size() == MAX_CALLSIGN) return false;
            call.push_back(c);
        }
        if (ended) break;
    }
    if (call.empty()) return false;
    at_ = at;
    value = call;
    return true;
}

bool Reader::bytes(std::vector<uint8_t>& value, size_t count)
{
    if (left() < count) return false;
    value.assign(at_, at_ + count);
    at_ += count;
    return true;
}

bool Reader::shortString(std::string& value)
{
    if (left() < 1 || left() < 1 + (size_t)at_[0]) return false;
    size_t length = at_[0];
    value.assign((const char*)at_ + 1, length);
    at_ += 1 + length;
    return true;
}

std::vector<uint8_t> Reader::rest()
{
    std::vector<uint8_t> out(at_, end_);
    at_ = end_;
    return out;
}

void putLength(std::vector<uint8_t>& out, size_t length)
{
    while (length >= 0x80)
    {
        out.push_back((uint8_t)(0x80 | (length & 0x7F)));
        length >>= 7;
    }
    out.push_back((uint8_t)length);
}

LengthRead readLength(const uint8_t* at, const uint8_t* end, size_t& length, size_t& fieldBytes)
{
    size_t value = 0;
    for (size_t i = 0; i < 2; i++)
    {
        if (at + i >= end) return LengthRead::Short;
        value |= (size_t)(at[i] & 0x7F) << (7 * i);
        if ((at[i] & 0x80) == 0)
        {
            length = value;
            fieldBytes = i + 1;
            return LengthRead::Ok;
        }
    }
    return LengthRead::Invalid;
}

std::string normalCallsign(const std::string& callsign)
{
    std::string out;
    for (char c : callsign)
    {
        char upper = (char)std::toupper((unsigned char)c);
        if (callsignCode(upper) < 0) return std::string();
        out.push_back(upper);
    }
    if (out.size() > MAX_CALLSIGN) return std::string();
    return out;
}

bool putCallsign(std::vector<uint8_t>& out, const std::string& callsign)
{
    std::string call = normalCallsign(callsign);
    if (call.empty()) return false;
    std::vector<int> codes;
    for (char c : call) codes.push_back(callsignCode(c));
    codes.push_back(END_MARK);
    while (codes.size() % 3 != 0) codes.push_back(END_MARK);
    for (size_t i = 0; i < codes.size(); i += 3)
    {
        int word = (codes[i] * RADIX + codes[i + 1]) * RADIX + codes[i + 2];
        out.push_back((uint8_t)(word >> 8));
        out.push_back((uint8_t)(word & 0xFF));
    }
    return true;
}

size_t callsignBytes(const std::string& callsign)
{
    std::string call = normalCallsign(callsign);
    return call.empty() ? 0 : 2 * ((call.size() + 1 + 2) / 3);
}

uint32_t packLocator(const std::string& locator)
{
    if (locator.size() != 4 && locator.size() != 6) return 0;
    auto letter = [](char c, char last) {
        c = (char)std::toupper((unsigned char)c);
        return c >= 'A' && c <= last ? c - 'A' : -1;
    };
    int f1 = letter(locator[0], 'R'), f2 = letter(locator[1], 'R');
    int d1 = std::isdigit((unsigned char)locator[2]) ? locator[2] - '0' : -1;
    int d2 = std::isdigit((unsigned char)locator[3]) ? locator[3] - '0' : -1;
    if (f1 < 0 || f2 < 0 || d1 < 0 || d2 < 0) return 0;
    uint32_t square = (uint32_t)(((f1 * 18 + f2) * 10 + d1) * 10 + d2);
    uint32_t sub = 0;
    if (locator.size() == 6)
    {
        int s1 = letter(locator[4], 'X'), s2 = letter(locator[5], 'X');
        if (s1 < 0 || s2 < 0) return 0;
        sub = (uint32_t)(1 + s1 * 24 + s2);
    }
    return square * 577 + sub + 1;
}

std::string unpackLocator(uint32_t packed)
{
    if (packed == 0 || packed > 32400u * 577) return std::string();
    uint32_t value = packed - 1;
    uint32_t sub = value % 577;
    uint32_t square = value / 577;
    std::string out(4, ' ');
    out[3] = (char)('0' + square % 10);
    square /= 10;
    out[2] = (char)('0' + square % 10);
    square /= 10;
    out[1] = (char)('A' + square % 18);
    out[0] = (char)('A' + square / 18);
    if (sub > 0)
    {
        out.push_back((char)('a' + (sub - 1) / 24));
        out.push_back((char)('a' + (sub - 1) % 24));
    }
    return out;
}

} // namespace protocol
} // namespace ragtime
