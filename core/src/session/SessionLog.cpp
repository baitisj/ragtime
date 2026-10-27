#include "ragtime/session/SessionLog.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

#include "ragtime/files/SessionFiles.h"

#include "../LogLines.h"

namespace ragtime
{
namespace session
{

namespace
{

using State = Message::State;
using Kind = Message::Kind;
using loglines::escape;
using loglines::fields;
using loglines::join;
using loglines::number;
using loglines::unescape;

const char* const FIRST_LINE = "ragtime session log 1";

char stateLetter(State state)
{
    switch (state)
    {
        case State::Heard: return 'H';
        case State::Queued: return 'Q';
        case State::Sent: return 'S';
        case State::Delivered: return 'D';
        case State::NotDelivered: return 'N';
    }
    return 'H';
}

bool letterState(const std::string& field, State& state)
{
    if (field.size() != 1) return false;
    switch (field[0])
    {
        case 'H': state = State::Heard; return true;
        case 'Q': state = State::Queued; return true;
        case 'S': state = State::Sent; return true;
        case 'D': state = State::Delivered; return true;
        case 'N': state = State::NotDelivered; return true;
        default: return false;
    }
}

} // namespace

std::string sessionLogName(const std::string& callsign)
{
    std::string name;
    for (char c : callsign)
    {
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        name += plain ? c : '_';
    }
    return name;
}

std::vector<Message> parseSessionLog(const std::string& text, size_t max)
{
    std::vector<Message> rows;
    std::map<uint64_t, size_t> ids;     // the open session's messages, by their id there
    bool open = false;
    bool any = false;                   // the open session has rows
    uint64_t began = 0;
    uint64_t lastMs = 0;

    auto close = [&](uint64_t endMs) {
        if (open && any)
        {
            for (const auto& id : ids)
            {
                Message& m = rows[id.second];
                if (m.state == State::Sent || m.state == State::Queued) m.state = State::NotDelivered;
            }
            Message rule;
            rule.kind = Kind::Rule;
            rule.past = true;
            rule.atMs = endMs;
            rule.beganMs = began;
            rows.push_back(rule);
        }
        open = any = false;
        ids.clear();
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f = fields(line);
        const std::string& kind = f[0];
        uint64_t ms = 0;
        if (kind == "B" && f.size() >= 2 && number(f[1], ms))
        {
            close(lastMs);
            open = true;
            began = ms;
        }
        else if (kind == "E" && f.size() >= 2 && number(f[1], ms))
        {
            close(ms);
            lastMs = std::max(lastMs, ms);
        }
        else if (kind == "M" && f.size() >= 9)
        {
            uint64_t id = 0, atMs = 0, time = 0, seq = 0;
            Message m;
            if (!number(f[1], id) || !number(f[2], atMs) || !number(f[3], time) || !number(f[4], seq)
                || !letterState(f[6], m.state))
                continue;
            m.past = true;
            m.ours = f[5] == "1";
            m.atMs = atMs;
            m.time = (uint32_t)time;
            m.seq = (uint32_t)seq;
            m.from = unescape(f[7]);
            m.text = unescape(f[8]);
            if (!open)
            {
                // A session whose B was cut away by compact().
                open = true;
                began = 0;
            }
            ids[id] = rows.size();
            rows.push_back(std::move(m));
            any = true;
            lastMs = std::max(lastMs, atMs);
        }
        else if (kind == "S" && f.size() >= 3)
        {
            uint64_t id = 0;
            State state = State::Heard;
            if (!number(f[1], id) || !letterState(f[2], state)) continue;
            auto it = ids.find(id);
            if (it != ids.end()) rows[it->second].state = state;
        }
        else if ((kind == "X" && f.size() >= 4) || (kind == "Y" && f.size() >= 3))
        {
            // A reaction to a message of the same session.
            uint64_t id = 0, code = 0;
            bool ours = kind == "Y";
            if (!number(f[1], id) || !number(f[ours ? 2 : 3], code) || code == 0 || code > 255) continue;
            auto it = ids.find(id);
            if (it == ids.end()) continue;
            if (ours) rows[it->second].myReaction = (uint8_t)code;
            else protocol::setReaction(rows[it->second].reactions, unescape(f[2]), (uint8_t)code);
        }
        else if (kind == "F" && f.size() >= 5)
        {
            uint64_t atMs = 0;
            if (!number(f[1], atMs)) continue;
            Message m;
            m.kind = Kind::File;
            m.past = true;
            m.ours = f[2] == "1";
            m.atMs = atMs;
            m.from = unescape(f[3]);
            m.text = unescape(f[4]);
            rows.push_back(std::move(m));
            if (open) any = true;
            lastMs = std::max(lastMs, atMs);
        }
    }
    // The last session never got its E: the app stopped under it.
    close(lastMs);

    if (rows.size() > max) rows.erase(rows.begin(), rows.end() - (std::ptrdiff_t)max);
    while (!rows.empty() && rows.front().kind == Kind::Rule) rows.erase(rows.begin());
    return rows;
}

SessionLog::SessionLog(std::string folder) : folder_(std::move(folder))
{
}

std::string SessionLog::pathFor(const std::string& peer) const
{
    if (folder_.empty() || peer.empty()) return std::string();
    std::filesystem::path path = files::pathFromUtf8(folder_) / (sessionLogName(peer) + ".log");
    return files::utf8FromPath(path);
}

std::vector<Message> SessionLog::load(const std::string& peer, size_t max) const
{
    std::string path = pathFor(peer);
    if (path.empty()) return {};
    std::ifstream in(files::pathFromUtf8(path), std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return parseSessionLog(text.str(), max);
}

void SessionLog::append(const std::string& peer, const std::string& line)
{
    std::string path = pathFor(peer);
    if (path.empty()) return;
    std::filesystem::path file = files::pathFromUtf8(path);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    bool fresh = !std::filesystem::exists(file, ec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) return;
    if (fresh) out << FIRST_LINE << '\n';
    out << line << '\n';
}

void SessionLog::compact(const std::string& peer)
{
    std::string path = pathFor(peer);
    if (path.empty()) return;
    std::filesystem::path file = files::pathFromUtf8(path);
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size <= MAX_FILE_BYTES) return;
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    in.close();
    // Keep from the first session that begins in the later half.
    uint64_t bytes = 0;
    size_t from = lines.size();
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (bytes >= size / 2 && lines[i].compare(0, 2, "B\t") == 0)
        {
            from = i;
            break;
        }
        bytes += lines[i].size() + 1;
    }
    std::filesystem::path temp = file;
    temp += ".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << FIRST_LINE << '\n';
        for (size_t i = from; i < lines.size(); i++) out << lines[i] << '\n';
        if (!out) return;
    }
    std::filesystem::rename(temp, file, ec);
}

void SessionLog::began(const std::string& peer, uint64_t nowMs)
{
    compact(peer);
    append(peer, join({"B", std::to_string(nowMs)}));
}

void SessionLog::added(const std::string& peer, const Message& m)
{
    append(peer, join({"M", std::to_string(m.id), std::to_string(m.atMs), std::to_string(m.time),
                       std::to_string(m.seq), m.ours ? "1" : "0", std::string(1, stateLetter(m.state)),
                       escape(m.from), escape(m.text)}));
}

void SessionLog::settled(const std::string& peer, const Message& m)
{
    append(peer, join({"S", std::to_string(m.id), std::string(1, stateLetter(m.state))}));
}

void SessionLog::reacted(const std::string& peer, const Message& m, const std::string& call, uint8_t code)
{
    append(peer, join({"X", std::to_string(m.id), escape(call), std::to_string(code)}));
}

void SessionLog::weReacted(const std::string& peer, const Message& m, uint8_t code)
{
    append(peer, join({"Y", std::to_string(m.id), std::to_string(code)}));
}

void SessionLog::file(const std::string& peer, const Message& note)
{
    append(peer, join({"F", std::to_string(note.atMs), note.ours ? "1" : "0", escape(note.from), escape(note.text)}));
}

void SessionLog::ended(const std::string& peer, uint64_t nowMs)
{
    append(peer, join({"E", std::to_string(nowMs)}));
}

} // namespace session
} // namespace ragtime
