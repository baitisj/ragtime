#include "ragtime/broadcast/BroadcastLog.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

#include "ragtime/files/SessionFiles.h"

#include "../LogLines.h"

namespace ragtime
{
namespace broadcast
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

const char* const FIRST_LINE = "ragtime broadcast log 1";

char stateLetter(State state)
{
    switch (state)
    {
        case State::Heard: return 'H';
        case State::Queued: return 'Q';
        case State::Sending: return 'G';
        case State::Held: return 'W';
        case State::Sent: return 'S';
        case State::NotSent: return 'N';
        case State::Cancelled: return 'C';
        case State::Acknowledged: return 'A';
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
        case 'G': state = State::Sending; return true;
        case 'W': state = State::Held; return true;
        case 'S': state = State::Sent; return true;
        case 'N': state = State::NotSent; return true;
        case 'C': state = State::Cancelled; return true;
        case 'A': state = State::Acknowledged; return true;
        default: return false;
    }
}

} // namespace

std::vector<Message> parseBroadcastLog(const std::string& text, size_t max)
{
    std::vector<Message> rows;
    std::map<uint64_t, size_t> ids;     // the open run's rows, by their id there
    bool any = false;                   // the open run has rows
    uint64_t lastMs = 0;

    auto close = [&](uint64_t endMs) {
        if (any)
        {
            for (const auto& id : ids)
            {
                Message& m = rows[id.second];
                // Still with us or the host when Ragtime closed: it may
                // never have gone.
                if (m.state == State::Queued || m.state == State::Sending || m.state == State::Held)
                    m.state = State::NotSent;
            }
            Message rule;
            rule.kind = Kind::Rule;
            rule.past = true;
            rule.atMs = endMs;
            rows.push_back(rule);
        }
        any = false;
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
        }
        else if (kind == "E" && f.size() >= 2 && number(f[1], ms))
        {
            close(ms);
            lastMs = std::max(lastMs, ms);
        }
        else if (kind == "M" && f.size() >= 12)
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
            m.to = unescape(f[8]);
            m.ackWanted = f[9] == "1";
            m.mode = unescape(f[10]);
            m.text = unescape(f[11]);
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
            if (it == ids.end()) continue;
            rows[it->second].state = state;
            if (f.size() >= 4 && !f[3].empty()) rows[it->second].mode = unescape(f[3]);
        }
        else if (kind == "F" && f.size() >= 6)
        {
            uint64_t id = 0, atMs = 0;
            if (!number(f[1], id) || !number(f[2], atMs)) continue;
            Message m;
            m.kind = Kind::File;
            m.past = true;
            m.ours = f[3] == "1";
            m.atMs = atMs;
            m.from = unescape(f[4]);
            m.text = unescape(f[5]);
            ids[id] = rows.size();
            rows.push_back(std::move(m));
            any = true;
            lastMs = std::max(lastMs, atMs);
        }
        else if (kind == "R" && f.size() >= 3)
        {
            uint64_t id = 0;
            if (!number(f[1], id)) continue;
            auto it = ids.find(id);
            if (it != ids.end() && rows[it->second].kind == Kind::File) rows[it->second].outcome = unescape(f[2]);
        }
    }
    // The last run never got its E: Ragtime stopped under it.
    close(lastMs);

    if (rows.size() > max) rows.erase(rows.begin(), rows.end() - (std::ptrdiff_t)max);
    while (!rows.empty() && rows.front().kind == Kind::Rule) rows.erase(rows.begin());
    return rows;
}

BroadcastLog::BroadcastLog(std::string path) : path_(std::move(path))
{
}

std::vector<Message> BroadcastLog::load(size_t max) const
{
    if (path_.empty()) return {};
    std::ifstream in(files::pathFromUtf8(path_), std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return parseBroadcastLog(text.str(), max);
}

void BroadcastLog::append(const std::string& line)
{
    if (path_.empty()) return;
    std::filesystem::path file = files::pathFromUtf8(path_);
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    bool fresh = !std::filesystem::exists(file, ec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) return;
    if (fresh) out << FIRST_LINE << '\n';
    out << line << '\n';
}

void BroadcastLog::compact()
{
    if (path_.empty()) return;
    std::filesystem::path file = files::pathFromUtf8(path_);
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size <= MAX_FILE_BYTES) return;
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    in.close();
    // Keep from the first run that begins in the later half, or, when one
    // run is most of the file, from the first row there (a state line
    // whose row was cut away is skipped on reading).
    uint64_t bytes = 0;
    size_t from = lines.size(), row = lines.size();
    for (size_t i = 1; i < lines.size(); i++)
    {
        if (bytes >= size / 2)
        {
            if (lines[i].compare(0, 2, "B\t") == 0)
            {
                from = i;
                break;
            }
            bool isRow = lines[i].compare(0, 2, "M\t") == 0 || lines[i].compare(0, 2, "F\t") == 0;
            if (isRow && row == lines.size()) row = i;
        }
        bytes += lines[i].size() + 1;
    }
    if (from == lines.size()) from = row;
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

void BroadcastLog::began(uint64_t nowMs)
{
    compact();
    append(join({"B", std::to_string(nowMs)}));
}

void BroadcastLog::added(const Message& m)
{
    if (m.kind == Kind::File)
    {
        append(join({"F", std::to_string(m.id), std::to_string(m.atMs), m.ours ? "1" : "0", escape(m.from),
                     escape(m.text)}));
        return;
    }
    if (m.kind != Kind::Text) return;
    append(join({"M", std::to_string(m.id), std::to_string(m.atMs), std::to_string(m.time), std::to_string(m.seq),
                 m.ours ? "1" : "0", std::string(1, stateLetter(m.state)), escape(m.from), escape(m.to),
                 m.ackWanted ? "1" : "0", escape(m.mode), escape(m.text)}));
}

void BroadcastLog::settled(const Message& m)
{
    if (m.kind != Kind::Text) return;
    append(join({"S", std::to_string(m.id), std::string(1, stateLetter(m.state)), escape(m.mode)}));
}

void BroadcastLog::fileEnded(const Message& card, const std::string& outcome)
{
    append(join({"R", std::to_string(card.id), escape(outcome)}));
}

void BroadcastLog::ended(uint64_t nowMs)
{
    append(join({"E", std::to_string(nowMs)}));
}

} // namespace broadcast
} // namespace ragtime
