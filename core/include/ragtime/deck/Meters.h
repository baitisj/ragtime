// The microphone deck's meters and its push to talk, with no drawing in
// them (docs/STAGE1.md, "The microphone deck"); app/src/VoiceDeck draws
// them.
//
// Two meters side by side (Jeff, 2026-10-31):
//   - Active level, a VU-style face from -40 to 0 dBFS: the speech level
//     with the pauses left out, the needle easing to it as a VU's does
//     (about 300 ms), with a green band at sotto's target, -29 to -23.
//   - Loudest 10 ms, a broadcast-style face from -30 to 0 dBFS: the RMS
//     of each 10 ms, sotto's own measure. Its needle jumps up at once and
//     falls back; a hold mark waits a second, then falls about 20 dB in
//     2 s. Green where sotto's test voices peak (-15 to -11), red past
//     -7.3, where sotto's energy field saturates.
#ifndef RAGTIME_DECK_METERS_H
#define RAGTIME_DECK_METERS_H

#include <cstdint>
#include <string>

namespace ragtime
{
namespace deck
{

// The faces' scales and bands, in dBFS.
constexpr double ACTIVE_FLOOR_DBFS = -40.0;
constexpr double ACTIVE_TOP_DBFS = 0.0;
constexpr double ACTIVE_BAND_LOW_DBFS = -29.0;
constexpr double ACTIVE_BAND_HIGH_DBFS = -23.0;
constexpr double LOUDEST_FLOOR_DBFS = -30.0;
constexpr double LOUDEST_TOP_DBFS = 0.0;
constexpr double LOUDEST_BAND_LOW_DBFS = -15.0;
constexpr double LOUDEST_BAND_HIGH_DBFS = -11.0;
constexpr double LOUDEST_RED_DBFS = -7.3;  // sotto's SOTTO_LEVEL_MAX_DBFS

// The leveller's gain reduction, 0 to 20 dB, with its bracket at 3 to 6,
// where normal talking should sit.
constexpr double REDUCTION_TOP_DB = 20.0;
constexpr double REDUCTION_BAND_LOW_DB = 3.0;
constexpr double REDUCTION_BAND_HIGH_DB = 6.0;

// How far round a face a level is, 0 at its floor to 1 at its top.
double faceFraction(double dbfs, double floor, double top);

// A VU's needle: it eases toward the reading with a time constant.
class VuNeedle
{
public:
    explicit VuNeedle(double floorDbfs, double seconds = 0.3) : position_(floorDbfs), floor_(floorDbfs), tau_(seconds) {}
    // The reading now (below the floor rests it on the floor), and the
    // seconds since the last step.
    void step(double dbfs, double seconds);
    double position() const { return position_; }
    void reset() { position_ = floor_; }

private:
    double position_;
    double floor_;
    double tau_;
};

// The Loudest 10 ms needle and its hold mark.
class PeakNeedle
{
public:
    explicit PeakNeedle(double floorDbfs) : needle_(floorDbfs), hold_(floorDbfs), floor_(floorDbfs) {}
    // The loudest 10 ms since the last step, and the seconds since then.
    void step(double dbfs, double seconds);
    double position() const { return needle_; }
    double hold() const { return hold_; }
    void reset();

    // The needle falls this fast; the hold waits, then falls.
    static constexpr double NEEDLE_FALL_DB_PER_S = 24.0;
    static constexpr double HOLD_WAIT_S = 1.0;
    static constexpr double HOLD_FALL_DB_PER_S = 10.0;

private:
    double needle_;
    double hold_;
    double floor_;
    double held_ = 0.0;     // seconds the hold has waited
};

// A whole number of dB for under a face: "−27", or "—" under the floor.
std::string dbText(double dbfs, double floor);

//-------------------------------------------------------------------------
// Push to talk
//-------------------------------------------------------------------------

// Where the clip is: the lamps under the button (Recording, Coding,
// Queued, Sending) and what the buttons may do. Push to talk and Monitor
// release each other (Jeff, 2026-10-31): Monitor is greyed while Push to
// talk is held, and pressing Push to talk stops Monitor at once.
enum class TalkPhase
{
    Idle,
    Recording,  // Push to talk held
    Coding,     // let go: the encoder's last frames, the file made
    Review,     // "Hold my clips for review": Play, Send or Discard
};

class Talk
{
public:
    TalkPhase phase() const { return phase_; }
    bool monitoring() const { return monitoring_; }

    // Monitor can start with a clip to play and Push to talk not held.
    bool canMonitor(bool haveClip) const { return haveClip && phase_ != TalkPhase::Recording && phase_ != TalkPhase::Coding; }

    // Push to talk pressed: recording starts (from Idle or Review, which
    // throws the clip held for review away), and Monitor stops. Returns
    // whether Monitor was playing, so its sound is stopped.
    bool press();
    // Let go: the clip is coded. False if not recording.
    bool release();
    // Esc while held: the clip is thrown away.
    bool discard();
    // The clip coded: held for review, or sent (back to Idle). An empty
    // clip (nothing but silence) goes nowhere.
    void coded(bool holdForReview, bool empty);
    // Review's Send or Discard.
    bool sendReviewed();
    bool discardReviewed();

    // Monitor started and finished (or stopped).
    bool startMonitor(bool haveClip);
    void stopMonitor() { monitoring_ = false; }

private:
    TalkPhase phase_ = TalkPhase::Idle;
    bool monitoring_ = false;
};

// The lamps of a clip that has gone to the file queue: queued until its
// first byte goes, then sending until its last, then dark.
enum class ClipAir
{
    None,
    Queued,
    Sending,
    Sent,
};

// The airtime bar: seconds on the air so far against the Max airtime
// mark, the bar red past it, and its words: "0:21 of air for 0:14 of
// speech".
struct AirBar
{
    double airSeconds = 0.0;
    double speechSeconds = 0.0;
    double maxSeconds = 60.0;

    // How full, 0 to 1, against a scale that shows the mark with room past
    // it: the mark at the scale's end up to it, then the scale grows.
    double scaleSeconds() const;
    bool over() const { return airSeconds > maxSeconds; }
    std::string words() const;
};

// "0:21", "1:05", "12:30".
std::string clockText(double seconds);

} // namespace deck
} // namespace ragtime

#endif
