// A small JSON reader for the Submode Map's thresholds file: enough of
// RFC 8259 to read it, with no dependencies. Internal to the gearbox.
#ifndef RAGTIME_GEARBOX_JSON_H
#define RAGTIME_GEARBOX_JSON_H

#include <string>
#include <vector>

namespace ragtime::gearbox::json
{

struct Value
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    } type = Type::Null;

    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;       // an array's items, or an object's values
    std::vector<std::string> keys;  // an object's keys, in order

    // An object's member, or nullptr.
    const Value* member(const std::string& key) const;
};

// Parses a whole document. False, with a reason and byte offset in *error,
// on bad input.
bool parse(const std::string& text, Value& out, std::string* error);

} // namespace ragtime::gearbox::json

#endif
