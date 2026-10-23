// The line format the lasting chat logs share (session::SessionLog,
// broadcast::BroadcastLog): one line a thing, fields split by tabs, text
// with backslash, tab, CR and LF escaped as \\, \t, \r and \n.
#ifndef RAGTIME_LOG_LINES_H
#define RAGTIME_LOG_LINES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace ragtime
{
namespace loglines
{

inline std::string escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            default: out += c; break;
        }
    }
    return out;
}

inline std::string unescape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        if (c != '\\' || i + 1 == text.size())
        {
            out += c;
            continue;
        }
        char next = text[++i];
        switch (next)
        {
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'n': out += '\n'; break;
            default: out += next; break;
        }
    }
    return out;
}

inline std::vector<std::string> fields(const std::string& line)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return out;
}

inline bool number(const std::string& field, uint64_t& value)
{
    if (field.empty() || field.size() > 20) return false;
    if (!std::all_of(field.begin(), field.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    value = std::strtoull(field.c_str(), nullptr, 10);
    return true;
}

inline std::string join(std::initializer_list<std::string> parts)
{
    std::string line;
    bool first = true;
    for (const std::string& p : parts)
    {
        if (!first) line += '\t';
        line += p;
        first = false;
    }
    return line;
}

} // namespace loglines
} // namespace ragtime

#endif
