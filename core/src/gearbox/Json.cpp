#include "Json.h"

#include <cstddef>
#include <cstdint>
#include <locale>
#include <sstream>
#include <utility>

namespace ragtime::gearbox::json
{

const Value* Value::member(const std::string& key) const
{
    if (type != Type::Object)
        return nullptr;
    for (std::size_t i = 0; i < keys.size(); i++)
    {
        if (keys[i] == key)
            return &items[i];
    }
    return nullptr;
}

namespace
{

// Deep enough for any thresholds file, shallow enough that a hostile one
// can't overflow the stack.
constexpr int MAX_DEPTH = 64;

class Parser
{
public:
    explicit Parser(const std::string& text)
        : text_(text)
    {
    }

    bool document(Value& out)
    {
        skipSpace();
        if (!value(out, 0))
            return false;
        skipSpace();
        if (pos_ != text_.size())
            return fail("text after the document");
        return true;
    }

    std::string error() const { return error_ + " at byte " + std::to_string(pos_); }

private:
    bool fail(const char* why)
    {
        if (error_.empty())
            error_ = why;
        return false;
    }

    void skipSpace()
    {
        while (pos_ < text_.size()
               && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n'
                   || text_[pos_] == '\r'))
            pos_++;
    }

    bool literal(const char* word)
    {
        const std::string w(word);
        if (text_.compare(pos_, w.size(), w) != 0)
            return fail("unknown word");
        pos_ += w.size();
        return true;
    }

    bool value(Value& out, int depth)
    {
        if (depth > MAX_DEPTH)
            return fail("nested too deeply");
        if (pos_ >= text_.size())
            return fail("unexpected end");
        const char c = text_[pos_];
        if (c == '{')
            return object(out, depth);
        if (c == '[')
            return array(out, depth);
        if (c == '"')
        {
            out.type = Value::Type::String;
            return string(out.string);
        }
        if (c == 't' || c == 'f')
        {
            out.type = Value::Type::Bool;
            out.boolean = c == 't';
            return literal(c == 't' ? "true" : "false");
        }
        if (c == 'n')
        {
            out.type = Value::Type::Null;
            return literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            out.type = Value::Type::Number;
            return number(out.number);
        }
        return fail("unexpected character");
    }

    bool object(Value& out, int depth)
    {
        out.type = Value::Type::Object;
        pos_++;  // {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}')
        {
            pos_++;
            return true;
        }
        for (;;)
        {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return fail("expected a key");
            std::string key;
            if (!string(key))
                return false;
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':')
                return fail("expected ':'");
            pos_++;
            skipSpace();
            Value item;
            if (!value(item, depth + 1))
                return false;
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(item));
            skipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',')
            {
                pos_++;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}')
            {
                pos_++;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value& out, int depth)
    {
        out.type = Value::Type::Array;
        pos_++;  // [
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']')
        {
            pos_++;
            return true;
        }
        for (;;)
        {
            skipSpace();
            Value item;
            if (!value(item, depth + 1))
                return false;
            out.items.push_back(std::move(item));
            skipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',')
            {
                pos_++;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']')
            {
                pos_++;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(std::uint32_t& out)
    {
        if (pos_ + 4 > text_.size())
            return fail("short \\u escape");
        out = 0;
        for (int i = 0; i < 4; i++)
        {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return fail("bad \\u escape");
        }
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out)
    {
        pos_++;  // "
        out.clear();
        while (pos_ < text_.size())
        {
            const char c = text_[pos_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in a string");
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (pos_ >= text_.size())
                break;
            const char e = text_[pos_++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                std::uint32_t cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00)
                {
                    std::uint32_t low = 0;
                    if (text_.compare(pos_, 2, "\\u") != 0)
                        return fail("lone surrogate");
                    pos_ += 2;
                    if (!hex4(low) || low < 0xDC00 || low >= 0xE000)
                        return fail("lone surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp < 0xE000)
                {
                    return fail("lone surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool number(double& out)
    {
        const std::size_t start = pos_;
        if (text_[pos_] == '-')
            pos_++;
        auto digits = [this]() {
            const std::size_t from = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9')
                pos_++;
            return pos_ > from;
        };
        if (!digits())
            return fail("bad number");
        if (pos_ < text_.size() && text_[pos_] == '.')
        {
            pos_++;
            if (!digits())
                return fail("bad number");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E'))
        {
            pos_++;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                pos_++;
            if (!digits())
                return fail("bad number");
        }
        // The classic locale, so a decimal comma in the operator's locale
        // never changes how the file reads.
        std::istringstream in(text_.substr(start, pos_ - start));
        in.imbue(std::locale::classic());
        in >> out;
        if (in.fail())
            return fail("bad number");
        return true;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    std::string error_;
};

} // namespace

bool parse(const std::string& text, Value& out, std::string* error)
{
    Parser parser(text);
    Value result;
    if (!parser.document(result))
    {
        if (error)
            *error = parser.error();
        return false;
    }
    out = std::move(result);
    return true;
}

} // namespace ragtime::gearbox::json
