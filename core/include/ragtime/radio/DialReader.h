// DialReader: Ragtime's side of a conversation with rigctld (Hamlib's rig
// daemon, localhost:4532 by default), with no sockets. It reads the
// radio's dial frequency once a second, and that is all it ever asks:
// the radio is the source of truth, and the operator's rigctld is often
// shared with other programs (fldigi), so Ragtime never sets anything.
//
// The only line it ever writes is "f" (get frequency). rigctld answers
// with the frequency in Hz on a line of its own, or "RPRT -n" for an
// error. A rigctld started with --vfo wants a VFO named after every
// command; Ragtime doesn't name one (a VFO name sent to a rigctld
// without --vfo would be read as more commands), so it reads nothing
// from such a rigctld, and says so.
//
// It doesn't ask while our transmitter is keyed (some radios answer CAT
// slowly then), and the last reading stands meanwhile.
//
// The time is always passed in, so tests run it on a simulated clock.
#ifndef RAGTIME_RADIO_DIAL_READER_H
#define RAGTIME_RADIO_DIAL_READER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ragtime
{
namespace radio
{

class DialReader
{
public:
    // Asked this often.
    static constexpr uint64_t ASK_MS = 1000;
    // An answer not back in this long: asked again.
    static constexpr uint64_t ANSWER_MS = 3000;
    // A reading older than this (keyed time aside) is no longer known.
    static constexpr uint64_t STALE_MS = 5000;

    // The socket to rigctld has connected or closed.
    void connected(uint64_t nowMs);
    void disconnected();
    bool isConnected() const { return connected_; }

    void bytes(const char* bytes, size_t length, uint64_t nowMs);
    // What to write to rigctld.
    std::string takeOutput();

    // Asks when it is time.
    void tick(uint64_t nowMs);

    // Our transmitter is keyed (the host's PTT report): no asking.
    void setKeyed(bool keyed, uint64_t nowMs);

    // The dial in Hz; none until rigctld has answered, or once a reading
    // has gone stale.
    std::optional<uint64_t> dialHz(uint64_t nowMs) const;
    // Why there is no reading, for a tooltip: "rigctld answered RPRT -11".
    const std::string& problem() const { return problem_; }

    // Goes up whenever the reading or its state changes.
    uint64_t version() const { return version_; }

private:
    void ask(uint64_t nowMs);
    void onLine(const std::string& line, uint64_t nowMs);

    bool connected_ = false;
    bool keyed_ = false;
    bool waiting_ = false;          // an "f" is out
    uint64_t askedMs_ = 0;
    uint64_t readMs_ = 0;           // the last answer (or the end of keying)
    std::optional<uint64_t> hz_;
    std::string partial_;
    std::string out_;
    std::string problem_;
    uint64_t version_ = 0;
};

} // namespace radio
} // namespace ragtime

#endif
