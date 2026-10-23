// KISS, command-line and mode formats for talking to data2g-host. Ported
// from Glissando's Data2GLink; see the header.
#include "ragtime/link/Data2GLink.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ragtime
{
namespace link
{

namespace
{

constexpr uint8_t FEND = 0xC0;
constexpr uint8_t FESC = 0xDB;
constexpr uint8_t TFEND = 0xDC;
constexpr uint8_t TFESC = 0xDD;

std::string upper(std::string text)
{
    for (char& c : text) c = (char)std::toupper((unsigned char)c);
    return text;
}

std::vector<std::string> words(const std::string& line)
{
    std::istringstream in(line);
    std::vector<std::string> out;
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

bool toInt(const std::string& text, int& out)
{
    if (text.empty()) return false;
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = (int)value;
    return true;
}

bool toDouble(const std::string& text, double& out)
{
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0';
}

// An SNR at the end of a report: a finite number with no letter in it, so
// never a callsign (a callsign always has a letter) nor "inf" or "nan".
std::optional<double> trailingSnr(const std::string& word)
{
    if (std::any_of(word.begin(), word.end(), [](char c) { return std::isalpha((unsigned char)c) != 0; }))
        return std::nullopt;
    double dB = 0.0;
    if (!toDouble(word, dB) || !std::isfinite(dB)) return std::nullopt;
    return dB;
}

void appendEscaped(std::vector<uint8_t>& out, uint8_t b)
{
    if (b == FEND)
    {
        out.push_back(FESC);
        out.push_back(TFEND);
    }
    else if (b == FESC)
    {
        out.push_back(FESC);
        out.push_back(TFESC);
    }
    else
    {
        out.push_back(b);
    }
}

std::vector<uint8_t> kissFrame(uint8_t commandByte, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> out;
    out.reserve(body.size() + 8);
    out.push_back(FEND);
    appendEscaped(out, commandByte);
    for (uint8_t b : body) appendEscaped(out, b);
    out.push_back(FEND);
    return out;
}

} // namespace

std::vector<uint8_t> kissEncode(int port, const std::vector<uint8_t>& data)
{
    return kissFrame((uint8_t)(((port & 0x0F) << 4) | KISS_DATA), data);
}

std::vector<uint8_t> kissEncodeAckMode(int port, uint16_t tag, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> body;
    body.reserve(data.size() + 2);
    body.push_back((uint8_t)(tag >> 8));
    body.push_back((uint8_t)(tag & 0xFF));
    body.insert(body.end(), data.begin(), data.end());
    return kissFrame((uint8_t)(((port & 0x0F) << 4) | KISS_ACKMODE), body);
}

void KissDecoder::feed(const uint8_t* bytes, size_t length, std::vector<KissFrame>& framesOut)
{
    for (size_t i = 0; i < length; i++)
    {
        uint8_t b = bytes[i];
        if (b == FEND)
        {
            if (inFrame_ && current_.size() > 1)
            {
                KissFrame frame;
                frame.port = current_[0] >> 4;
                frame.command = current_[0] & 0x0F;
                frame.payload.assign(current_.begin() + 1, current_.end());
                if (frame.command == KISS_DATA || frame.command == KISS_ACKMODE) framesOut.push_back(frame);
            }
            current_.clear();
            escaped_ = false;
            inFrame_ = true;
            continue;
        }
        if (!inFrame_) continue;

        if (escaped_)
        {
            escaped_ = false;
            if (b == TFEND) current_.push_back(FEND);
            else if (b == TFESC) current_.push_back(FESC);
            else current_.push_back(b); // a protocol error; keep the byte as sent
            continue;
        }
        if (b == FESC)
        {
            escaped_ = true;
            continue;
        }
        current_.push_back(b);
    }
}

void KissDecoder::reset()
{
    current_.clear();
    escaped_ = false;
    inFrame_ = false;
}

double ModeInfo::burstSeconds(const std::vector<int>& frameBytes) const
{
    if (!valid()) return 0.0;

    int bytes = 0;
    for (int length : frameBytes) bytes += 2 + length;
    int codewords = 1 + (bytes + bytesPerCodeword - 1) / bytesPerCodeword;
    codewords = std::min(codewords, maxCodewords);
    if (maxCodewords <= 1) return secondsAtOne;
    return secondsAtOne + (secondsAtMax - secondsAtOne) * (codewords - 1) / (maxCodewords - 1);
}

const std::vector<ModeInfo>& knownModes()
{
    // From a 2400 Hz data2g-host's MODES (Data2G 8ffe15c, 2026-10-07, as
    // Glissando measured them).
    static const std::vector<ModeInfo> modes = {
        {"n4-qpsk-r1/5", 200, 22, 32, 4.14, 111.27},
        {"fsk16r25-r1/2", 500, 58, 9, 4.98, 89.22},
        {"fsk16r25-r1/3", 500, 38, 9, 4.98, 89.22},
        {"fsk8r50-r1/2", 500, 58, 9, 3.62, 59.62},
        {"fsk8r50-r1/3", 500, 38, 9, 3.62, 59.62},
        {"n10-16qam-r1/2", 500, 121, 32, 2.12, 46.76},
        {"n10-qpsk-r1/2", 500, 60, 32, 2.12, 46.76},
        {"n10-qpsk-r1/3", 500, 40, 32, 2.12, 46.76},
        {"n10-qpsk-r1/5", 500, 23, 32, 2.12, 46.76},
        {"qpsk-r1/3", 1200, 76, 64, 1.78, 74.36},
        {"fsk32r62-r1/3", 2300, 38, 9, 2.42, 29.30},
        {"w48-16qam-r1/2", 2400, 236, 64, 1.21, 37.50},
        {"w48-qpsk-r1/2", 2400, 116, 64, 1.21, 37.50},
    };
    return modes;
}

std::vector<ModeInfo> narrowModes(const std::vector<ModeInfo>& offered)
{
    const std::vector<ModeInfo>& modes = offered.empty() ? knownModes() : offered;
    std::vector<ModeInfo> narrow;
    for (const ModeInfo& mode : modes)
    {
        if (mode.bandwidthHz > 0 && mode.bandwidthHz <= NARROW_MODE_MAX_HZ) narrow.push_back(mode);
    }
    return narrow;
}

CommandEvent parseCommandLine(const std::string& line)
{
    CommandEvent event;
    std::vector<std::string> w = words(line);
    if (w.empty()) return event;

    std::string verb = upper(w[0]);
    std::string arg1 = w.size() > 1 ? upper(w[1]) : std::string();

    if (verb == "OK" && w.size() == 1)
    {
        event.type = CommandEvent::Type::Ok;
    }
    else if (verb == "WRONG")
    {
        event.type = CommandEvent::Type::Wrong;
    }
    else if ((verb == "PTT" || verb == "BUSY") && w.size() == 2 && (arg1 == "ON" || arg1 == "OFF"))
    {
        event.type = verb == "PTT" ? CommandEvent::Type::Ptt : CommandEvent::Type::Busy;
        event.on = arg1 == "ON";
    }
    else if (verb == "MODE" && w.size() == 2)
    {
        event.type = CommandEvent::Type::Mode;
        event.text = w[1];
    }
    else if (verb == "MODE" && w.size() == 7)
    {
        ModeInfo mode;
        mode.name = w[1];
        double bandwidth = 0.0;
        if (toDouble(w[2], bandwidth) && toInt(w[3], mode.bytesPerCodeword) && toInt(w[4], mode.maxCodewords) &&
            toDouble(w[5], mode.secondsAtOne) && toDouble(w[6], mode.secondsAtMax))
        {
            mode.bandwidthHz = (int)std::lround(bandwidth);
            event.type = CommandEvent::Type::ModeLine;
            event.modeInfo = mode;
        }
    }
    else if (verb == "BCAST" && w.size() == 3 && arg1 == "PORT" && toInt(w[2], event.number))
    {
        event.type = CommandEvent::Type::BcastPort;
    }
    else if (verb == "BCAST" && w.size() >= 3 && toInt(w[1], event.number))
    {
        std::string what = upper(w[2]);
        if (what == "HEARD")
        {
            // BCAST n HEARD [call] [snr]: the SNR, when there, is the last word.
            event.type = CommandEvent::Type::BcastHeard;
            size_t end = w.size();
            if (end > 3 && (event.snr = trailingSnr(w[end - 1]))) end--;
            if (end > 3) event.text = w[3];
        }
        else if ((what == "LOST" || what == "DROPPED") && w.size() == 4 && toInt(w[3], event.count))
        {
            event.type = what == "LOST" ? CommandEvent::Type::BcastLost : CommandEvent::Type::BcastDropped;
        }
    }
    else if (verb == "BCAST" && w.size() >= 5 && w[1] == "*" && upper(w[2]) == "MISSED" && toInt(w[4], event.count))
    {
        // BCAST * MISSED submode n_cw [snr]
        event.type = CommandEvent::Type::BcastMissed;
        event.text = w[3];
        if (w.size() > 5) event.snr = trailingSnr(w[5]);
    }
    else if (verb == "SN" && w.size() == 2 && (event.snr = trailingSnr(w[1])))
    {
        event.type = CommandEvent::Type::Sn;
    }
    else if (verb == "CONNECTED" && w.size() >= 3)
    {
        event.type = CommandEvent::Type::Connected;
        event.text = upper(w[1]);
        event.peer = upper(w[2]);
        if (w.size() > 3) toInt(w[3], event.number);
    }
    else if (verb == "DISCONNECTED")
    {
        event.type = CommandEvent::Type::Disconnected;
    }
    else if (verb == "BUFFER" && w.size() == 2 && toInt(w[1], event.count))
    {
        event.type = CommandEvent::Type::Buffer;
    }
    else if (verb == "CQFRAME")
    {
        event.type = CommandEvent::Type::Cqframe;
    }
    return event;
}

void LineSplitter::feed(const char* bytes, size_t length, std::vector<std::string>& linesOut)
{
    for (size_t i = 0; i < length; i++)
    {
        char c = bytes[i];
        if (c == '\r' || c == '\n')
        {
            if (!current_.empty()) linesOut.push_back(current_);
            current_.clear();
        }
        else if (current_.size() < 1024)
        {
            current_.push_back(c);
        }
    }
}

std::string commandCallsign(const std::string& callsign)
{
    std::string out;
    for (char c : upper(callsign))
    {
        if (out.size() == 10) break;
        if (std::isalnum((unsigned char)c) || c == '/' || c == '-') out.push_back(c);
    }
    return out;
}

} // namespace link
} // namespace ragtime
