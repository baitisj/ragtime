#include "ragtime/gearbox/Gearbox.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>

namespace ragtime::gearbox
{

int widthCapHz(Width width)
{
    switch (width)
    {
    case Width::Hz200: return 200;
    case Width::Hz500: return 500;
    case Width::Hz1200: return 1200;
    case Width::Hz2400: return 2400;
    }
    return 2400;
}

const char* widthLabel(Width width)
{
    switch (width)
    {
    case Width::Hz200: return "200 Hz";
    case Width::Hz500: return "500 Hz";
    case Width::Hz1200: return "1.2 kHz";
    case Width::Hz2400: return "2.4 kHz";
    }
    return "";
}

const char* widthHostCommand(Width width)
{
    switch (width)
    {
    case Width::Hz500: return "BW500";
    case Width::Hz2400: return "BW2300";
    case Width::Hz200:
    case Width::Hz1200: return "";
    }
    return "";
}

bool hostSupportsWidth(Width width)
{
    return widthHostCommand(width)[0] != '\0';
}

std::vector<const Submode*> bestModes(const SubmodeTable& table, Path path, int capHz)
{
    // Most robust first; at equal S/N the faster first, then by name, so the
    // order never depends on the table's (as tools/gearbox/gearbox_ladder.py).
    std::vector<const Submode*> usable;
    for (const Submode& mode : table.modes())
    {
        if (mode.hasFacts() && mode.widthHz <= capHz && mode.thresholdOn(path))
            usable.push_back(&mode);
    }
    std::sort(usable.begin(), usable.end(), [path](const Submode* a, const Submode* b) {
        return std::make_tuple(*a->thresholdOn(path), -a->bitsPerSecond, a->name)
               < std::make_tuple(*b->thresholdOn(path), -b->bitsPerSecond, b->name);
    });

    std::vector<const Submode*> best;
    double fastest = 0.0;
    for (const Submode* mode : usable)
    {
        if (mode->bitsPerSecond > fastest)
        {
            fastest = mode->bitsPerSecond;
            best.push_back(mode);
        }
    }
    return best;
}

char snrLetter(double calmDb)
{
    const double step = std::floor((calmDb + 12.5) / 1.25 + 0.5);
    return static_cast<char>('A' + static_cast<int>(std::clamp(step, 0.0, 25.0)));
}

std::string speedShorthand(double bitsPerSecond)
{
    if (!(bitsPerSecond > 0) || !std::isfinite(bitsPerSecond))
        return {};
    // nearbyint rounds halves to even, as formatSpeed does, so the digits
    // agree with the speed shown beside them.
    const double whole = std::nearbyint(bitsPerSecond);
    if (whole < 100)
        return std::to_string(static_cast<long>(whole));
    const double hundreds = std::nearbyint(bitsPerSecond / 100.0);
    if (hundreds < 10)
        return std::to_string(static_cast<long>(hundreds)) + "C";
    return std::to_string(static_cast<long>(std::max(1.0, std::nearbyint(bitsPerSecond / 1000.0))))
           + "k";
}

std::string baseShortName(const Submode& mode)
{
    const std::optional<double> calm = mode.thresholdOn(Path::Calm);
    const std::string speed = speedShorthand(mode.bitsPerSecond);
    if (!calm || speed.empty())
        return {};
    return snrLetter(*calm) + speed;
}

std::map<std::string, std::string> shortNames(const SubmodeTable& table)
{
    // Easiest first, so the harder mode of a pair is the one that moves.
    std::vector<const Submode*> named;
    for (const Submode& mode : table.modes())
    {
        if (!baseShortName(mode).empty())
            named.push_back(&mode);
    }
    std::sort(named.begin(), named.end(), [](const Submode* a, const Submode* b) {
        return std::make_tuple(*a->thresholdOn(Path::Calm), -a->bitsPerSecond, a->name)
               < std::make_tuple(*b->thresholdOn(Path::Calm), -b->bitsPerSecond, b->name);
    });

    std::map<std::string, std::string> names;
    std::set<std::string> taken;
    for (const Submode* mode : named)
    {
        const std::string speed = speedShorthand(mode->bitsPerSecond);
        for (char letter = snrLetter(*mode->thresholdOn(Path::Calm)); letter <= 'Z'; letter++)
        {
            const std::string name = letter + speed;
            if (taken.insert(name).second)
            {
                names[mode->name] = name;
                break;
            }
        }
    }
    return names;
}

std::vector<Gear> gearLadder(const SubmodeTable& table, Path path, int capHz)
{
    const std::vector<const Submode*> best = bestModes(table, path, capHz);
    if (best.empty())
        return {};

    std::vector<const Submode*> picked = {best.front()};
    for (std::size_t i = 1; i < best.size(); i++)
    {
        if (best[i]->bitsPerSecond >= GEAR_RATIO * picked.back()->bitsPerSecond)
            picked.push_back(best[i]);
    }
    // The fastest best mode is always the top gear.
    if (picked.back() != best.back())
    {
        if (best.back()->bitsPerSecond >= TOP_GEAR_RATIO * picked.back()->bitsPerSecond)
            picked.push_back(best.back());
        else
            picked.back() = best.back();
    }

    const std::map<std::string, std::string> names = shortNames(table);
    std::vector<Gear> ladder;
    for (const Submode* mode : picked)
    {
        Gear gear;
        gear.number = static_cast<int>(ladder.size()) + 1;
        gear.mode = mode->name;
        if (auto name = names.find(mode->name); name != names.end())
            gear.name = name->second;
        gear.widthHz = mode->widthHz;
        gear.modulation = mode->modulation;
        gear.bitsPerSecond = mode->bitsPerSecond;
        gear.threshold = *mode->thresholdOn(path);
        ladder.push_back(std::move(gear));
    }
    return ladder;
}

int autoGearIndex(const std::vector<Gear>& ladder, double pathSnr, double margin)
{
    if (ladder.empty())
        return -1;
    int pick = 0;
    for (std::size_t i = 0; i < ladder.size(); i++)
    {
        // Gears get faster as they go up, so the last that fits is the pick.
        if (ladder[i].threshold <= pathSnr - margin)
            pick = static_cast<int>(i);
    }
    return pick;
}

double dbShort(double threshold, double pathSnr, double margin)
{
    return threshold - (pathSnr - margin);
}

double secondsPerKilobyte(double bitsPerSecond)
{
    if (!(bitsPerSecond > 0))
        return 0.0;
    return 1024.0 * 8.0 / bitsPerSecond;
}

std::string formatDuration(double seconds)
{
    char text[32];
    if (seconds < 9.95)
    {
        std::snprintf(text, sizeof text, "%.1f s", seconds);
        return text;
    }
    const long whole = std::lround(seconds);
    if (whole < 60)
    {
        std::snprintf(text, sizeof text, "%ld s", whole);
        return text;
    }
    std::snprintf(text, sizeof text, "%ld min %ld s", whole / 60, whole % 60);
    return text;
}

std::string formatSpeed(double bitsPerSecond)
{
    // %.0f rounds halves to even, as the Python ladder script does, so
    // 72.5 bit/s reads 72 in both.
    char text[32];
    std::snprintf(text, sizeof text, "%.0f bit/s", bitsPerSecond);
    return text;
}

std::string formatDb(double dB)
{
    // Round first, so -0.04 reads +0.0 dB rather than a minus zero.
    const double rounded = std::round(dB * 10.0) / 10.0;
    char text[32];
    std::snprintf(text, sizeof text, "%.1f dB", std::fabs(rounded));
    return std::string(rounded < 0 ? "\xE2\x88\x92" : "+") + text;
}

std::string gearLabel(const Gear& gear)
{
    return "Gear " + (gear.name.empty() ? gear.mode : gear.name);
}

void PathSnr::record(double dB)
{
    if (!std::isfinite(dB))
        return;
    bursts_.push_back(dB);
    while (bursts_.size() > BURSTS)
        bursts_.pop_front();
}

std::optional<double> PathSnr::average() const
{
    if (bursts_.empty())
        return std::nullopt;
    return std::accumulate(bursts_.begin(), bursts_.end(), 0.0)
           / static_cast<double>(bursts_.size());
}

void Gearshift::setTable(SubmodeTable table)
{
    table_ = std::move(table);
    rebuild();
}

void Gearshift::setHostModes(std::vector<HostMode> modes)
{
    hostModes_ = std::move(modes);
    rebuild();
}

void Gearshift::setWidth(Width width)
{
    width_ = width;
    rebuild();
}

void Gearshift::setPath(Path path)
{
    path_ = path;
    rebuild();
}

bool Gearshift::selectGear(int number)
{
    if (number < 1 || number > static_cast<int>(ladder_.size()))
        return false;
    chosen_ = number - 1;
    auto_ = false;
    return true;
}

std::string Gearshift::shortName(const std::string& mode) const
{
    auto name = names_.find(mode);
    return name != names_.end() ? name->second : mode;
}

std::optional<Gear> Gearshift::chosen() const
{
    if (ladder_.empty())
        return std::nullopt;
    return ladder_[static_cast<std::size_t>(chosen_)];
}

std::optional<Gear> Gearshift::suggestion() const
{
    const std::optional<double> snr = snr_.average();
    if (!snr || ladder_.empty())
        return std::nullopt;
    return ladder_[static_cast<std::size_t>(autoGearIndex(ladder_, *snr))];
}

std::optional<Gear> Gearshift::current() const
{
    if (auto_)
    {
        if (std::optional<Gear> pick = suggestion())
            return pick;
    }
    return chosen();
}

void Gearshift::rebuild()
{
    // A new ladder keeps the chosen mode if it is still a gear, else the
    // fastest gear no faster than it, so changing the width or path never
    // shifts up on its own.
    const std::optional<Gear> before = chosen();
    SubmodeTable withHost = table_;
    std::vector<std::string> offered;
    for (const HostMode& mode : hostModes_)
    {
        if (isArqControlMode(mode.name))
            continue;
        offered.push_back(mode.name);
        if (mode.widthHz > 0 && mode.bitsPerSecond > 0)
            withHost.setFacts(mode.name, mode.widthHz, mode.bitsPerSecond);
        for (Path path : ALL_PATHS)
        {
            if (std::optional<double> dB = mode.threshold[static_cast<int>(path)])
                withHost.setThreshold(mode.name, path, dB);
        }
    }
    names_ = shortNames(withHost);
    usable_ = withHost.offeredBy(offered);
    ladder_ = gearLadder(usable_, path_, widthCapHz(width_));
    for (Gear& gear : ladder_)
    {
        auto name = names_.find(gear.mode);
        gear.name = name != names_.end() ? name->second : std::string();
    }
    chosen_ = 0;
    if (!before)
        return;
    for (std::size_t i = 0; i < ladder_.size(); i++)
    {
        if (ladder_[i].mode == before->mode)
        {
            chosen_ = static_cast<int>(i);
            return;
        }
        if (ladder_[i].bitsPerSecond <= before->bitsPerSecond)
            chosen_ = static_cast<int>(i);
    }
}

} // namespace ragtime::gearbox
