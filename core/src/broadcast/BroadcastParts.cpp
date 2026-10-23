#include "ragtime/broadcast/BroadcastParts.h"

#include <cstddef>
#include <sstream>
#include <utility>

namespace ragtime
{
namespace broadcast
{

namespace
{

const char* const MAGIC = "ragtime broadcast part 1";

std::string hex(const uint8_t* bytes, size_t length)
{
    static const char* const digits = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; i++)
    {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0x0f];
    }
    return out;
}

std::string hex(const std::vector<uint8_t>& bytes)
{
    return hex(bytes.data(), bytes.size());
}

std::string hex(const protocol::Digest& digest)
{
    return hex(digest.data(), digest.size());
}

int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool unhex(const std::string& text, std::vector<uint8_t>& out)
{
    if (text.size() % 2 != 0) return false;
    out.clear();
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2)
    {
        int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)(hi << 4 | lo));
    }
    return true;
}

bool unhex(const std::string& text, protocol::Digest& out)
{
    std::vector<uint8_t> bytes;
    if (!unhex(text, bytes) || bytes.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); i++) out[i] = bytes[i];
    return true;
}

} // namespace

std::string encodePart(const BroadcastPart& part)
{
    std::ostringstream out;
    out << MAGIC << "\n";
    out << "announce " << hex(protocol::encodeFrame({part.announce})) << "\n";
    out << "state " << part.state << " " << (part.stopped ? 1 : 0) << " " << (part.autoReceived ? 1 : 0) << " "
        << (part.chosenPath ? 1 : 0) << "\n";
    out << "times " << part.startedMs << " " << part.lastMs << " " << part.expiresAtMs << "\n";
    // The path last: the rest of its line, whatever it holds but a newline.
    std::string path = part.path;
    for (char& c : path)
    {
        if (c == '\n' || c == '\r') c = ' ';
    }
    out << "path " << path << "\n";
    for (const KeptSeal& s : part.seals)
    {
        out << "seal " << s.seal.number << " " << s.heardMs << " " << hex(s.digest) << " " << hex(s.seal.previous)
            << " " << s.seal.firstItem;
        for (const protocol::Digest& item : s.seal.items) out << " " << hex(item);
        out << "\n";
    }
    for (const auto& p : part.pieces) out << "piece " << p.first << " " << hex(p.second) << "\n";
    // A file cut short, by a full disk say, lacks this and is passed over.
    out << "end\n";
    return out.str();
}

bool decodePart(const std::string& text, BroadcastPart& out)
{
    out = BroadcastPart();
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != MAGIC) return false;
    bool announced = false, ended = false;
    while (!ended && std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string key;
        fields >> key;
        if (key == "announce")
        {
            std::string h;
            fields >> h;
            std::vector<uint8_t> frame;
            std::vector<protocol::Record> records;
            if (!unhex(h, frame) || !protocol::decodeFrame(frame, records) || records.size() != 1
                || records[0].as<protocol::FileAnnounce>() == nullptr)
                return false;
            out.announce = records[0];
            announced = true;
        }
        else if (key == "state")
        {
            int stopped = 0, autoReceived = 0, chosen = 0;
            if (!(fields >> out.state >> stopped >> autoReceived >> chosen)) return false;
            out.stopped = stopped != 0;
            out.autoReceived = autoReceived != 0;
            out.chosenPath = chosen != 0;
        }
        else if (key == "times")
        {
            if (!(fields >> out.startedMs >> out.lastMs >> out.expiresAtMs)) return false;
        }
        else if (key == "path")
        {
            out.path = line.size() > 5 ? line.substr(5) : std::string();
        }
        else if (key == "seal")
        {
            KeptSeal s;
            std::string digest, previous, item;
            if (!(fields >> s.seal.number >> s.heardMs >> digest >> previous >> s.seal.firstItem)) return false;
            if (!unhex(digest, s.digest) || !unhex(previous, s.seal.previous)) return false;
            while (fields >> item)
            {
                protocol::Digest d{};
                if (!unhex(item, d)) return false;
                s.seal.items.push_back(d);
            }
            out.seals.push_back(std::move(s));
        }
        else if (key == "piece")
        {
            unsigned index = 0;
            std::string h;
            std::vector<uint8_t> bytes;
            if (!(fields >> index >> h) || index > 0xffff || !unhex(h, bytes) || bytes.empty()) return false;
            out.pieces[(uint16_t)index] = std::move(bytes);
        }
        else if (key == "end")
        {
            ended = true;
        }
    }
    return announced && ended;
}

} // namespace broadcast
} // namespace ragtime
