// Voice messages: the .sot file, the pitch strip and the airtime sums
// (ragtime/voice/Voice.h, docs/PROTOCOL.md "Voice").
#include "ragtime/voice/Voice.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ragtime/broadcast/Airtime.h"

namespace ragtime
{
namespace voice
{

namespace
{

// sotto's pitch field runs from 2π/160 to 2π/16 radians a sample at 8 kHz.
constexpr double PITCH_LOW_HZ = 50.0;
constexpr double PITCH_HIGH_HZ = 500.0;

// The frame's 48 bits, most significant first.
uint64_t bitsOf(const Frame& frame)
{
    uint64_t v = 0;
    for (uint8_t b : frame) v = (v << 8) | b;
    return v;
}

Frame frameOf(uint64_t v)
{
    Frame frame{};
    for (int i = FRAME_BYTES - 1; i >= 0; i--)
    {
        frame[(size_t)i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
    return frame;
}

// Where the fields sit, counted up from the frame's last bit.
constexpr int VOICING_SHIFT = 46;
constexpr int PITCH_SHIFT = 39;
constexpr int ENERGY_SHIFT = 34;
constexpr uint64_t SHAPE_MASK = (1ull << 34) - 1;
// A silent frame's run count: the 16 bits after its energy field.
constexpr int RUN_SHIFT = 18;

double fieldDbOf(int code)
{
    return ENERGY_MIN_DB + code * ENERGY_STEP_DB;
}

// The value at fraction p (0 to 1) of sorted values; values isn't empty.
double percentile(std::vector<double> values, double p)
{
    std::sort(values.begin(), values.end());
    size_t i = (size_t)std::floor(p * (double)(values.size() - 1) + 0.5);
    return values[std::min(i, values.size() - 1)];
}

bool overlaps(uint64_t begin, uint64_t end, const std::vector<ByteRange>& missing)
{
    for (const ByteRange& r : missing)
        if (r.begin < end && begin < r.end) return true;
    return false;
}

} // namespace

//-------------------------------------------------------------------------
// sotto's frame
//-------------------------------------------------------------------------

Voicing voicingOf(const Frame& frame)
{
    return (Voicing)((bitsOf(frame) >> VOICING_SHIFT) & 0x3);
}

int pitchCodeOf(const Frame& frame)
{
    return (int)((bitsOf(frame) >> PITCH_SHIFT) & 0x7F);
}

double pitchHzOf(const Frame& frame)
{
    const double lo = std::log(PITCH_LOW_HZ), hi = std::log(PITCH_HIGH_HZ);
    return std::exp(lo + pitchCodeOf(frame) * (hi - lo) / 127.0);
}

int energyCodeOf(const Frame& frame)
{
    return (int)((bitsOf(frame) >> ENERGY_SHIFT) & 0x1F);
}

double energyDbfsOf(const Frame& frame)
{
    int code = energyCodeOf(frame);
    return code == 0 ? SILENCE_DBFS : fieldDbOf(code) - ENERGY_OFFSET_DB;
}

Frame makeFrame(Voicing voicing, int pitchCode, int energyCode, uint64_t shape)
{
    uint64_t v = ((uint64_t)voicing & 0x3) << VOICING_SHIFT;
    v |= ((uint64_t)pitchCode & 0x7F) << PITCH_SHIFT;
    v |= ((uint64_t)energyCode & 0x1F) << ENERGY_SHIFT;
    v |= shape & SHAPE_MASK;
    return frameOf(v);
}

//-------------------------------------------------------------------------
// The .sot file
//-------------------------------------------------------------------------

uint8_t codecOfFormat(const std::string& format)
{
    return format == SOTTO_V36_FORMAT ? CODEC_SOTTO_V36 : 0;
}

bool isSot(const uint8_t* bytes, size_t length)
{
    return length >= 4 && bytes[0] == MAGIC_0 && bytes[1] == MAGIC_1 && bytes[2] == CODEC_SOTTO_V36 &&
           (bytes[3] & ~KNOWN_FLAGS) == 0;
}

std::vector<Frame> gatePauses(const std::vector<Frame>& frames, const PauseGate& gate)
{
    std::vector<Frame> out = frames;
    if (!gate.enabled) return out;
    std::vector<double> sounding;
    for (const Frame& f : frames)
        if (!isSilent(f)) sounding.push_back(fieldDbOf(energyCodeOf(f)));
    if (sounding.empty()) return out;
    const double threshold = percentile(sounding, 0.95) - gate.belowDb;
    const size_t n = frames.size();
    auto quiet = [&](size_t i) { return isSilent(frames[i]) || fieldDbOf(energyCodeOf(frames[i])) <= threshold; };
    const size_t hang = (size_t)std::max(0, gate.hangFrames);
    size_t i = 0;
    while (i < n)
    {
        if (!quiet(i))
        {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && quiet(j)) j++;
        const bool atStart = i == 0, atEnd = j == n;
        if (atStart || atEnd || j - i >= (size_t)std::max(1, gate.minFrames))
        {
            size_t a = atStart ? i : std::min(j, i + hang);
            size_t b = atEnd ? j : (j - i > hang ? j - hang : i);
            for (size_t k = a; k < b; k++) out[k] = SILENT_FRAME;
        }
        i = j;
    }
    return out;
}

std::vector<Frame> trimSilence(const std::vector<Frame>& frames)
{
    size_t a = 0, b = frames.size();
    while (a < b && isSilent(frames[a])) a++;
    while (b > a && isSilent(frames[b - 1])) b--;
    return std::vector<Frame>(frames.begin() + (std::ptrdiff_t)a, frames.begin() + (std::ptrdiff_t)b);
}

std::vector<uint8_t> writeSot(const std::vector<Frame>& frames, uint8_t codec)
{
    if (frames.size() > MAX_CLIP_FRAMES) return {};
    const uint32_t length = (uint32_t)frames.size();
    std::vector<uint8_t> out = {MAGIC_0, MAGIC_1, codec, 0, (uint8_t)(length >> 16), (uint8_t)(length >> 8),
                                (uint8_t)length};
    size_t i = 0;
    while (i < frames.size())
    {
        if (!isSilent(frames[i]))
        {
            out.insert(out.end(), frames[i].begin(), frames[i].end());
            i++;
            continue;
        }
        size_t j = i;
        while (j < frames.size() && isSilent(frames[j])) j++;
        size_t run = j - i;
        while (run > 0)
        {
            size_t take = std::min<size_t>(run, (size_t)MAX_RUN + 1);
            Frame head = frameOf((uint64_t)(take - 1) << RUN_SHIFT);
            out.insert(out.end(), head.begin(), head.end());
            run -= take;
        }
        i = j;
    }
    return out;
}

std::vector<uint8_t> makeSot(const std::vector<Frame>& frames, const PauseGate& gate, uint8_t codec)
{
    std::vector<Frame> clip = trimSilence(gatePauses(frames, gate));
    if (clip.empty()) return {};
    return writeSot(clip, codec);
}

ReadResult readSot(const uint8_t* bytes, uint64_t size, uint64_t have, const std::vector<ByteRange>& missing,
                   Clip& clip)
{
    clip = Clip();
    have = std::min(have, size);
    if (have < 2 || overlaps(0, 2, missing)) return ReadResult::Short;
    if (bytes[0] != MAGIC_0 || bytes[1] != MAGIC_1) return ReadResult::NotSot;
    if (have < HEADER_BYTES || overlaps(0, HEADER_BYTES, missing)) return ReadResult::Short;
    clip.codec = bytes[2];
    clip.flags = bytes[3];
    clip.length = ((uint32_t)bytes[4] << 16) | ((uint32_t)bytes[5] << 8) | bytes[6];
    if (clip.codec != CODEC_SOTTO_V36) return ReadResult::UnknownCodec;
    if ((clip.flags & ~KNOWN_FLAGS) != 0) return ReadResult::UnknownFlags;
    if (clip.length > MAX_CLIP_FRAMES) return ReadResult::TooLong;
    if ((size - HEADER_BYTES) % FRAME_BYTES != 0) return ReadResult::Malformed;

    // The clip in stretches: frames that came, and holes of lost frames,
    // each first counted as one lost frame for each it lost.
    struct Hole
    {
        size_t at = 0;      // where in frames it goes
        size_t lost = 0;    // file frames lost in it
    };
    std::vector<Hole> holes;
    std::vector<Frame> frames;
    size_t total = 0;       // frames, with a frame for each lost one
    const uint64_t coded = (size - HEADER_BYTES) / FRAME_BYTES;
    bool arrived = true;
    for (uint64_t i = 0; i < coded; i++)
    {
        const uint64_t begin = HEADER_BYTES + i * FRAME_BYTES, end = begin + FRAME_BYTES;
        if (end > have)
        {
            arrived = false;
            break;
        }
        if (overlaps(begin, end, missing))
        {
            if (holes.empty() || holes.back().at != frames.size() || holes.back().lost == 0 ||
                // Only a hole that runs on from the frame before grows.
                !overlaps(begin - FRAME_BYTES, begin, missing))
                holes.push_back(Hole{frames.size(), 0});
            holes.back().lost++;
            total++;
        }
        else
        {
            Frame f;
            std::copy(bytes + begin, bytes + end, f.begin());
            const size_t count = isSilent(f) ? 1 + (size_t)((bitsOf(f) >> RUN_SHIFT) & MAX_RUN) : 1;
            if (total + count > clip.length) return ReadResult::Malformed;
            frames.insert(frames.end(), count, isSilent(f) ? SILENT_FRAME : f);
            total += count;
        }
        if (total > clip.length) return ReadResult::Malformed;
    }

    // Whatever the holes lost against the header's length goes back into
    // them, shared by how many frames each lost.
    std::vector<size_t> extra(holes.size(), 0);
    if (arrived)
    {
        size_t deficit = clip.length - total;
        if (holes.empty() && deficit != 0) return ReadResult::Malformed;
        size_t lostAll = 0;
        for (const Hole& h : holes) lostAll += h.lost;
        size_t given = 0;
        for (size_t k = 0; k < holes.size(); k++)
        {
            extra[k] = (size_t)((uint64_t)deficit * holes[k].lost / lostAll);
            given += extra[k];
        }
        for (size_t k = 0; given < deficit; k = (k + 1) % holes.size(), given++) extra[k]++;
    }

    clip.frames.reserve(total);
    clip.good.reserve(total);
    size_t from = 0;
    for (size_t k = 0; k <= holes.size(); k++)
    {
        const size_t to = k < holes.size() ? holes[k].at : frames.size();
        clip.frames.insert(clip.frames.end(), frames.begin() + (std::ptrdiff_t)from,
                           frames.begin() + (std::ptrdiff_t)to);
        clip.good.insert(clip.good.end(), to - from, 1);
        from = to;
        if (k < holes.size())
        {
            const size_t lost = holes[k].lost + extra[k];
            clip.frames.insert(clip.frames.end(), lost, SILENT_FRAME);
            clip.good.insert(clip.good.end(), lost, 0);
        }
    }
    clip.whole = arrived && holes.empty() && clip.frames.size() == clip.length;
    return ReadResult::Ok;
}

ReadResult readSot(const std::vector<uint8_t>& file, Clip& clip)
{
    return readSot(file.data(), file.size(), file.size(), {}, clip);
}

//-------------------------------------------------------------------------
// The pitch strip
//-------------------------------------------------------------------------

double PitchStrip::height(const StripColumn& column) const
{
    if (!column.voiced || !(column.pitchHz > 0)) return 0.0;
    const double h = (std::log(column.pitchHz) - std::log(lowHz)) / (std::log(highHz) - std::log(lowHz));
    return std::clamp(h, 0.0, 1.0);
}

double PitchStrip::thickness(const StripColumn& column) const
{
    if (column.silent) return 0.0;
    return std::clamp(1.0 + (column.dbfs - loudDbfs) / 40.0, 0.0, 1.0);
}

PitchStrip pitchStrip(const Clip& clip, int columns)
{
    PitchStrip strip;
    std::vector<double> pitches, levels;
    for (size_t i = 0; i < clip.frames.size(); i++)
    {
        const Frame& f = clip.frames[i];
        if (!clip.good[i] || isSilent(f)) continue;
        levels.push_back(energyDbfsOf(f));
        if (voicingOf(f) != Voicing::Unvoiced) pitches.push_back(pitchHzOf(f));
    }
    if (!pitches.empty())
    {
        double lo = percentile(pitches, 0.05), hi = percentile(pitches, 0.95);
        if (hi < lo * 2.0)
        {
            const double centre = std::sqrt(lo * hi);
            lo = centre / std::sqrt(2.0);
            hi = centre * std::sqrt(2.0);
        }
        strip.lowHz = lo;
        strip.highHz = hi;
    }
    if (!levels.empty()) strip.loudDbfs = percentile(levels, 0.95);

    if (columns <= 0) return strip;
    strip.columns.resize((size_t)columns);
    const uint64_t length = clip.length;
    if (length == 0) return strip;
    for (int c = 0; c < columns; c++)
    {
        StripColumn& col = strip.columns[(size_t)c];
        uint64_t a = length * (uint64_t)c / (uint64_t)columns;
        uint64_t b = std::max(a + 1, length * (uint64_t)(c + 1) / (uint64_t)columns);
        b = std::min(b, length);
        double logSum = 0.0, weightSum = 0.0;
        for (uint64_t i = a; i < b && i < clip.frames.size(); i++)
        {
            col.arrived = true;
            if (!clip.good[(size_t)i])
            {
                col.lost = true;
                continue;
            }
            const Frame& f = clip.frames[(size_t)i];
            if (isSilent(f)) continue;
            col.silent = false;
            const double db = energyDbfsOf(f);
            col.dbfs = std::max(col.dbfs, db);
            if (voicingOf(f) != Voicing::Unvoiced)
            {
                const double w = std::pow(10.0, db / 20.0);
                logSum += w * std::log(pitchHzOf(f));
                weightSum += w;
            }
        }
        if (weightSum > 0)
        {
            col.voiced = true;
            col.pitchHz = std::exp(logSum / weightSum);
        }
    }
    return strip;
}

uint32_t frameAt(const Clip& clip, double x, double width)
{
    if (clip.length == 0 || !(width > 0)) return 0;
    const double t = std::clamp(x / width, 0.0, 1.0);
    return std::min<uint32_t>(clip.length - 1, (uint32_t)(t * clip.length));
}

//-------------------------------------------------------------------------
// What a clip costs the air
//-------------------------------------------------------------------------

double broadcastAirSeconds(uint64_t bytes, const std::string& name, const std::string& callsign,
                           const link::ModeInfo& mode)
{
    return broadcast::estimateFile(bytes, name, callsign, mode).airSeconds;
}

double sessionAirSeconds(uint64_t bytes, double bytesPerSecond)
{
    if (!(bytesPerSecond > 0)) return 0.0;
    // As picture::sessionBytesFor counts it: the offer is about 40 bytes,
    // and each FileData carries 18 of its own ahead of at least 256 of the
    // file's.
    return (40.0 + (double)bytes * (256.0 + 18.0) / 256.0) / bytesPerSecond;
}

double playDelaySeconds(const std::vector<uint8_t>& file, double bytesPerSecond)
{
    Clip clip;
    if (readSot(file, clip) != ReadResult::Ok || !(bytesPerSecond > 0)) return -1.0;
    // sotto's decoder gives a frame's sound once it has the frame after,
    // so frame t (from 0) must be here by the time frame t - 1 plays.
    double delay = 0.0;
    uint64_t t = 0;
    const size_t coded = (file.size() - HEADER_BYTES) / FRAME_BYTES;
    for (size_t i = 0; i < coded; i++)
    {
        const size_t begin = HEADER_BYTES + i * FRAME_BYTES;
        Frame f;
        std::copy(file.begin() + (std::ptrdiff_t)begin, file.begin() + (std::ptrdiff_t)(begin + FRAME_BYTES),
                  f.begin());
        const double here = (double)(begin + FRAME_BYTES) / bytesPerSecond;
        const double plays = ((double)t - 1.0) * FRAME_MS / 1000.0;
        delay = std::max(delay, here - plays);
        t += isSilent(f) ? 1 + ((bitsOf(f) >> RUN_SHIFT) & MAX_RUN) : 1;
    }
    return delay;
}

} // namespace voice
} // namespace ragtime
