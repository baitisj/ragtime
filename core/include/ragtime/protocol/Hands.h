// What a Ragtime Hand says, and how a roll call is run (docs/PROTOCOL.md,
// "Roll calls").
//
// A Hand (Data2G PR #146) is 63 bits in one of up to 40 narrow channels,
// sent at once by every station that heard one burst. Data2G sends it
// only as an answer, timed from a burst heard, into the window its sender
// opened. So a check-in by Hand is a roll call: the station checking in
// sends its Hello with a RollCall record and a window behind it, and every
// Ragtime that hears it checks in back by Hand.
//
// The Hand's 63 bits, most significant first:
//
//   version 3 | call 28 | locator 15 | S/N 6 | sessions 1 | automatic 1 |
//   500 Hz 1 | spare 8 (zero)
//
// - call: a standard callsign (up to six letters and digits, the digit in
//   the second or third place) packed as a number, or a 22-bit hash of any
//   other call (one with '/' or '-', or longer), which net control knows
//   only if it has heard that call some other way.
// - locator: a 4-character Maidenhead square, or none.
// - S/N: the roll call as the answering station's host heard it.
//
// No sockets, no clock.
#ifndef RAGTIME_PROTOCOL_HANDS_H
#define RAGTIME_PROTOCOL_HANDS_H

#include <cstdint>
#include <optional>
#include <string>

namespace ragtime
{
namespace protocol
{

constexpr int HAND_VERSION = 0;

// The 28-bit call field: standard calls take [0, STANDARD_CALLS), hashes
// [HAND_HASH_BASE, 2^28). The values between are never sent, so a Hand
// carrying one is a false decode.
constexpr uint32_t STANDARD_CALLS = 37u * 36u * 10u * 27u * 27u * 27u;   // 262,177,560
constexpr uint32_t HAND_HASH_BITS = 22;
constexpr uint32_t HAND_HASH_BASE = (1u << 28) - (1u << HAND_HASH_BITS);

// A standard call as a number below STANDARD_CALLS: up to six characters,
// A-Z and 0-9, with a digit second or third and only letters after it
// (W1AW, AG7EW, 2E0ABC). None for any other call.
std::optional<uint32_t> packStandardCall(const std::string& call);
// The call a number stands for; empty if it isn't a standard call's.
std::string unpackStandardCall(uint32_t packed);

// The call's 22-bit hash, for a call that isn't standard.
uint32_t handCallHash(const std::string& call);
// A shorter one, 16 bits, for a roll call's list of those already in.
uint16_t rollCallHash(const std::string& call);

// A Maidenhead square as 15 bits (its first four characters), or
// LOCATOR_NONE.
constexpr uint16_t LOCATOR_NONE = 0x7FFF;
uint16_t packSquare(const std::string& locator);
std::string unpackSquare(uint16_t packed);

struct HandCheckIn
{
    int version = HAND_VERSION;
    // The call: given when it is standard or the hash has been put
    // back to a call; otherwise empty, with hash set.
    std::string call;
    std::optional<uint32_t> hash;   // a call that isn't standard: its handCallHash
    std::string locator;            // four characters, or empty
    std::optional<double> snr;      // the roll call as heard by the answering host, -30 to +32 dB
    bool acceptsSessions = false;
    bool automatic = false;
    bool narrow = false;            // sends only 500 Hz modes on this link
};

// The 63 bits. The call goes standard when it can, else as its hash.
// 0 if the call isn't a callsign at all.
uint64_t packHand(const HandCheckIn& hand);
// What 63 bits say; none for a layout this version doesn't know, a call
// or locator number never sent, or spare bits set: all signs of a false
// decode. A hashed call comes back with hash set and call empty.
std::optional<HandCheckIn> unpackHand(uint64_t payload);

// The channel a station answers a roll call in, of the first `channels`:
// its call and the round's number, hashed, so each round shuffles who
// shares a channel.
int rollCallChannel(const std::string& call, uint16_t round, int channels);
// Whether a Hand from call in channel fits the round: the channel the call
// would pick in any width up to channels (8, 20 or 40, as a narrower
// station answers within its own width).
bool rollCallChannelFits(const std::string& call, uint16_t round, int channel, int channels);
// The tag Data2G seeds the Hands' CRC with: net control's call and the
// round, so a Hand can't be credited to another station's roll call.
int rollCallTag(const std::string& netControl, uint16_t round);

} // namespace protocol
} // namespace ragtime

#endif
