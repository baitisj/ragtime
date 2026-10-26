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
// Between bursts it also asks "+l STRENGTH", the S-meter in dB over S9,
// every STRENGTH_MS but not in the first SETTLE_MS after a burst (the
// receiver's AGC recovering), for the band's noise (radio::NoiseFloor).
//
// While the Ear is open (setFilterWatch), between bursts it also reads the
// rig's receive filter every FILTER_MS: "+m" (the mode and its passband
// width), then "+l PBT_IN" and "+l PBT_OUT" (radio::RigFilter). Jeff
// agreed to these reads on his shared rigctld (2026-10-26).
//
// The one thing it ever sets: the receive filter, when the Ear asks
// (setFilter) with "Let the Ear change the rig's filters" ticked (Jeff,
// 2026-10-26, for his rig held by a shared rigctld). Never while keyed:
// "M USB 2400" when the mode or width differs from the last reading, then
// "L PBT_IN 0.6510" and "L PBT_OUT ..." (with currVFO for a --vfo
// rigctld), one at a time, each answered by an RPRT; then the filter is
// read again at once. A refusal is kept for the Ear to show.
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

#include "ragtime/radio/Noise.h"
#include "ragtime/radio/RigFilter.h"
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
    // Between bursts, the S-meter this often, and not this soon after one.
    static constexpr uint64_t STRENGTH_MS = NoiseFloor::READ_MS;
    static constexpr uint64_t SETTLE_MS = NoiseFloor::SETTLE_MS;
    // While the Ear is open, the filter this often between bursts.
    static constexpr uint64_t FILTER_MS = 2000;

    enum class Meter
    {
        Alc,
        Swr,
        Watts,      // RFPOWER_METER_WATTS
        Power,      // RFPOWER_METER, asked only when the rig knows no watts
        Strength,   // STRENGTH, the S-meter, asked between bursts
        PbtIn,      // PBT_IN and PBT_OUT, asked only while the Ear is open
        PbtOut,
    };
    static constexpr int METERS = 7;
    // Probed on connecting: all but the PBT.
    static constexpr int PROBED = 5;

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
    // The Ear is open: read the filter between bursts. Off forgets it.
    void setFilterWatch(bool on, uint64_t nowMs);

    // The rig's filter, once "+m" has been answered while watching.
    const std::optional<RigFilter>& filter() const { return filter_; }
    // rigctld can't say the mode (it answered "+m" with an error).
    bool modeDropped() const { return modeDropped_; }
    // Set the receive filter, between bursts (the Ear, with its box
    // ticked: the caller's check). A newer setting replaces one not yet
    // sent.
    void setFilter(const RigFilter& filter, uint64_t nowMs);
    // Why the last setting was refused ("rigctld refused L PBT_IN 0.6510:
    // RPRT -11"), or empty; settings sent.
    const std::string& filterProblem() const { return filterProblem_; }
    // Closing (Ragtime quitting, the filter put back as found): every
    // filter command not yet sent goes now, without waiting for answers.
    void flushFilter();
    uint64_t filterWrites() const { return filterWrites_; }

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
    bool readsStrength() const { return reads(Meter::Strength); }
    // The meter readings taken while keyed, oldest first, each with one
    // meter in it.
    std::vector<MeterReading> takeReadings();
    // The S-meter readings taken between bursts, in dB over S9.
    std::vector<double> takeStrength();

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
        Mode,
        Set,        // a filter setting, answered by an RPRT
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
    bool transmitMeter(int meter) const { return meter < (int)Meter::Strength && askable(meter); }
    bool askFilter(uint64_t nowMs);
    void queueFilter();
    bool writeFilter(uint64_t nowMs);
    bool filterQuestionsLeft() const;
    void publishFilter();
    void modeLine(const std::string& line, uint64_t nowMs);
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
    uint64_t strengthDueMs_ = 0;
    uint64_t unkeyedMs_ = 0;
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
    std::vector<double> strength_;
    bool filterWatch_ = false;
    uint64_t filterDueMs_ = 0;
    int filterStep_ = 0;            // 0 the mode, 1 PBT_IN, 2 PBT_OUT
    bool inMode_ = false;           // "get_mode:" came: its long answer follows
    int modeWidth_ = 0;
    bool modeDropped_ = false;
    int modeFailures_ = 0;
    std::string modeName_;          // the answer coming in
    std::optional<RigFilter> filter_;
    std::optional<RigFilter> reading_;      // the one coming in
    std::optional<RigFilter> filterWrite_;  // to send
    std::vector<std::string> setLines_;     // its commands still to send
    std::string setLine_;                   // the one out
    std::string filterProblem_;
    uint64_t filterWrites_ = 0;
    std::string partial_;
    std::string out_;
    std::string problem_;
    uint64_t version_ = 0;
};

} // namespace radio
} // namespace ragtime

#endif
