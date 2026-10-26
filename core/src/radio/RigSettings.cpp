#include "ragtime/radio/RigSettings.h"

#include <cstddef>

namespace ragtime
{
namespace radio
{

namespace
{

template <typename E, size_t N>
const char* wordOf(E value, const char* const (&words)[N])
{
    size_t i = (size_t)value;
    return i < N ? words[i] : words[0];
}

template <typename E, size_t N>
E fromWord(const std::string& word, const char* const (&words)[N])
{
    for (size_t i = 0; i < N; i++)
    {
        if (word == words[i]) return static_cast<E>(i);
    }
    return static_cast<E>(0);
}

// In each enum's order.
const char* const SOURCES[] = {"none", "rigctld", "hamlib"};
const char* const PTTS[] = {"cat-data", "cat-mic", "dtr", "rts"};
const char* const PARITIES[] = {"", "None", "Odd", "Even", "Mark", "Space"};
const char* const HANDSHAKES[] = {"", "None", "XONXOFF", "Hardware"};
const char* const LINES[] = {"", "ON", "OFF"};

bool separator(char c)
{
    return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '\n' || c == '\r';
}

} // namespace

const char* sourceWord(RigSource source)
{
    return wordOf(source, SOURCES);
}

RigSource sourceFromWord(const std::string& word)
{
    return fromWord<RigSource>(word, SOURCES);
}

const char* pttWord(PttMethod ptt)
{
    return wordOf(ptt, PTTS);
}

PttMethod pttFromWord(const std::string& word)
{
    return fromWord<PttMethod>(word, PTTS);
}

const char* parityWord(Parity parity)
{
    return wordOf(parity, PARITIES);
}

Parity parityFromWord(const std::string& word)
{
    return fromWord<Parity>(word, PARITIES);
}

const char* handshakeWord(Handshake handshake)
{
    return wordOf(handshake, HANDSHAKES);
}

Handshake handshakeFromWord(const std::string& word)
{
    return fromWord<Handshake>(word, HANDSHAKES);
}

const char* lineWord(LineState state)
{
    return wordOf(state, LINES);
}

LineState lineFromWord(const std::string& word)
{
    return fromWord<LineState>(word, LINES);
}

const char* pttLabel(PttMethod ptt)
{
    switch (ptt)
    {
    case PttMethod::CatData: return "CAT, data audio (rear port)";
    case PttMethod::CatMic: return "CAT, the radio's usual audio";
    case PttMethod::Dtr: return "DTR";
    case PttMethod::Rts: return "RTS";
    }
    return "";
}

bool parseBackendConf(const std::string& text, std::vector<std::pair<std::string, std::string>>& out,
                      std::string& problem)
{
    out.clear();
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && separator(text[i])) i++;
        if (i == text.size()) break;
        size_t end = i;
        while (end < text.size() && !separator(text[end])) end++;
        std::string word = text.substr(i, end - i);
        i = end;
        size_t equals = word.find('=');
        if (equals == std::string::npos || equals == 0)
        {
            problem = "\"" + word + "\" isn't a backend setting: write name=value, such as civaddr=0x88.";
            out.clear();
            return false;
        }
        out.push_back({word.substr(0, equals), word.substr(equals + 1)});
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> hamlibConf(const RigSettings& s, PortType port, std::string& problem)
{
    std::vector<std::pair<std::string, std::string>> conf;
    std::vector<std::pair<std::string, std::string>> backend;
    if (!parseBackendConf(s.backendConf, backend, problem)) return conf;
    if (s.port.empty() && port != PortType::None)
    {
        problem = port == PortType::Network ? "No address for the rig: write host:port."
                                            : "No port for the rig: name its serial port.";
        return conf;
    }
    if (port != PortType::None) conf.push_back({"rig_pathname", s.port});
    if (port == PortType::Serial)
    {
        if (s.baud > 0) conf.push_back({"serial_speed", std::to_string(s.baud)});
        if (s.dataBits > 0) conf.push_back({"data_bits", std::to_string(s.dataBits)});
        if (s.stopBits > 0) conf.push_back({"stop_bits", std::to_string(s.stopBits)});
        if (s.parity != Parity::Default) conf.push_back({"serial_parity", parityWord(s.parity)});
        if (s.handshake != Handshake::Default) conf.push_back({"serial_handshake", handshakeWord(s.handshake)});
        if (s.dtr != LineState::Default) conf.push_back({"dtr_state", lineWord(s.dtr)});
        if (s.rts != LineState::Default) conf.push_back({"rts_state", lineWord(s.rts)});
    }
    // CAT keying with the data audio needs RIGMICDATA: with plain RIG,
    // Hamlib turns a data PTT into a plain one. A line on a port needs
    // naming.
    if (s.keyRig && s.ptt == PttMethod::CatData) conf.push_back({"ptt_type", "RIGMICDATA"});
    if (s.keyRig && s.ptt == PttMethod::CatMic) conf.push_back({"ptt_type", "RIG"});
    if (s.keyRig && (s.ptt == PttMethod::Dtr || s.ptt == PttMethod::Rts))
    {
        conf.push_back({"ptt_type", s.ptt == PttMethod::Dtr ? "DTR" : "RTS"});
        std::string line = s.pttPort.empty() ? s.port : s.pttPort;
        if (line.empty())
        {
            problem = std::string("No port for keying by ") + (s.ptt == PttMethod::Dtr ? "DTR" : "RTS") + ".";
            return {};
        }
        conf.push_back({"ptt_pathname", line});
    }
    conf.insert(conf.end(), backend.begin(), backend.end());
    problem.clear();
    return conf;
}

bool sameRig(const RigSettings& a, const RigSettings& b)
{
    // Keying names its PTT type when the rig opens.
    return a.source == b.source && a.rigctldHost == b.rigctldHost && a.rigctldPort == b.rigctldPort
           && a.model == b.model && a.port == b.port && a.baud == b.baud && a.dataBits == b.dataBits
           && a.stopBits == b.stopBits && a.parity == b.parity && a.handshake == b.handshake && a.dtr == b.dtr
           && a.rts == b.rts && a.backendConf == b.backendConf && a.keyRig == b.keyRig
           && (!a.keyRig || (a.ptt == b.ptt && a.pttPort == b.pttPort));
}

} // namespace radio
} // namespace ragtime
