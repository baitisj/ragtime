// Voice messages (docs/PROTOCOL.md, "Voice"): a clip of speech coded with
// sotto, Jeff's 2,400 bit/s vocoder, sent as a file of kind 0x03 exactly as
// a picture goes. This is the part with no audio and no codec in it: the
// .sot file (its header, sotto's 6-byte frames, and the silence runs that
// squeeze pauses), reading one that is still arriving or has holes, the
// pitch strip a voice card draws straight from the frames' bits, and what a
// clip costs the air. libsotto itself, which turns sound into frames and
// back, is loaded at run time elsewhere.
//
// A sotto frame is 48 bits for 20 ms of speech, most significant bit
// first: voicing (2 bits), pitch (7), energy (5), then the shape of the
// voice's spectrum (34, codebook indexes: 9, 9, 8 and 8 bits in v3.6). No frame depends on another, so
// any first part of a clip plays, and a lost frame costs 20 ms.
#ifndef RAGTIME_VOICE_VOICE_H
#define RAGTIME_VOICE_VOICE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/link/Data2GLink.h"

namespace ragtime
{
namespace voice
{

//-------------------------------------------------------------------------
// sotto's frame
//-------------------------------------------------------------------------

constexpr int FRAME_BYTES = 6;
constexpr int FRAME_MS = 20;
constexpr int FRAMES_PER_SECOND = 1000 / FRAME_MS;
// Speech costs this many bytes a second, before silence runs.
constexpr int SPEECH_BYTES_PER_SECOND = FRAME_BYTES * FRAMES_PER_SECOND;

using Frame = std::array<uint8_t, FRAME_BYTES>;

// A frame of true silence as a .sot file holds it: every bit 0. sotto
// decodes any frame whose energy is 0 as silence, whatever its other bits.
constexpr Frame SILENT_FRAME{};

// The voicing field: buzz (voiced), hiss (unvoiced) or both, for the
// frame's two 10 ms halves. Codes 0 to 2 say the same for both halves;
// code 3 is one half unvoiced and the other mixed, the order following the
// frame before.
enum class Voicing : uint8_t
{
    Unvoiced = 0,
    Voiced = 1,
    Mixed = 2,
    Onset = 3,
};

Voicing voicingOf(const Frame& frame);
// 0 to 127, log spaced from 50 Hz to 500 Hz.
int pitchCodeOf(const Frame& frame);
double pitchHzOf(const Frame& frame);
// 0 to 31; 0 is true silence.
int energyCodeOf(const Frame& frame);
inline bool isSilent(const Frame& frame) { return energyCodeOf(frame) == 0; }
// The frame's level as sotto codes it: the RMS of its 10 ms analysis
// window in dBFS (a full-scale sine is -3 dBFS), from -75.0 (code 1) to
// -7.3 (code 31, where louder frames saturate); sotto sends frames under
// -76.2 as silence, which gives SILENCE_DBFS here.
double energyDbfsOf(const Frame& frame);
constexpr double SILENCE_DBFS = -120.0;
// sotto.h's figures: field dB = RMS dBFS + ENERGY_OFFSET_DB, the field
// running from ENERGY_MIN_DB (code 0) to ENERGY_MAX_DB (code 31).
constexpr double ENERGY_OFFSET_DB = 87.3;
constexpr double ENERGY_MIN_DB = 10.0;
constexpr double ENERGY_MAX_DB = 80.0;
constexpr double ENERGY_STEP_DB = (ENERGY_MAX_DB - ENERGY_MIN_DB) / 31.0;

// A frame with these fields, the shape's 34 bits as given (only the low 34
// bits of shape are used). For tests and the bench's bots.
Frame makeFrame(Voicing voicing, int pitchCode, int energyCode, uint64_t shape);

//-------------------------------------------------------------------------
// The .sot file
//-------------------------------------------------------------------------

// The header: magic "So", codec, flags, then the clip's length in frames
// (3 bytes, big-endian), counting every silent frame a run stands for.
constexpr size_t HEADER_BYTES = 7;
constexpr uint8_t MAGIC_0 = 'S';
constexpr uint8_t MAGIC_1 = 'o';
// Codec 1 is sotto v3.6 (Jeff, 2026-10-31): its frame, its codebook (shape
// 9-9-8-8, 8-bit values), its energy from the spectral envelope, its
// postfilter and its concealment, frozen as text table 0 is. A later sotto
// that changes the bits is a new number; a receiver plays only numbers it
// knows.
constexpr uint8_t CODEC_SOTTO_V36 = 1;
// sotto_format() of a libsotto that speaks codec 1.
constexpr const char* SOTTO_V36_FORMAT = "sotto v3.6, shape 9-9-8-8";
// No flags yet. A reader refuses a file with a flag it doesn't know, since
// a flag could change what the frames mean.
constexpr uint8_t KNOWN_FLAGS = 0x00;
// A clip is at most this long, silences included, so a file can't ask a
// reader to make more than a few hundred KB of frames.
constexpr uint32_t MAX_CLIP_SECONDS = 30 * 60;
constexpr uint32_t MAX_CLIP_FRAMES = MAX_CLIP_SECONDS * FRAMES_PER_SECOND;
// A silent frame in the file stands for itself and the next run count
// silent frames, the count in the 16 bits after its energy field.
constexpr uint32_t MAX_RUN = 0xFFFF;

// The codec the library loaded speaks, from its sotto_format(); 0 if none
// we know.
uint8_t codecOfFormat(const std::string& format);

// A .sot file by its first bytes.
bool isSot(const uint8_t* bytes, size_t length);

// Silence at the ends goes, a quiet pause becomes silence, and a run of
// silent frames costs one frame: the file a clip of frames from libsotto
// is sent as. Empty if the clip is longer than MAX_CLIP_FRAMES or has
// nothing but silence in it.
struct PauseGate
{
    // A frame this far under the clip's loud frames (its 95th percentile
    // of energy) is quiet. sotto's test voices pause about 30 dB down.
    double belowDb = 30.0;
    // A quiet run at least this long becomes silence, but for hangFrames
    // at each end that touch speech, so word tails and breaths before a
    // word keep their sound. Quiet at either end of the clip goes whatever
    // its length, but for the hang next to the speech.
    int minFrames = 10;
    int hangFrames = 2;
    bool enabled = true;
};
std::vector<uint8_t> makeSot(const std::vector<Frame>& frames, const PauseGate& gate = PauseGate(),
                             uint8_t codec = CODEC_SOTTO_V36);

// The steps makeSot takes, one at a time.
// Quiet pauses into SILENT_FRAMEs.
std::vector<Frame> gatePauses(const std::vector<Frame>& frames, const PauseGate& gate);
// Silent frames off both ends.
std::vector<Frame> trimSilence(const std::vector<Frame>& frames);
// The file for frames as they stand: every silent frame becomes
// SILENT_FRAME and each run of them one frame. Empty past MAX_CLIP_FRAMES.
std::vector<uint8_t> writeSot(const std::vector<Frame>& frames, uint8_t codec = CODEC_SOTTO_V36);

// Bytes of a file that haven't come, [begin, end): pieces lost on D2G.
struct ByteRange
{
    uint64_t begin = 0;
    uint64_t end = 0;
};

enum class ReadResult
{
    Ok,
    Short,          // not even the header has come
    NotSot,         // the magic is wrong
    UnknownCodec,   // a codec this Ragtime can't play
    UnknownFlags,   // a flag this Ragtime doesn't know
    TooLong,        // the header says more than MAX_CLIP_FRAMES
    Malformed,      // the frames don't add up to the header's length
};

// A clip read from a .sot file, every 20 ms its own frame: a run of
// silence is as many SILENT_FRAMEs. good says whether each frame came (0:
// lost, for sotto's decoder to cover as it covers a lost frame on 2 m).
struct Clip
{
    uint8_t codec = 0;
    uint8_t flags = 0;
    uint32_t length = 0;            // the header's, in frames
    std::vector<Frame> frames;      // as far as the bytes have come; at most length
    std::vector<uint8_t> good;      // one a frame
    bool whole = false;             // every frame is here and good

    double seconds() const { return length / (double)FRAMES_PER_SECOND; }
};

// Reads a .sot file of size bytes, of which the first have bytes have come
// (a session's file arriving in order; size when all are here), less any
// in missing (a D2G file with pieces lost). A frame with a byte missing is
// one lost frame: if it stood for a silence run, the pause is shorter, so
// whatever frames the holes lose against the header's length go back into
// the holes as lost frames, shared by how many each lost. Frames past have
// aren't there yet, and the clip stops short of length.
ReadResult readSot(const uint8_t* bytes, uint64_t size, uint64_t have, const std::vector<ByteRange>& missing,
                   Clip& clip);
// A whole file.
ReadResult readSot(const std::vector<uint8_t>& file, Clip& clip);

//-------------------------------------------------------------------------
// The pitch strip
//-------------------------------------------------------------------------

// A voice card's strip: the clip's melody, pitch as height, loudness as
// thickness, silence as a gap, drawn from the frames' bits with no
// decoding. Each column is a slice of the clip's length.
struct StripColumn
{
    bool arrived = false;   // some of it has come
    bool lost = false;      // some of it is lost (hatched)
    bool silent = true;     // nothing but silence
    bool voiced = false;    // has buzz, so pitch means something
    double pitchHz = 0.0;   // voiced: the loudness-weighted mean pitch (on a log scale)
    double dbfs = SILENCE_DBFS; // the loudest frame
};

struct PitchStrip
{
    std::vector<StripColumn> columns;
    // The clip's pitch range, for the height: the voiced frames' 5th and
    // 95th percentiles, at least an octave apart; 80 to 300 Hz with no
    // voiced frames.
    double lowHz = 80.0;
    double highHz = 300.0;
    // The clip's loud level (95th percentile of its sounding frames), for
    // the thickness.
    double loudDbfs = -20.0;

    // 0 at the bottom to 1 at the top, on a log scale; clamped.
    double height(const StripColumn& column) const;
    // 0 for silence to 1 at loudDbfs or over, over a 40 dB range.
    double thickness(const StripColumn& column) const;
};

// columns slices across the clip's length (its header's, so a clip still
// arriving fills from the left).
PitchStrip pitchStrip(const Clip& clip, int columns);

// The frame (from 0) a click at x across a strip width wide plays from.
uint32_t frameAt(const Clip& clip, double x, double width);

//-------------------------------------------------------------------------
// What a clip costs the air
//-------------------------------------------------------------------------

// The most airtime a clip may take, as for pictures (the deck's Max
// airtime setting; its bar turns red past it).
constexpr int DEFAULT_MAX_AIR_SECONDS = 60;
constexpr int MIN_MAX_AIR_SECONDS = 5;
constexpr int MAX_MAX_AIR_SECONDS = 30 * 60;

// The first pass of a .sot of bytes called name broadcast on D2G by
// callsign in mode (announces and pieces, as BroadcastFiles sends them),
// in seconds. The seal that follows is picture::sealSeconds.
double broadcastAirSeconds(uint64_t bytes, const std::string& name, const std::string& callsign,
                           const link::ModeInfo& mode);

// The same in a session moving bytesPerSecond (picture::
// sessionBytesPerSecond): the offer and FileData's own bytes included. 0
// if the rate isn't known.
double sessionAirSeconds(uint64_t bytes, double bytesPerSecond);

// In a session the far end can play while the bytes arrive. With the file
// arriving at bytesPerSecond from its first byte, how long it must wait
// before playing so it never stalls: each frame's bytes must have come by
// the time it plays. 0 when the link outruns the speech. Taken from the
// file itself, so a silence run, which plays long for its 6 bytes, gives
// the link time to get ahead. Negative if the file doesn't read.
double playDelaySeconds(const std::vector<uint8_t>& file, double bytesPerSecond);

} // namespace voice
} // namespace ragtime

#endif
