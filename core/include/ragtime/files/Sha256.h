// SHA-256 (FIPS 180-4): whole-file checks and the 12-byte digests the
// protocol names items by (docs/PROTOCOL.md). Fed in pieces or all at once.
//
// Ported from Glissando's Sha256 (written there for Glissando from FIPS
// 180-4) and relicensed MIT by Jeff.
#ifndef RAGTIME_FILES_SHA256_H
#define RAGTIME_FILES_SHA256_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ragtime
{
namespace files
{

class Sha256
{
public:
    using Digest = std::array<uint8_t, 32>;

    Sha256();

    void update(const uint8_t* bytes, size_t length);
    void update(const std::vector<uint8_t>& bytes) { update(bytes.data(), bytes.size()); }

    // The digest of everything fed in; the hash starts again afterwards.
    Digest finish();

    static Digest of(const uint8_t* bytes, size_t length);
    static Digest of(const std::vector<uint8_t>& bytes) { return of(bytes.data(), bytes.size()); }

    // Lower-case hex, for tests and logs.
    static std::string hex(const Digest& digest);

private:
    void reset();
    void block(const uint8_t* bytes);

    uint32_t state_[8];
    uint8_t buffer_[64];
    size_t buffered_;
    uint64_t length_;   // bytes fed in
};

} // namespace files
} // namespace ragtime

#endif
