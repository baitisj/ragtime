// The field codings records are built from (docs/PROTOCOL.md, "Fields"):
// big-endian numbers, the record length, packed callsigns and locators,
// and the 12-byte digests items are named by.
#ifndef RAGTIME_PROTOCOL_FIELDS_H
#define RAGTIME_PROTOCOL_FIELDS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ragtime
{
namespace protocol
{

// SHA-256, first 12 bytes.
constexpr size_t DIGEST_BYTES = 12;
using Digest = std::array<uint8_t, DIGEST_BYTES>;

Digest digestOf(const uint8_t* bytes, size_t length);
inline Digest digestOf(const std::vector<uint8_t>& bytes)
{
    return digestOf(bytes.data(), bytes.size());
}
std::string hex(const Digest& digest);

// Writes the low `bytes` bytes of value, most significant first.
void putNumber(std::vector<uint8_t>& out, uint64_t value, int bytes);

// Reads a field from [at, end), moving at past it. Each returns false,
// with at left where it was, when the field is not all there or not valid.
class Reader
{
public:
    Reader(const uint8_t* begin, const uint8_t* end) : at_(begin), end_(end) {}

    bool number(uint64_t& value, int bytes);
    bool u8(uint8_t& value);
    bool u16(uint16_t& value);
    bool u24(uint32_t& value);
    bool u32(uint32_t& value);
    bool digest(Digest& value);
    bool callsign(std::string& value);
    bool bytes(std::vector<uint8_t>& value, size_t count);
    // A length byte, then that many bytes of UTF-8.
    bool shortString(std::string& value);

    const uint8_t* at() const { return at_; }
    size_t left() const { return (size_t)(end_ - at_); }
    bool done() const { return at_ == end_; }
    // Everything still unread.
    std::vector<uint8_t> rest();

private:
    const uint8_t* at_;
    const uint8_t* end_;
};

//-------------------------------------------------------------------------
// Record lengths: 7 bits a byte, least significant group first, the top
// bit set on every byte but the last (LEB128). Ragtime v0 takes at most
// two bytes, so a record is at most MAX_RECORD_LENGTH long after its
// length field.
//-------------------------------------------------------------------------

constexpr size_t MAX_RECORD_LENGTH = 16383;

void putLength(std::vector<uint8_t>& out, size_t length);

enum class LengthRead
{
    Ok,
    Short,      // not all of it here yet
    Invalid,    // longer than two bytes
};
LengthRead readLength(const uint8_t* at, const uint8_t* end, size_t& length, size_t& fieldBytes);

//-------------------------------------------------------------------------
// Callsigns
//-------------------------------------------------------------------------

constexpr size_t MAX_CALLSIGN = 10;

// The callsign as Data2G takes it, and as records carry it: letters,
// digits, '/' and '-', upper case, 1 to 10 characters. Empty if it is not
// one (a character outside those, or too long).
std::string normalCallsign(const std::string& callsign);

// Radix 40, three characters to two bytes: the call and an end mark,
// padded with end marks to a multiple of three. A..Z are 1..26, 0..9
// 27..36, '/' 37, '-' 38; 0 is the end mark and 39 is reserved. False if
// the call is not a normal one.
bool putCallsign(std::vector<uint8_t>& out, const std::string& callsign);
size_t callsignBytes(const std::string& callsign);

//-------------------------------------------------------------------------
// Maidenhead locators: four or six characters in four bytes, 0 for none.
//-------------------------------------------------------------------------

uint32_t packLocator(const std::string& locator);
std::string unpackLocator(uint32_t packed);

} // namespace protocol
} // namespace ragtime

#endif
