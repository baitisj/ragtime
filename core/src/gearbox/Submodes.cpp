#include "ragtime/gearbox/Submodes.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "Json.h"

namespace ragtime::gearbox
{

const char* pathLabel(Path path)
{
    switch (path)
    {
    case Path::Calm: return "Calm";
    case Path::Good: return "Good";
    case Path::Poor: return "Poor";
    case Path::Disturbed: return "Disturbed";
    }
    return "";
}

const char* pathKey(Path path)
{
    switch (path)
    {
    case Path::Calm: return "awgn";
    case Path::Good: return "mpg";
    case Path::Poor: return "mpp";
    case Path::Disturbed: return "mpd";
    }
    return "";
}

std::optional<Path> pathFromKey(const std::string& key)
{
    for (Path path : ALL_PATHS)
    {
        if (key == pathKey(path))
            return path;
    }
    return std::nullopt;
}

Modulation modulationOf(const std::string& mapModulation, const std::string& modeName)
{
    if (mapModulation.find("FSK") != std::string::npos)
        return Modulation::Fsk;
    if (mapModulation == "QPSK")
        return Modulation::Qpsk;
    if (mapModulation == "QAM16")
        return Modulation::Qam16;
    if (mapModulation == "QAM64")
        return Modulation::Qam64;
    if (mapModulation == "QAM256")
        return Modulation::Qam256;
    if (!mapModulation.empty() && mapModulation != "Specialty")
        return Modulation::Other;

    // By name: fsk16r25-r1/3, qpsk-r1/5, n4-16qam-r1/3, w48-64l-r2/3,
    // n10-256l-r3/4, polar-k96-f4.
    auto has = [&modeName](const char* part) { return modeName.find(part) != std::string::npos; };
    if (modeName.compare(0, 6, "polar-") == 0)
        return Modulation::Polar;
    if (has("fsk"))
        return Modulation::Fsk;
    if (has("256l") || has("256qam"))
        return Modulation::Qam256;
    if (has("64l") || has("64qam"))
        return Modulation::Qam64;
    if (has("16qam"))
        return Modulation::Qam16;
    if (has("qpsk"))
        return Modulation::Qpsk;
    return Modulation::Other;
}

const char* modulationLabel(Modulation modulation)
{
    switch (modulation)
    {
    case Modulation::Fsk: return "FSK";
    case Modulation::Qpsk: return "QPSK";
    case Modulation::Qam16: return "16-QAM";
    case Modulation::Qam64: return "64-QAM";
    case Modulation::Qam256: return "256-QAM";
    case Modulation::Polar: return "Polar";
    case Modulation::Other: return "Other";
    }
    return "";
}

std::uint32_t modulationColour(Modulation modulation)
{
    switch (modulation)
    {
    case Modulation::Fsk: return 0x0f766e;
    case Modulation::Qpsk: return 0x1f5fbf;
    case Modulation::Qam16: return 0x7c3aed;
    case Modulation::Qam64: return 0xb8326b;
    case Modulation::Qam256: return 0x9a3412;
    case Modulation::Polar: return 0xa16207;
    case Modulation::Other: return 0x5b6370;
    }
    return 0x5b6370;
}

bool isArqControlMode(const std::string& name)
{
    // Narrow ones carry a prefix too: n4-ack-8f.
    return name.find("ack-") != std::string::npos;
}

double payloadBitsPerSecond(int bytesPerCodeword, int maxCodewords, double secondsAtOne,
                            double secondsAtMax)
{
    if (bytesPerCodeword <= 0 || maxCodewords <= 1 || !(secondsAtMax > secondsAtOne))
        return 0.0;
    const double secondsPerCodeword = (secondsAtMax - secondsAtOne) / (maxCodewords - 1);
    return bytesPerCodeword * 8.0 / secondsPerCodeword;
}

namespace
{

bool nameBefore(const Submode& mode, const std::string& name)
{
    return mode.name < name;
}

// data2g-host's mode_thresholds.json marks a threshold never reached as 99.
constexpr double NEVER_DB = 99.0;

bool readThreshold(const json::Value& v, std::optional<double>& out)
{
    if (v.type == json::Value::Type::Null)
    {
        out.reset();
        return true;
    }
    if (v.type != json::Value::Type::Number || !std::isfinite(v.number))
        return false;
    if (v.number >= NEVER_DB)
        out.reset();
    else
        out = v.number;
    return true;
}

// The Submode Map's form: {name: {"hz", "mod", "rate", "bps", "th": {...}}}.
bool readMapForm(const json::Value& root, SubmodeTable& table, std::string& error)
{
    for (std::size_t i = 0; i < root.keys.size(); i++)
    {
        const std::string& name = root.keys[i];
        const json::Value& entry = root.items[i];
        if (name.empty())
        {
            error = "a mode with no name";
            return false;
        }
        if (isArqControlMode(name))
            continue;
        if (entry.type != json::Value::Type::Object)
        {
            error = name + ": not an object";
            return false;
        }

        const json::Value* hz = entry.member("hz");
        const json::Value* bps = entry.member("bps");
        if (!hz || hz->type != json::Value::Type::Number || !(hz->number > 0)
            || hz->number > 100000)
        {
            error = name + ": no width (hz)";
            return false;
        }
        if (!bps || bps->type != json::Value::Type::Number || !(bps->number > 0)
            || !std::isfinite(bps->number))
        {
            error = name + ": no speed (bps)";
            return false;
        }

        Submode mode;
        mode.name = name;
        mode.widthHz = static_cast<int>(std::lround(hz->number));
        mode.bitsPerSecond = bps->number;
        const json::Value* mod = entry.member("mod");
        mode.modulation = modulationOf(
            mod && mod->type == json::Value::Type::String ? mod->string : std::string(), name);
        const json::Value* rate = entry.member("rate");
        if (rate && rate->type == json::Value::Type::String)
            mode.rate = rate->string;

        if (const json::Value* th = entry.member("th"))
        {
            if (th->type != json::Value::Type::Object)
            {
                error = name + ": th is not an object";
                return false;
            }
            for (Path path : ALL_PATHS)
            {
                const json::Value* v = th->member(pathKey(path));
                if (v && !readThreshold(*v, mode.threshold[static_cast<int>(path)]))
                {
                    error = name + ": bad threshold for " + pathKey(path);
                    return false;
                }
            }
        }
        table.add(std::move(mode));
    }
    return true;
}

// data2g-host's form: {"channels": [...], "modes": {name: [dB, ...]}}.
bool readHostForm(const json::Value& root, SubmodeTable& table, std::string& error)
{
    const json::Value& channels = *root.member("channels");
    const json::Value& modes = *root.member("modes");
    if (channels.type != json::Value::Type::Array || modes.type != json::Value::Type::Object)
    {
        error = "channels is not a list, or modes not an object";
        return false;
    }
    std::vector<std::optional<Path>> columns;
    for (const json::Value& c : channels.items)
    {
        if (c.type != json::Value::Type::String)
        {
            error = "a channel that is not a name";
            return false;
        }
        columns.push_back(pathFromKey(c.string));  // unknown channels are skipped
    }

    for (std::size_t i = 0; i < modes.keys.size(); i++)
    {
        const std::string& name = modes.keys[i];
        const json::Value& row = modes.items[i];
        if (name.empty())
        {
            error = "a mode with no name";
            return false;
        }
        if (isArqControlMode(name))
            continue;
        if (row.type != json::Value::Type::Array || row.items.size() != columns.size())
        {
            error = name + ": not one threshold per channel";
            return false;
        }
        if (!table.find(name))
        {
            Submode mode;
            mode.name = name;
            mode.modulation = modulationOf(std::string(), name);
            table.add(std::move(mode));
        }
        for (std::size_t c = 0; c < columns.size(); c++)
        {
            std::optional<double> dB;
            if (!readThreshold(row.items[c], dB))
            {
                error = name + ": bad threshold";
                return false;
            }
            if (columns[c])
                table.setThreshold(name, *columns[c], dB);
        }
    }
    return true;
}

} // namespace

bool SubmodeTable::parseJson(const std::string& text, std::string* error)
{
    json::Value root;
    std::string why;
    if (!json::parse(text, root, &why))
    {
        if (error)
            *error = why;
        return false;
    }

    SubmodeTable table = *this;
    bool ok = false;
    if (root.type != json::Value::Type::Object)
        why = "the thresholds file is not a JSON object";
    else if (root.member("channels") && root.member("modes"))
        ok = readHostForm(root, table, why);
    else
        ok = readMapForm(root, table, why);
    if (!ok)
    {
        if (error)
            *error = why;
        return false;
    }
    *this = std::move(table);
    return true;
}

bool SubmodeTable::setFacts(const std::string& name, int widthHz, double bitsPerSecond)
{
    if (name.empty() || isArqControlMode(name))
        return false;
    if (!find(name))
    {
        Submode mode;
        mode.name = name;
        mode.modulation = modulationOf(std::string(), name);
        add(std::move(mode));
    }
    auto at = std::lower_bound(modes_.begin(), modes_.end(), name, nameBefore);
    at->widthHz = widthHz;
    at->bitsPerSecond = bitsPerSecond;
    return true;
}

bool SubmodeTable::add(Submode mode)
{
    if (mode.name.empty() || isArqControlMode(mode.name))
        return false;
    auto at = std::lower_bound(modes_.begin(), modes_.end(), mode.name, nameBefore);
    if (at != modes_.end() && at->name == mode.name)
        *at = std::move(mode);
    else
        modes_.insert(at, std::move(mode));
    return true;
}

const Submode* SubmodeTable::find(const std::string& name) const
{
    auto at = std::lower_bound(modes_.begin(), modes_.end(), name, nameBefore);
    if (at != modes_.end() && at->name == name)
        return &*at;
    return nullptr;
}

bool SubmodeTable::setThreshold(const std::string& name, Path path, std::optional<double> dB)
{
    auto at = std::lower_bound(modes_.begin(), modes_.end(), name, nameBefore);
    if (at == modes_.end() || at->name != name)
        return false;
    at->threshold[static_cast<int>(path)] = dB;
    return true;
}

SubmodeTable SubmodeTable::offeredBy(const std::vector<std::string>& names) const
{
    if (names.empty())
        return *this;
    SubmodeTable out;
    for (const Submode& mode : modes_)
    {
        if (std::find(names.begin(), names.end(), mode.name) != names.end())
            out.modes_.push_back(mode);
    }
    return out;
}

} // namespace ragtime::gearbox
