#include "ragtime/protocol/Hands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/files/Sha256.h"
#include "ragtime/protocol/Fields.h"

namespace ragtime
{
namespace protocol
{

namespace
{

// The six places of a standard call: a prefix character (or space), a
// prefix character, the digit, and up to three letters (or spaces).
const char FIRST[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";    // 37
const char SECOND[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";    // 36
const char SUFFIX[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";             // 27

int indexIn(const char* set, char c)
{
    for (int i = 0; set[i] != 0; i++)
    {
        if (set[i] == c) return i;
    }
    return -1;
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

files::Sha256::Digest hashOf(const std::string& text, const std::vector<uint8_t>& extra = {})
{
    files::Sha256 sha;
    sha.update((const uint8_t*)text.data(), text.size());
    if (!extra.empty()) sha.update(extra);
    return sha.finish();
}

uint32_t firstWord(const files::Sha256::Digest& d)
{
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | (uint32_t)d[3];
}

std::vector<uint8_t> roundBytes(uint16_t round)
{
    return {0, (uint8_t)(round >> 8), (uint8_t)(round & 0xFF)};
}

constexpr int SNR_LOWEST = -30;
constexpr int SNR_HIGHEST = 32;
constexpr uint16_t SQUARES = 18 * 18 * 10 * 10;    // 32,400

constexpr int VERSION_SHIFT = 60;
constexpr int CALL_SHIFT = 32;
constexpr int LOCATOR_SHIFT = 17;
constexpr int SNR_SHIFT = 11;
constexpr uint64_t SESSIONS_BIT = uint64_t(1) << 10;
constexpr uint64_t AUTOMATIC_BIT = uint64_t(1) << 9;
constexpr uint64_t NARROW_BIT = uint64_t(1) << 8;
constexpr uint64_t SPARE_MASK = 0xFF;
constexpr uint64_t PAYLOAD_MASK = (uint64_t(1) << 63) - 1;

} // namespace

std::optional<uint32_t> packStandardCall(const std::string& call)
{
    if (call.size() < 3 || call.size() > 6) return std::nullopt;
    for (char c : call)
    {
        if (!isDigit(c) && !(c >= 'A' && c <= 'Z')) return std::nullopt;
    }
    std::string s;
    if (isDigit(call[2])) s = call;
    else if (isDigit(call[1])) s = " " + call;
    else return std::nullopt;
    if (s.size() > 6) return std::nullopt;
    s.resize(6, ' ');
    int a = indexIn(FIRST, s[0]);
    int b = indexIn(SECOND, s[1]);
    int c = isDigit(s[2]) ? s[2] - '0' : -1;
    int d = indexIn(SUFFIX, s[3]);
    int e = indexIn(SUFFIX, s[4]);
    int f = indexIn(SUFFIX, s[5]);
    if (a < 0 || b < 0 || c < 0 || d <= 0 || e < 0 || f < 0) return std::nullopt;
    // Letters after the digit, then only spaces.
    if (e == 0 && f != 0) return std::nullopt;
    uint32_t n = (uint32_t)a;
    n = n * 36 + (uint32_t)b;
    n = n * 10 + (uint32_t)c;
    n = n * 27 + (uint32_t)d;
    n = n * 27 + (uint32_t)e;
    n = n * 27 + (uint32_t)f;
    return n;
}

std::string unpackStandardCall(uint32_t packed)
{
    if (packed >= STANDARD_CALLS) return std::string();
    uint32_t n = packed;
    char s[6];
    s[5] = SUFFIX[n % 27];
    n /= 27;
    s[4] = SUFFIX[n % 27];
    n /= 27;
    s[3] = SUFFIX[n % 27];
    n /= 27;
    s[2] = (char)('0' + n % 10);
    n /= 10;
    s[1] = SECOND[n % 36];
    n /= 36;
    s[0] = FIRST[n];
    std::string call(s, 6);
    while (!call.empty() && call.back() == ' ') call.pop_back();
    if (!call.empty() && call.front() == ' ') call.erase(0, 1);
    // Only the one way of writing each call is ever sent.
    std::optional<uint32_t> again = packStandardCall(call);
    if (!again || *again != packed) return std::string();
    return call;
}

uint32_t handCallHash(const std::string& call)
{
    return firstWord(hashOf(normalCallsign(call))) >> (32 - HAND_HASH_BITS);
}

uint16_t rollCallHash(const std::string& call)
{
    return (uint16_t)(firstWord(hashOf(normalCallsign(call))) >> 16);
}

uint16_t packSquare(const std::string& locator)
{
    if (locator.size() < 4) return LOCATOR_NONE;
    char a = (char)std::toupper((unsigned char)locator[0]);
    char b = (char)std::toupper((unsigned char)locator[1]);
    char c = locator[2];
    char d = locator[3];
    if (a < 'A' || a > 'R' || b < 'A' || b > 'R' || !isDigit(c) || !isDigit(d)) return LOCATOR_NONE;
    return (uint16_t)((((a - 'A') * 18 + (b - 'A')) * 10 + (c - '0')) * 10 + (d - '0'));
}

std::string unpackSquare(uint16_t packed)
{
    if (packed >= SQUARES) return std::string();
    std::string s(4, ' ');
    s[3] = (char)('0' + packed % 10);
    packed /= 10;
    s[2] = (char)('0' + packed % 10);
    packed /= 10;
    s[1] = (char)('A' + packed % 18);
    packed /= 18;
    s[0] = (char)('A' + packed);
    return s;
}

uint64_t packHand(const HandCheckIn& hand)
{
    std::string call = normalCallsign(hand.call);
    if (call.empty()) return 0;
    uint32_t field = 0;
    if (std::optional<uint32_t> standard = packStandardCall(call)) field = *standard;
    else field = HAND_HASH_BASE + handCallHash(call);
    uint64_t snr = 0;
    if (hand.snr && std::isfinite(*hand.snr))
    {
        long dB = std::lround(*hand.snr);
        dB = std::min<long>(std::max<long>(dB, SNR_LOWEST), SNR_HIGHEST);
        snr = (uint64_t)(dB - SNR_LOWEST + 1);
    }
    uint64_t out = (uint64_t)(hand.version & 7) << VERSION_SHIFT;
    out |= (uint64_t)field << CALL_SHIFT;
    out |= (uint64_t)packSquare(hand.locator) << LOCATOR_SHIFT;
    out |= snr << SNR_SHIFT;
    if (hand.acceptsSessions) out |= SESSIONS_BIT;
    if (hand.automatic) out |= AUTOMATIC_BIT;
    if (hand.narrow) out |= NARROW_BIT;
    return out & PAYLOAD_MASK;
}

std::optional<HandCheckIn> unpackHand(uint64_t payload)
{
    payload &= PAYLOAD_MASK;
    HandCheckIn hand;
    hand.version = (int)((payload >> VERSION_SHIFT) & 7);
    if (hand.version != HAND_VERSION || (payload & SPARE_MASK) != 0) return std::nullopt;
    uint32_t field = (uint32_t)((payload >> CALL_SHIFT) & 0x0FFFFFFF);
    if (field < STANDARD_CALLS)
    {
        hand.call = unpackStandardCall(field);
        if (hand.call.empty()) return std::nullopt;
    }
    else if (field >= HAND_HASH_BASE)
    {
        hand.hash = field - HAND_HASH_BASE;
    }
    else
    {
        return std::nullopt;
    }
    uint16_t square = (uint16_t)((payload >> LOCATOR_SHIFT) & 0x7FFF);
    if (square != LOCATOR_NONE)
    {
        hand.locator = unpackSquare(square);
        if (hand.locator.empty()) return std::nullopt;
    }
    int snr = (int)((payload >> SNR_SHIFT) & 0x3F);
    if (snr != 0) hand.snr = (double)(snr - 1 + SNR_LOWEST);
    hand.acceptsSessions = (payload & SESSIONS_BIT) != 0;
    hand.automatic = (payload & AUTOMATIC_BIT) != 0;
    hand.narrow = (payload & NARROW_BIT) != 0;
    return hand;
}

int rollCallChannel(const std::string& call, uint16_t round, int channels)
{
    if (channels <= 0) return 0;
    return (int)(firstWord(hashOf(normalCallsign(call), roundBytes(round))) % (uint32_t)channels);
}

bool rollCallChannelFits(const std::string& call, uint16_t round, int channel, int channels)
{
    // The widths Data2G gives Hands: 500, 1200 and 2400 Hz.
    for (int width : {8, 20, 40})
    {
        if (width > channels) break;
        if (rollCallChannel(call, round, width) == channel) return true;
    }
    // A net control narrower than all of them (none today).
    return channels < 8 && rollCallChannel(call, round, channels) == channel;
}

int rollCallTag(const std::string& netControl, uint16_t round)
{
    return hashOf(normalCallsign(netControl), roundBytes(round))[4];
}

} // namespace protocol
} // namespace ragtime
