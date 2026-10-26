// DialReader: Ragtime's side of a conversation with rigctld (Hamlib's rig
// daemon, localhost:4532 by default), with no sockets. It reads the
// radio's dial frequency once a second, and while data2g-host transmits
// the rig's transmit meters, and that is all it ever asks: the radio is
// the source of truth, and the operator's rigctld is often shared with
// other programs (fldigi), so Ragtime never sets anything.
//
// Every line it writes is a question, one at a time. On connecting it asks
// "\chk_vfo", as Hamlib's own rigctl -m 2 does: a rigctld started with
// --vfo answers 1 and wants a VFO named after every command, any other 0
// (a VFO name sent to a rigctld without --vfo would be read as more
// commands). A rigctld too old to know \chk_vfo answers with an RPRT and
// gets no VFO names. Then it asks each meter once, to learn which this rig
// reports, and from then on "f" (get frequency) once a second. rigctld
// answers "f" with the frequency in Hz on a line of its own, or "RPRT -n"
// for an error.
//
// The meters are levels: "+l ALC", "+l SWR" and "+l RFPOWER_METER_WATTS"
// (or "+l RFPOWER_METER", a share of full power, for a rig that knows no
// watts). The + asks for rigctld's long answer, which names the level
// before its value and ends with an RPRT line, so an answer that comes
// late is never taken for another meter's. With a VFO named they are
// "f currVFO" and "+l currVFO ALC".
//
// While our transmitter is keyed (the host's PTT report) it asks no dial,
// and the last reading stands; it asks the meters instead, one every
// METER_MS, each as soon as the last is answered or given up. A meter the
// probe was told is unknown (RPRT -1, -4 or -11), or that fails twice in a
// row while keyed, isn't asked again on this connection.
//
// The time is always passed in, so tests run it on a simulated clock.
#ifndef RAGTIME_RADIO_DIAL_READER_H
#define RAGTIME_RADIO_DIAL_READER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/radio/TxMeters.h"

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
    // While keyed, a meter is asked this long after the last answer.
    static constexpr uint64_t METER_MS = 250;

    enum class Meter
    {
        Alc,
        Swr,
        Watts,      // RFPOWER_METER_WATTS
        Power,      // RFPOWER_METER, asked only when the rig knows no watts
    };
    static constexpr int METERS = 4;

    // The socket to rigctld has connected or closed.
    void connected(uint64_t nowMs);
    void disconnected();
    bool isConnected() const { return connected_; }

    void bytes(const char* bytes, size_t length, uint64_t nowMs);
    // What to write to rigctld.
    std::string takeOutput();

    // Asks when it is time.
    void tick(uint64_t nowMs);

    // Our transmitter is keyed (the host's PTT report): the meters, not the
    // dial.
    void setKeyed(bool keyed, uint64_t nowMs);
    // The SWR trip is on: while keyed, every other question is the SWR.
    void setSwrWatch(bool on) { swrWatch_ = on; }

    // The dial in Hz; none until rigctld has answered, or once a reading
    // has gone stale.
    std::optional<uint64_t> dialHz(uint64_t nowMs) const;
    // Why there is no reading, for a tooltip: "rigctld answered RPRT -11".
    const std::string& problem() const { return problem_; }
    // rigctld was started with --vfo (it answered \chk_vfo with 1).
    bool vfoMode() const { return vfo_ == Vfo::Yes; }

    // rigctld has answered this meter on this connection.
    bool reads(Meter meter) const { return state_[(int)meter] == State::Answers; }
    bool readsPower() const { return reads(Meter::Watts) || reads(Meter::Power); }
    bool readsAny() const { return reads(Meter::Alc) || reads(Meter::Swr) || readsPower(); }
    // The meter readings taken while keyed, oldest first, each with one
    // meter in it.
    std::vector<MeterReading> takeReadings();

    // Goes up whenever the reading or its state changes.
    uint64_t version() const { return version_; }

private:
    enum class Vfo
    {
        Asking,     // \chk_vfo not yet answered
        No,
        Yes,
    };
    enum class Ask
    {
        None,
        Vfo,
        Freq,
        Meter,
    };
    enum class State
    {
        Unknown,
        Answers,
        Dropped,
    };

    void askNext(uint64_t nowMs);
    void askMeter(int meter, uint64_t nowMs);
    bool askable(int meter) const;
    int pickMeter();
    void answered(uint64_t nowMs);
    void onLine(const std::string& line, uint64_t nowMs);
    void vfoAnswer(const std::string& line);
    void meterValue(int meter, double value);
    void meterFailed(int meter, int code);

    bool connected_ = false;
    bool keyed_ = false;
    bool swrWatch_ = false;
    Vfo vfo_ = Vfo::Asking;
    Ask asking_ = Ask::None;
    uint64_t askedMs_ = 0;          // the question out
    uint64_t freqAskedMs_ = 0;      // the last "f"
    bool freqAsked_ = false;
    uint64_t meterDueMs_ = 0;
    uint64_t readMs_ = 0;           // the last answer (or the end of keying)
    std::optional<uint64_t> hz_;
    State state_[METERS] = {};
    int failures_[METERS] = {};
    int probe_ = 0;                 // the next meter to probe; METERS once done
    int askedMeter_ = -1;
    int answering_ = -1;            // the meter whose long answer is coming in
    int turn_ = 0;
    bool swrTurn_ = false;
    std::vector<MeterReading> readings_;
    std::string partial_;
    std::string out_;
    std::string problem_;
    uint64_t version_ = 0;
};

} // namespace radio
} // namespace ragtime

#endif
