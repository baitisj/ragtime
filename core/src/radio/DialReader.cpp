#include "ragtime/radio/DialReader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <string>

namespace ragtime
{
namespace radio
{

namespace
{

// Hamlib's names for the meters, in Meter's order.
const char* const LEVELS[DialReader::METERS] = {"ALC",      "SWR",    "RFPOWER_METER_WATTS", "RFPOWER_METER",
                                                 "STRENGTH", "PBT_IN", "PBT_OUT"};

// A number on a line of its own: digits, perhaps signed, perhaps with a
// decimal part ("14074000", "14074000.000000", "0.593750").
bool numberLine(const std::string& line, double& value)
{
    if (line.empty() || line.size() > 24) return false;
    size_t digits = 0;
    bool point = false;
    for (size_t i = 0; i < line.size(); i++)
    {
        char c = line[i];
        if (c >= '0' && c <= '9')
        {
            digits++;
        }
        else if (c == '.' && !point)
        {
            point = true;
        }
        else if (!(c == '-' && i == 0))
        {
            return false;
        }
    }
    if (digits == 0) return false;
    value = std::strtod(line.c_str(), nullptr);
    return true;
}

// A frequency line: a whole number of Hz under a terahertz, perhaps with
// a decimal part from some Hamlib versions. Anything under 1 kHz is no
// dial (a late "1" from \chk_vfo, a meter's value).
bool frequencyLine(const std::string& line, uint64_t& hz)
{
    double value = 0;
    if (!numberLine(line, value) || line[0] == '-') return false;
    if (value < 1000.0 || value >= 1e12) return false;
    hz = (uint64_t)(value + 0.5);
    return true;
}

bool rprt(const std::string& line, int& code)
{
    if (line.compare(0, 5, "RPRT ") != 0) return false;
    code = std::atoi(line.c_str() + 5);
    return true;
}

// Hamlib's answers to a question it doesn't know or the rig can't answer:
// RIG_EINVAL, RIG_ENIMPL, RIG_ENAVAIL.
bool unknownCode(int code)
{
    return code == -1 || code == -4 || code == -11;
}

} // namespace

void DialReader::connected(uint64_t nowMs)
{
    connected_ = true;
    partial_.clear();
    out_.clear();
    problem_.clear();
    readings_.clear();
    strength_.clear();
    strengthDueMs_ = 0;
    freqAsked_ = false;
    answering_ = -1;
    for (int m = 0; m < METERS; m++)
    {
        state_[m] = State::Unknown;
        failures_[m] = 0;
    }
    probe_ = 0;
    filterStep_ = 0;
    filterDueMs_ = 0;
    inMode_ = false;
    modeDropped_ = false;
    modeFailures_ = 0;
    filter_.reset();
    reading_.reset();
    setLines_.clear();
    setLine_.clear();
    version_++;
    // First: was rigctld started with --vfo? Asked even while keyed, as
    // it touches nothing but rigctld itself.
    vfo_ = Vfo::Asking;
    out_ += "\\chk_vfo\n";
    asking_ = Ask::Vfo;
    askedMs_ = nowMs;
}

void DialReader::disconnected()
{
    connected_ = false;
    asking_ = Ask::None;
    answering_ = -1;
    partial_.clear();
    out_.clear();
    hz_.reset();
    filter_.reset();
    reading_.reset();
    inMode_ = false;
    for (int m = 0; m < METERS; m++) state_[m] = State::Unknown;
    version_++;
}

bool DialReader::askable(int meter) const
{
    if (state_[meter] == State::Dropped) return false;
    // A share of full power only from a rig that knows no watts.
    if (meter == (int)Meter::Power) return state_[(int)Meter::Watts] == State::Dropped;
    return true;
}

int DialReader::pickMeter()
{
    int swr = (int)Meter::Swr;
    int count = 0;
    for (int m = 0; m < METERS; m++) count += transmitMeter(m) ? 1 : 0;
    if (count == 0) return -1;
    if (swrWatch_ && transmitMeter(swr) && count > 1)
    {
        swrTurn_ = !swrTurn_;
        if (swrTurn_) return swr;
    }
    for (int i = 0; i < METERS; i++)
    {
        int m = turn_++ % METERS;
        if (transmitMeter(m) && !(swrWatch_ && m == swr && count > 1)) return m;
    }
    return swr;
}

void DialReader::askMeter(int meter, uint64_t nowMs)
{
    // Questions only: "l" gets a level, and nothing here sets the radio.
    out_ += std::string("+l ") + (vfo_ == Vfo::Yes ? "currVFO " : "") + LEVELS[meter] + "\n";
    asking_ = Ask::Meter;
    askedMeter_ = meter;
    askedMs_ = nowMs;
    if (meter == (int)Meter::Strength) strengthDueMs_ = nowMs + STRENGTH_MS;
}

void DialReader::askNext(uint64_t nowMs)
{
    if (!connected_ || asking_ != Ask::None || vfo_ == Vfo::Asking) return;
    if (keyed_)
    {
        if (nowMs < meterDueMs_) return;
        int meter = pickMeter();
        if (meter >= 0) askMeter(meter, nowMs);
        return;
    }
    // Which meters the rig reports, once, before the dial.
    while (probe_ < PROBED)
    {
        int meter = probe_++;
        if (askable(meter) && state_[meter] == State::Unknown)
        {
            askMeter(meter, nowMs);
            return;
        }
    }
    // A filter setting from the Ear, never while keyed (above).
    if (writeFilter(nowMs)) return;
    if (freqAsked_ && nowMs < freqAskedMs_ + ASK_MS)
    {
        // The band's noise, between dial readings, once the receiver has
        // settled after a burst.
        int strength = (int)Meter::Strength;
        if (askable(strength) && nowMs >= strengthDueMs_ && nowMs >= unkeyedMs_ + SETTLE_MS)
        {
            askMeter(strength, nowMs);
            return;
        }
        // The filter, while the Ear is open.
        askFilter(nowMs);
        return;
    }
    // Frequency queries only.
    out_ += vfo_ == Vfo::Yes ? "f currVFO\n" : "f\n";
    asking_ = Ask::Freq;
    askedMs_ = freqAskedMs_ = nowMs;
    freqAsked_ = true;
}

bool DialReader::askFilter(uint64_t nowMs)
{
    if (!filterWatch_ || nowMs < filterDueMs_) return false;
    while (filterStep_ < 3)
    {
        const int step = filterStep_++;
        if (step == 0)
        {
            if (modeDropped_) continue;
            // Questions only: "m" gets the mode, and nothing here sets it.
            out_ += vfo_ == Vfo::Yes ? "+m currVFO\n" : "+m\n";
            asking_ = Ask::Mode;
            askedMs_ = nowMs;
            modeName_.clear();
            return true;
        }
        const int meter = step == 1 ? (int)Meter::PbtIn : (int)Meter::PbtOut;
        if (askable(meter))
        {
            askMeter(meter, nowMs);
            return true;
        }
    }
    filterStep_ = 0;
    filterDueMs_ = nowMs + FILTER_MS;
    return false;
}

bool DialReader::filterQuestionsLeft() const
{
    for (int step = filterStep_; step < 3; step++)
        if (step == 0 ? !modeDropped_ : askable(step == 1 ? (int)Meter::PbtIn : (int)Meter::PbtOut)) return true;
    return false;
}

void DialReader::publishFilter()
{
    if (reading_ && filter_ != reading_)
    {
        filter_ = reading_;
        version_++;
    }
    reading_.reset();
}

void DialReader::setFilter(const RigFilter& filter, uint64_t nowMs)
{
    filterWrite_ = filter;
    askNext(nowMs);
}

void DialReader::flushFilter()
{
    if (!connected_) return;
    queueFilter();
    // rigctld takes them in turn; the answers come after we have gone.
    for (const std::string& line : setLines_) out_ += line + "\n";
    setLines_.clear();
}

void DialReader::queueFilter()
{
    if (setLines_.empty() && filterWrite_)
    {
        // The commands, worked out when it goes: the mode and width only
        // when they differ from what the rig said last.
        const RigFilter& f = *filterWrite_;
        const std::string vfo = vfo_ == Vfo::Yes ? "currVFO " : "";
        auto number = [](double v) {
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out.setf(std::ios::fixed);
            out.precision(4);
            out << v;
            return out.str();
        };
        if (!f.mode.empty() && f.widthHz > 0 &&
            (!filter_ || filter_->mode != f.mode || filter_->widthHz != f.widthHz))
            setLines_.push_back("M " + vfo + f.mode + " " + std::to_string(f.widthHz));
        if (f.pbtIn) setLines_.push_back("L " + vfo + "PBT_IN " + number(*f.pbtIn));
        if (f.pbtOut) setLines_.push_back("L " + vfo + "PBT_OUT " + number(*f.pbtOut));
        filterWrite_.reset();
        filterProblem_.clear();
        // A reading begun before is of the old setting: start afresh after.
        reading_.reset();
        filterStep_ = 0;
        if (!setLines_.empty()) filterWrites_++;
        version_++;
    }
}

bool DialReader::writeFilter(uint64_t nowMs)
{
    queueFilter();
    if (setLines_.empty()) return false;
    setLine_ = setLines_.front();
    setLines_.erase(setLines_.begin());
    out_ += setLine_ + "\n";
    asking_ = Ask::Set;
    askedMs_ = nowMs;
    if (setLines_.empty())
    {
        // Read back at once, while the Ear watches.
        filterStep_ = 0;
        filterDueMs_ = nowMs;
    }
    return true;
}

void DialReader::setFilterWatch(bool on, uint64_t nowMs)
{
    if (filterWatch_ == on) return;
    filterWatch_ = on;
    filterStep_ = 0;
    filterDueMs_ = nowMs;
    reading_.reset();
    if (!on && filter_)
    {
        filter_.reset();
        version_++;
    }
    askNext(nowMs);
}

std::string DialReader::takeOutput()
{
    std::string out;
    out.swap(out_);
    return out;
}

std::vector<double> DialReader::takeStrength()
{
    std::vector<double> out;
    out.swap(strength_);
    return out;
}

std::vector<MeterReading> DialReader::takeReadings()
{
    std::vector<MeterReading> out;
    out.swap(readings_);
    return out;
}

void DialReader::tick(uint64_t nowMs)
{
    if (!connected_) return;
    if (asking_ != Ask::None && nowMs >= askedMs_ + ANSWER_MS)
    {
        // No answer: given up. No answer to \chk_vfo: no VFO names. A
        // late answer is still read for what it says.
        if (asking_ == Ask::Vfo) vfoAnswer(std::string());
        if (asking_ == Ask::Mode) inMode_ = false;
        asking_ = Ask::None;
        meterDueMs_ = nowMs;
    }
    if (!keyed_ && hz_ && nowMs > readMs_ + STALE_MS)
    {
        hz_.reset();
        problem_ = "rigctld hasn't answered for " + std::to_string(STALE_MS / 1000) + " s";
        version_++;
    }
    askNext(nowMs);
}

void DialReader::setKeyed(bool keyed, uint64_t nowMs)
{
    if (keyed_ == keyed) return;
    keyed_ = keyed;
    meterDueMs_ = nowMs;
    // The reading from before the burst stands through it, and for a
    // moment after while the next answer comes.
    if (!keyed_)
    {
        if (hz_) readMs_ = nowMs;
        freqAsked_ = false;
        unkeyedMs_ = nowMs;
    }
    askNext(nowMs);
}

void DialReader::bytes(const char* bytes, size_t length, uint64_t nowMs)
{
    for (size_t i = 0; i < length; i++)
    {
        char c = bytes[i];
        if (c == '\n')
        {
            std::string line;
            line.swap(partial_);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            onLine(line, nowMs);
        }
        else if (partial_.size() < 256)
        {
            partial_ += c;
        }
    }
}

void DialReader::vfoAnswer(const std::string& line)
{
    // "1" or "0"; "CHKVFO 1" from Hamlib 3; an RPRT, or nothing, from a
    // rigctld that doesn't know the question.
    std::string answer = line.compare(0, 7, "CHKVFO ") == 0 ? line.substr(7) : line;
    vfo_ = answer == "1" ? Vfo::Yes : Vfo::No;
    version_++;
}

void DialReader::answered(uint64_t nowMs)
{
    // The meters come METER_MS apart; the first of a burst at once.
    if (asking_ == Ask::Meter) meterDueMs_ = nowMs + METER_MS;
    // The filter's last answer: the whole reading at once, never the mode
    // of one with the knobs of the last.
    const bool filterAnswer = asking_ == Ask::Mode || (asking_ == Ask::Meter && (askedMeter_ == (int)Meter::PbtIn ||
                                                                                askedMeter_ == (int)Meter::PbtOut));
    if (filterAnswer && !filterQuestionsLeft()) publishFilter();
    asking_ = Ask::None;
    askNext(nowMs);
}

void DialReader::meterValue(int meter, double value)
{
    // Nonsense (a negative SWR) is no reading.
    bool strength = meter == (int)Meter::Strength;
    bool pbt = meter == (int)Meter::PbtIn || meter == (int)Meter::PbtOut;
    if (pbt ? std::fabs(value) > 10000
            : strength ? (value < -100 || value > 100) : (value < 0 || (meter == (int)Meter::Alc && value > 2.0)))
        return;
    failures_[meter] = 0;
    if (state_[meter] != State::Answers)
    {
        state_[meter] = State::Answers;
        version_++;
    }
    if (pbt)
    {
        if (!filterWatch_) return;
        if (!reading_) reading_ = filter_.value_or(RigFilter{});
        (meter == (int)Meter::PbtIn ? reading_->pbtIn : reading_->pbtOut) = value;
        return;
    }
    if (strength)
    {
        // Taken while transmitting it would be our own signal.
        if (!keyed_) strength_.push_back(value);
        return;
    }
    if (!keyed_) return;
    MeterReading reading;
    switch ((Meter)meter)
    {
    case Meter::Alc: reading.alc = value; break;
    case Meter::Swr: reading.swr = value; break;
    case Meter::Watts: reading.watts = value; break;
    case Meter::Power: reading.power = value; break;
    case Meter::Strength:
    case Meter::PbtIn:
    case Meter::PbtOut: break;
    }
    readings_.push_back(reading);
}

void DialReader::meterFailed(int meter, int code)
{
    // The probe, not keyed: a level this Hamlib or this rig doesn't know
    // (RIG_EINVAL, RIG_ENIMPL, RIG_ENAVAIL). Anything else may be the rig
    // not transmitting, so it is asked again when keyed.
    // The S-meter is asked between bursts, so its failures count then.
    // So are the PBT.
    bool unknown = unknownCode(code);
    bool counts = keyed_ || meter >= (int)Meter::Strength;
    if ((!keyed_ && unknown) || (counts && ++failures_[meter] >= 2))
    {
        if (state_[meter] != State::Dropped)
        {
            state_[meter] = State::Dropped;
            version_++;
        }
        // No watts: the share of full power instead, probed afresh.
        if (meter == (int)Meter::Watts && probe_ > (int)Meter::Power) probe_ = (int)Meter::Power;
    }
}

void DialReader::onLine(const std::string& line, uint64_t nowMs)
{
    if (line.empty()) return;
    int code = 0;

    // The mode's long answer: "get_mode:" (or "get_mode: currVFO"),
    // "Mode: USB", "Passband: 2400", then RPRT; its short one the mode
    // and the width on lines of their own.
    if (line.compare(0, 9, "get_mode:") == 0)
    {
        inMode_ = true;
        modeName_.clear();
        return;
    }
    if (inMode_ || (asking_ == Ask::Mode && answering_ < 0 && line.compare(0, 11, "get_level: ") != 0))
    {
        modeLine(line, nowMs);
        return;
    }

    // A long answer: "get_level: SWR" (or "get_level: currVFO SWR"), the
    // value, then RPRT.
    if (line.compare(0, 11, "get_level: ") == 0)
    {
        std::string name = line.substr(line.rfind(' ') + 1);
        answering_ = -1;
        for (int m = 0; m < METERS; m++)
        {
            if (name == LEVELS[m]) answering_ = m;
        }
        return;
    }
    if (answering_ >= 0)
    {
        double value = 0;
        if (rprt(line, code))
        {
            if (code != 0) meterFailed(answering_, code);
            bool ours = asking_ == Ask::Meter && answering_ == askedMeter_;
            answering_ = -1;
            if (ours) answered(nowMs);
        }
        else if (numberLine(line, value))
        {
            meterValue(answering_, value);
        }
        return;
    }

    if (asking_ == Ask::Vfo)
    {
        vfoAnswer(line);
        answered(nowMs);
        return;
    }
    // A meter's answer that came without its name (a rigctld that ignores
    // the +): its value means nothing, but its RPRT ends it.
    if (asking_ == Ask::Meter)
    {
        if (rprt(line, code))
        {
            if (code != 0) meterFailed(askedMeter_, code);
            answered(nowMs);
        }
        return;
    }

    // A filter setting's answer.
    if (asking_ == Ask::Set && rprt(line, code))
    {
        if (code != 0)
        {
            // The rest would land on a setting half made: stop, and read
            // what the rig has now.
            filterProblem_ = "rigctld refused " + setLine_ + ": " + line;
            setLines_.clear();
            filterStep_ = 0;
            filterDueMs_ = nowMs;
            version_++;
        }
        answered(nowMs);
        return;
    }

    uint64_t hz = 0;
    if (frequencyLine(line, hz))
    {
        readMs_ = nowMs;
        if (hz_ != hz || !problem_.empty()) version_++;
        hz_ = hz;
        problem_.clear();
        if (asking_ == Ask::Freq) answered(nowMs);
        return;
    }
    if (rprt(line, code))
    {
        // "RPRT 0" follows a set command; ours are taken above.
        if (code != 0 && problem_ != "rigctld answered " + line)
        {
            problem_ = "rigctld answered " + line;
            version_++;
        }
        if (asking_ == Ask::Freq) answered(nowMs);
    }
}

void DialReader::modeLine(const std::string& line, uint64_t nowMs)
{
    int code = 0;
    double width = 0;
    bool done = false;
    bool ok = false;
    if (rprt(line, code))
    {
        done = true;
        ok = code == 0 && !modeName_.empty();
        if (code != 0 && (unknownCode(code) || ++modeFailures_ >= 3)) modeDropped_ = true;
    }
    else if (inMode_ && line.compare(0, 6, "Mode: ") == 0)
    {
        modeName_ = line.substr(6);
        modeWidth_ = 0;
    }
    else if (inMode_ && line.compare(0, 10, "Passband: ") == 0)
    {
        modeWidth_ = std::atoi(line.c_str() + 10);
    }
    else if (!inMode_ && modeName_.empty())
    {
        // A late dial isn't a mode.
        if (numberLine(line, width)) return;
        modeName_ = line;
        modeWidth_ = 0;
    }
    else if (!inMode_ && numberLine(line, width))
    {
        modeWidth_ = (int)width;
        done = ok = true;
    }
    if (!done) return;

    inMode_ = false;
    if (ok && filterWatch_)
    {
        modeFailures_ = 0;
        if (!reading_) reading_ = filter_.value_or(RigFilter{});
        reading_->mode = modeName_;
        reading_->widthHz = std::max(0, modeWidth_);
    }
    modeName_.clear();
    if (asking_ == Ask::Mode) answered(nowMs);
}

std::optional<uint64_t> DialReader::dialHz(uint64_t nowMs) const
{
    if (!connected_ || !hz_) return std::nullopt;
    if (!keyed_ && nowMs > readMs_ + STALE_MS) return std::nullopt;
    return hz_;
}

} // namespace radio
} // namespace ragtime
