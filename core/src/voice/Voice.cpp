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

// sotto 1200's frame B energy step (bits 37 to 39), whose code 4 is a
// step of 0 from halfway between this A's energy and the next one's
// (sotto's c/sotto_lr_tables.h, LR_E_ZERO).
constexpr int LR_B_ENERGY_SHIFT = 8;
constexpr uint64_t LR_B_ENERGY_ZERO = 4;
// A silent superframe at 1,200: A's energy 0 and B's step 0, as sotto's
// encoder sends for digital silence (its other fields then carry the
// hiss band's guesses, which say nothing in silence).
constexpr uint64_t LR_SILENCE = LR_B_ENERGY_ZERO << LR_B_ENERGY_SHIFT;

// sotto 600 and 400 (sotto's c/sotto_vlr_tables.h): voicing pattern 3
// bits, pitch centre 7, pitch slope 3, energy mean 5, energy residual 5,
// then 25 bits of shape anchors.
constexpr int VLR_PATTERN_SHIFT = 45;
constexpr int VLR_PITCH_SHIFT = 38;
constexpr int VLR_MEAN_SHIFT = 30;
constexpr int VLR_RESIDUAL_SHIFT = 25;
// A silent superframe's run count, in the anchors.
constexpr int VLR_RUN_SHIFT = 9;
// The energy residual whose offsets from a mean of 0 leave every frame's
// energy code 0, so the superframe is silence: row 12 of sotto 600's
// table (-2, 0, 0, -2 sixteenths) and row 17 of sotto 400's (0, 0, -1, 1,
// 2, 1). sotto's encoder sends them for digital silence. Frozen with the
// codec numbers: new tables are a new codec.
constexpr uint64_t VLR600_SILENT_RESIDUAL = 12;
constexpr uint64_t VLR400_SILENT_RESIDUAL = 17;

// Every codec: number, bit/s, frame ms, coded rate, layout, lookahead,
// lost frames before quiet, name.
const Codec CODECS[] = {
    {CODEC_SOTTO_V36, 2400, 20, 8000, Layout::Frame20, 1, 3, "sotto v3.6"},
    {CODEC_SOTTO_V38, 2400, 20, 16000, Layout::Frame20, 1, 3, "sotto 2400"},
    {CODEC_SOTTO_1200, 1200, 40, 16000, Layout::Lr, 2, 2, "sotto 1200"},
    {CODEC_SOTTO_600, 600, 80, 16000, Layout::Vlr, 2, 1, "sotto 600"},
    {CODEC_SOTTO_400, 400, 120, 16000, Layout::Vlr, 2, 1, "sotto 400"},
};

uint64_t vlrSilentResidual(uint8_t codec)
{
    return codec == CODEC_SOTTO_400 ? VLR400_SILENT_RESIDUAL : VLR600_SILENT_RESIDUAL;
}

// Where a silent frame's run count sits.
int runShiftOf(const Codec& c)
{
    return c.layout == Layout::Vlr ? VLR_RUN_SHIFT : RUN_SHIFT;
}

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
// The codecs
//-------------------------------------------------------------------------

uint32_t Codec::maxFrames() const
{
    return MAX_CLIP_SECONDS * 1000u / (uint32_t)frameMs;
}

const Codec* codecOf(uint8_t number)
{
    for (const Codec& c : CODECS)
        if (c.number == number) return &c;
    return nullptr;
}

const std::vector<uint8_t>& sendCodecs()
{
    static const std::vector<uint8_t> codecs = {CODEC_SOTTO_V38, CODEC_SOTTO_1200, CODEC_SOTTO_600, CODEC_SOTTO_400};
    return codecs;
}

uint8_t codecForRate(int bitsPerSecond)
{
    for (uint8_t n : sendCodecs())
        if (codecOf(n)->bitsPerSecond == bitsPerSecond) return n;
    return 0;
}

bool isSilent(const Frame& frame, uint8_t codec)
{
    const Codec* c = codecOf(codec);
    if (!c || c->layout == Layout::Frame20) return isSilent(frame);
    const uint64_t v = bitsOf(frame);
    if (c->layout == Layout::Lr)
        return energyCodeOf(frame) == 0 && ((v >> LR_B_ENERGY_SHIFT) & 0x7) == LR_B_ENERGY_ZERO;
    return ((v >> VLR_MEAN_SHIFT) & 0x1F) == 0 && ((v >> VLR_RESIDUAL_SHIFT) & 0x1F) == vlrSilentResidual(codec);
}

Frame silentFrame(uint8_t codec)
{
    const Codec* c = codecOf(codec);
    if (c && c->layout == Layout::Vlr) return frameOf(vlrSilentResidual(codec) << VLR_RESIDUAL_SHIFT);
    if (c && c->layout == Layout::Lr) return frameOf(LR_SILENCE);
    return SILENT_FRAME;
}

FrameLook lookOf(const Frame& frame, uint8_t codec)
{
    FrameLook look;
    if (isSilent(frame, codec)) return look;
    const Codec* c = codecOf(codec);
    if (c && c->layout == Layout::Vlr)
    {
        const uint64_t v = bitsOf(frame);
        const int mean = (int)((v >> VLR_MEAN_SHIFT) & 0x1F);
        // Pattern 0 is unvoiced throughout, in both modes' tables; every
        // other pattern has buzz somewhere.
        look.voiced = ((v >> VLR_PATTERN_SHIFT) & 0x7) != 0;
        const double lo = std::log(PITCH_LOW_HZ), hi = std::log(PITCH_HIGH_HZ);
        look.pitchHz = std::exp(lo + (double)((v >> VLR_PITCH_SHIFT) & 0x7F) * (hi - lo) / 127.0);
        // A mean of 0 with another residual is a whisper of sound.
        look.dbfs = (mean == 0 ? fieldDbOf(1) : fieldDbOf(mean)) - ENERGY_OFFSET_DB;
        look.silent = false;
        return look;
    }
    // 2,400, and 1,200's frame A.
    look.silent = false;
    look.dbfs = energyCodeOf(frame) == 0 ? fieldDbOf(1) - ENERGY_OFFSET_DB : energyDbfsOf(frame);
    look.voiced = voicingOf(frame) != Voicing::Unvoiced;
    look.pitchHz = pitchHzOf(frame);
    return look;
}

//-------------------------------------------------------------------------
// The .sot file
//-------------------------------------------------------------------------

int Clip::frameMs() const
{
    const Codec* c = codecOf(codec);
    return c ? c->frameMs : FRAME_MS;
}

uint8_t codecOfFormat(const std::string& format)
{
    return format == SOTTO_V38_FORMAT ? CODEC_SOTTO_V38 : 0;
}

bool isSot(const uint8_t* bytes, size_t length)
{
    return length >= 4 && bytes[0] == MAGIC_0 && bytes[1] == MAGIC_1 && knownCodec(bytes[2]) &&
           (bytes[3] & ~KNOWN_FLAGS) == 0;
}

std::vector<Frame> gatePauses(const std::vector<Frame>& frames, const PauseGate& gate, uint8_t codec)
{
    std::vector<Frame> out = frames;
    const Codec* c = codecOf(codec);
    if (!gate.enabled || !c) return out;
    std::vector<double> level(frames.size(), SILENCE_DBFS);
    std::vector<double> sounding;
    for (size_t i = 0; i < frames.size(); i++)
    {
        const FrameLook look = lookOf(frames[i], codec);
        if (look.silent) continue;
        level[i] = look.dbfs;
        sounding.push_back(look.dbfs);
    }
    if (sounding.empty()) return out;
    const double threshold = percentile(sounding, 0.95) - gate.belowDb;
    const size_t n = frames.size();
    auto quiet = [&](size_t i) { return level[i] <= threshold; };
    // The gate's 20 ms frames in the codec's.
    auto frames20 = [&](int count) { return (count * FRAME_MS + c->frameMs - 1) / c->frameMs; };
    const size_t hang = gate.hangFrames > 0 ? (size_t)std::max(1, frames20(gate.hangFrames)) : 0;
    const size_t minRun = (size_t)std::max(1, frames20(gate.minFrames));
    const Frame silence = silentFrame(codec);
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
        if (atStart || atEnd || j - i >= minRun)
        {
            size_t a = atStart ? i : std::min(j, i + hang);
            size_t b = atEnd ? j : (j - i > hang ? j - hang : i);
            for (size_t k = a; k < b; k++) out[k] = silence;
        }
        i = j;
    }
    return out;
}

std::vector<Frame> trimSilence(const std::vector<Frame>& frames, uint8_t codec)
{
    size_t a = 0, b = frames.size();
    while (a < b && isSilent(frames[a], codec)) a++;
    while (b > a && isSilent(frames[b - 1], codec)) b--;
    return std::vector<Frame>(frames.begin() + (std::ptrdiff_t)a, frames.begin() + (std::ptrdiff_t)b);
}

std::vector<uint8_t> writeSot(const std::vector<Frame>& frames, uint8_t codec)
{
    const Codec* c = codecOf(codec);
    if (!c || frames.size() > c->maxFrames()) return {};
    const uint64_t silence = bitsOf(silentFrame(codec));
    const int runShift = runShiftOf(*c);
    const uint32_t length = (uint32_t)frames.size();
    std::vector<uint8_t> out = {MAGIC_0, MAGIC_1, codec, 0, (uint8_t)(length >> 16), (uint8_t)(length >> 8),
                                (uint8_t)length};
    size_t i = 0;
    while (i < frames.size())
    {
        if (!isSilent(frames[i], codec))
        {
            out.insert(out.end(), frames[i].begin(), frames[i].end());
            i++;
            continue;
        }
        size_t j = i;
        while (j < frames.size() && isSilent(frames[j], codec)) j++;
        size_t run = j - i;
        while (run > 0)
        {
            size_t take = std::min<size_t>(run, (size_t)MAX_RUN + 1);
            Frame head = frameOf(silence | (uint64_t)(take - 1) << runShift);
            out.insert(out.end(), head.begin(), head.end());
            run -= take;
        }
        i = j;
    }
    return out;
}

std::vector<uint8_t> makeSot(const std::vector<Frame>& frames, const PauseGate& gate, uint8_t codec)
{
    std::vector<Frame> clip = trimSilence(gatePauses(frames, gate, codec), codec);
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
    const Codec* codec = codecOf(clip.codec);
    if (!codec) return ReadResult::UnknownCodec;
    if ((clip.flags & ~KNOWN_FLAGS) != 0) return ReadResult::UnknownFlags;
    if (clip.length > codec->maxFrames()) return ReadResult::TooLong;
    const Frame silence = silentFrame(clip.codec);
    const int runShift = runShiftOf(*codec);
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
            const bool silent = isSilent(f, clip.codec);
            const size_t count = silent ? 1 + (size_t)((bitsOf(f) >> runShift) & MAX_RUN) : 1;
            if (total + count > clip.length) return ReadResult::Malformed;
            frames.insert(frames.end(), count, silent ? silence : f);
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
            clip.frames.insert(clip.frames.end(), lost, silence);
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
        if (!clip.good[i]) continue;
        const FrameLook f = lookOf(clip.frames[i], clip.codec);
        if (f.silent) continue;
        levels.push_back(f.dbfs);
        if (f.voiced) pitches.push_back(f.pitchHz);
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
            const FrameLook f = lookOf(clip.frames[(size_t)i], clip.codec);
            if (f.silent) continue;
            col.silent = false;
            col.dbfs = std::max(col.dbfs, f.dbfs);
            if (f.voiced)
            {
                const double w = std::pow(10.0, f.dbfs / 20.0);
                logSum += w * std::log(f.pitchHz);
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
    // sotto's decoder gives a frame's sound once it has the frame after
    // (two after, at 1,200 and slower), so frame t (from 0) must be here
    // by the time frame t - lookahead plays.
    const Codec& codec = *codecOf(clip.codec);
    const int runShift = runShiftOf(codec);
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
        const double plays = ((double)t - codec.lookahead) * codec.frameSeconds();
        delay = std::max(delay, here - plays);
        t += isSilent(f, clip.codec) ? 1 + ((bitsOf(f) >> runShift) & MAX_RUN) : 1;
    }
    return delay;
}

} // namespace voice
} // namespace ragtime
