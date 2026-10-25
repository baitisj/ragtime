// Parity for repairs (docs/PROTOCOL.md, "Parity"): a Cauchy Reed-Solomon
// code over GF(2^16), the field of 16-bit numbers where adding is XOR.
//
// The data are n symbols, each named by a number y (a piece's index, an
// item's place in a seal). A parity symbol, named by a number x that is
// none of the ys, is the sum over every data symbol of d_y times
// 1 / (x + y), worked 16 bits at a time. Any n of the data and parity
// symbols together give back all the data, whichever n they are: so one
// parity symbol fills a different gap for every listener that lacks one.
//
// Symbols are byte strings of one length, even (two bytes to a 16-bit
// number, most significant first); a shorter one counts as padded with
// zeros. No sockets and no clock.
#ifndef RAGTIME_FILES_PARITY_H
#define RAGTIME_FILES_PARITY_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ragtime
{
namespace files
{
namespace parity
{

// A symbol and its name.
struct Symbol
{
    uint16_t name = 0;
    const std::vector<uint8_t>* bytes = nullptr;
};

// The length parity symbols take for data of up to this many bytes: the
// next even number.
inline size_t symbolLength(size_t bytes)
{
    return (bytes + 1) & ~(size_t)1;
}

// The parity symbol named x over data, each symbol length bytes (even).
// Empty if x names a data symbol or length is odd.
std::vector<uint8_t> parityOf(uint16_t x, const std::vector<Symbol>& data, size_t length);

// Rebuilds the data symbols named missing from the rest of the data
// (known) and parity symbols, using the first missing.size() of them;
// out gets one symbol of length bytes for each name in missing, in order.
// False if there are too few parity symbols, length is odd, or two
// symbols share a name.
bool rebuild(const std::vector<Symbol>& known, const std::vector<Symbol>& parity,
             const std::vector<uint16_t>& missing, size_t length, std::vector<std::vector<uint8_t>>& out);

// How many parity symbols a listener that lacks need of them asks for,
// when it loses about a share loss of what is sent (held at 0 to 0.75):
// enough that about nine times in ten one round of them fills every gap,
// (need + sqrt(need * loss / (1 - loss))) / (1 - loss), rounded up.
uint32_t wantFor(uint32_t need, double loss);

} // namespace parity
} // namespace files
} // namespace ragtime

#endif
