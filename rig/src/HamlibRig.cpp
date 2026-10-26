#include "ragtime/rig/HamlibRig.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include <hamlib/rig.h>

#include "ragtime/link/Reconnect.h"

namespace ragtime
{
namespace rig
{

namespace
{

// rig_init and the backend list share Hamlib's tables: one at a time.
std::mutex& hamlibMutex()
{
    static std::mutex m;
    return m;
}

// Hamlib quiet, and every backend loaded, once.
void hamlibReady()
{
    static std::once_flag once;
    std::call_once(once, []() {
        rig_set_debug(RIG_DEBUG_NONE);
        std::lock_guard<std::mutex> lock(hamlibMutex());
        rig_load_all_backends();
    });
}

uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// An error in plain words. Hamlib's own rigerror() adds its debug trail,
// and rigerror2() shares one buffer between threads, so the words are
// ours.
std::string errorText(int code)
{
    switch (code < 0 ? -code : code)
    {
    case RIG_EINVAL: return "a setting Hamlib won't take";
    case RIG_ECONF: return "the port settings are wrong (Hamlib: invalid configuration)";
    case RIG_ENOMEM: return "out of memory";
    case RIG_ENIMPL: return "Hamlib can't do that with this rig yet";
    case RIG_ETIMEOUT: return "the rig didn't answer (is it on, and at that speed?)";
    case RIG_EIO: return "the port couldn't be opened or used";
    case RIG_EINTERNAL: return "Hamlib's internal error";
    case RIG_EPROTO: return "the rig's answer made no sense (protocol error)";
    case RIG_ERJCTED: return "the rig refused the command";
    case RIG_ETRUNC: return "the rig took the command, cut short";
    case RIG_ENAVAIL: return "this rig can't do that";
    case RIG_ENTARGET: return "the rig can't address that VFO";
    case RIG_BUSERROR: return "an error on the CI-V bus";
    case RIG_BUSBUSY: return "a collision on the CI-V bus";
    case RIG_EVFO: return "an invalid VFO";
    case RIG_EDOM: return "a value out of range";
    case RIG_EPOWER: return "the rig is switched off";
    default: break;
    }
    return "Hamlib error " + std::to_string(code);
}

const char* statusWord(enum rig_status_e status)
{
    switch (status)
    {
    case RIG_STATUS_ALPHA: return "Alpha";
    case RIG_STATUS_UNTESTED: return "Untested";
    case RIG_STATUS_BETA: return "Beta";
    case RIG_STATUS_STABLE: return "Stable";
    case RIG_STATUS_BUGGY: return "Buggy";
    }
    return "";
}

radio::PortType portType(const struct rig_caps* caps)
{
    switch (caps->port_type)
    {
    case RIG_PORT_NONE: return radio::PortType::None;
    case RIG_PORT_SERIAL: return radio::PortType::Serial;
    case RIG_PORT_NETWORK:
    case RIG_PORT_UDP_NETWORK: return radio::PortType::Network;
    default: return radio::PortType::Other;
    }
}

// As a person reads them: Kachina before KTH-SDR kit, IC-703 before
// IC-7000. Letters without case, runs of digits by their value.
int readingOrder(const std::string& a, const std::string& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[j];
        if (std::isdigit(x) && std::isdigit(y))
        {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit((unsigned char)a[ei])) ei++;
            while (ej < b.size() && std::isdigit((unsigned char)b[ej])) ej++;
            // Leading zeros aside, the longer run is the bigger number.
            size_t si = a.find_first_not_of('0', i), sj = b.find_first_not_of('0', j);
            si = std::min(si, ei);
            sj = std::min(sj, ej);
            if (ei - si != ej - sj) return ei - si < ej - sj ? -1 : 1;
            int c = a.compare(si, ei - si, b, sj, ej - sj);
            if (c != 0) return c;
            i = ei;
            j = ej;
            continue;
        }
        int cx = std::tolower(x), cy = std::tolower(y);
        if (cx != cy) return cx < cy ? -1 : 1;
        i++;
        j++;
    }
    return (int)(a.size() - i) - (int)(b.size() - j);
}

Model modelOf(const struct rig_caps* caps)
{
    Model m;
    m.id = (int)caps->rig_model;
    m.maker = caps->mfg_name != nullptr ? caps->mfg_name : "";
    m.name = caps->model_name != nullptr ? caps->model_name : "";
    m.status = statusWord(caps->status);
    m.port = portType(caps);
    return m;
}

int addModel(const struct rig_caps* caps, rig_ptr_t data)
{
    if (caps->rig_model != RIG_MODEL_NETRIGCTL) static_cast<std::vector<Model>*>(data)->push_back(modelOf(caps));
    return 1; // go on
}

// Opens the rig, or says why it couldn't.
RIG* openRig(const radio::RigSettings& settings, std::string& why)
{
    hamlibReady();
    RIG* rig = nullptr;
    const struct rig_caps* caps = nullptr;
    {
        std::lock_guard<std::mutex> lock(hamlibMutex());
        caps = rig_get_caps((rig_model_t)settings.model);
        if (caps != nullptr && settings.model != RIG_MODEL_NETRIGCTL) rig = rig_init((rig_model_t)settings.model);
    }
    if (rig == nullptr)
    {
        why = settings.model == 0 ? std::string("No rig picked")
                                  : "Hamlib has no rig model " + std::to_string(settings.model);
        return nullptr;
    }
    std::string name = std::string(caps->model_name != nullptr ? caps->model_name : "the rig");
    std::string problem;
    auto conf = radio::hamlibConf(settings, portType(caps), problem);
    if (!problem.empty())
    {
        why = problem;
        rig_cleanup(rig);
        return nullptr;
    }
    for (const auto& [word, value] : conf)
    {
        auto token = rig_token_lookup(rig, word.c_str());
        if (token == RIG_CONF_END)
        {
            why = "Hamlib's " + name + " has no setting \"" + word + "\"";
            rig_cleanup(rig);
            return nullptr;
        }
        int r = rig_set_conf(rig, token, value.c_str());
        if (r != RIG_OK)
        {
            why = "Hamlib refused " + word + "=" + value + ": " + errorText(r);
            rig_cleanup(rig);
            return nullptr;
        }
    }
    int r = rig_open(rig);
    if (r != RIG_OK)
    {
        why = "Couldn't open the " + name + (settings.port.empty() ? std::string() : " on " + settings.port) + ": "
              + errorText(r);
        rig_cleanup(rig);
        return nullptr;
    }
    return rig;
}

// A transmit meter the rig reports, and where its reading goes.
struct Meter
{
    setting_t level = 0;
    std::optional<double> radio::MeterReading::*field = nullptr;
    uint64_t HamlibRig::Status::*count = nullptr;
    int failures = 0;
};

// The meters this rig says it reports: ALC, SWR, and RF power in watts
// where the backend knows them, or else as a share of full power.
std::vector<Meter> metersOf(RIG* rig)
{
    std::vector<Meter> meters;
    using Status = HamlibRig::Status;
    auto add = [&](setting_t level, std::optional<double> radio::MeterReading::*field, uint64_t Status::*count) {
        if (rig_has_get_level(rig, level) != 0) meters.push_back(Meter{level, field, count, 0});
    };
    add(RIG_LEVEL_ALC, &radio::MeterReading::alc, &Status::alcReadings);
    add(RIG_LEVEL_SWR, &radio::MeterReading::swr, &Status::swrReadings);
    if (rig_has_get_level(rig, RIG_LEVEL_RFPOWER_METER_WATTS) != 0)
        add(RIG_LEVEL_RFPOWER_METER_WATTS, &radio::MeterReading::watts, &Status::powerReadings);
    else
        add(RIG_LEVEL_RFPOWER_METER, &radio::MeterReading::power, &Status::powerReadings);
    return meters;
}

// Which meters the status says it reads.
void showMeters(HamlibRig::Status& s, const std::vector<Meter>& meters)
{
    s.readsAlc = s.readsSwr = s.readsPower = false;
    for (const Meter& m : meters)
    {
        s.readsAlc = s.readsAlc || m.level == RIG_LEVEL_ALC;
        s.readsSwr = s.readsSwr || m.level == RIG_LEVEL_SWR;
        s.readsPower = s.readsPower || m.level == RIG_LEVEL_RFPOWER_METER_WATTS || m.level == RIG_LEVEL_RFPOWER_METER;
    }
}

void closeRig(RIG*& rig)
{
    if (rig == nullptr) return;
    rig_close(rig);
    rig_cleanup(rig);
    rig = nullptr;
}

} // namespace

bool available()
{
    return true;
}

std::string hamlibVersion()
{
    // "Hamlib 4.5.5 Sat Apr 20 ...": the first two words.
    std::string text = hamlib_version;
    size_t space = text.find(' ');
    if (space != std::string::npos) space = text.find(' ', space + 1);
    return space == std::string::npos ? text : text.substr(0, space);
}

std::vector<Model> models()
{
    hamlibReady();
    std::vector<Model> all;
    {
        std::lock_guard<std::mutex> lock(hamlibMutex());
        rig_list_foreach(addModel, &all);
    }
    std::sort(all.begin(), all.end(), [](const Model& a, const Model& b) {
        int maker = readingOrder(a.maker, b.maker);
        if (maker != 0) return maker < 0;
        if (a.maker != b.maker) return a.maker < b.maker;
        int name = readingOrder(a.name, b.name);
        return name != 0 ? name < 0 : a.id < b.id;
    });
    return all;
}

std::optional<Model> model(int id)
{
    hamlibReady();
    std::lock_guard<std::mutex> lock(hamlibMutex());
    const struct rig_caps* caps = rig_get_caps((rig_model_t)id);
    if (caps == nullptr || id == RIG_MODEL_NETRIGCTL) return std::nullopt;
    return modelOf(caps);
}

std::vector<BackendParam> backendParams(int id)
{
    hamlibReady();
    std::vector<BackendParam> params;
    std::lock_guard<std::mutex> lock(hamlibMutex());
    const struct rig_caps* caps = rig_get_caps((rig_model_t)id);
    if (caps == nullptr) return params;
    for (const struct confparams* p = caps->cfgparams; p != nullptr && p->token != RIG_CONF_END; p++)
    {
        if (p->name == nullptr) break;
        BackendParam b;
        b.name = p->name;
        b.label = p->label != nullptr ? p->label : "";
        b.fallback = p->dflt != nullptr ? p->dflt : "";
        params.push_back(b);
    }
    return params;
}

//-------------------------------------------------------------------------
// The rig's thread
//-------------------------------------------------------------------------

struct HamlibRig::Shared
{
    mutable std::mutex mutex;
    std::condition_variable wake;
    radio::RigSettings settings;
    bool stopping = false;
    bool wantPtt = false;
    bool transmitting = false;
    int pollSeconds = 1;
    Status status;
    uint64_t retryAtMs = 0;     // when the next try to open is, while one waits

    // Changes the status under the lock.
    template <typename F>
    void update(F change)
    {
        std::lock_guard<std::mutex> lock(mutex);
        change(status);
        status.version++;
    }
};

HamlibRig::HamlibRig() = default;

HamlibRig::~HamlibRig()
{
    stop();
}

void HamlibRig::start(const radio::RigSettings& settings)
{
    stop();
    shared_ = std::make_shared<Shared>();
    shared_->settings = settings;
    shared_->pollSeconds = std::max(radio::POLL_MIN_SECONDS, settings.pollSeconds);
    shared_->status.running = true;
    thread_ = std::thread([this]() { run(); });
}

void HamlibRig::stop()
{
    if (!shared_) return;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->stopping = true;
        shared_->wantPtt = false;
    }
    shared_->wake.notify_all();
    if (thread_.joinable()) thread_.join();
    shared_.reset();
}

void HamlibRig::setPollSeconds(int seconds)
{
    if (!shared_) return;
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->pollSeconds = std::max(radio::POLL_MIN_SECONDS, seconds);
}

void HamlibRig::setPtt(bool on)
{
    if (!shared_) return;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->wantPtt == on) return;
        shared_->wantPtt = on;
    }
    shared_->wake.notify_all();
}

void HamlibRig::setTransmitting(bool on)
{
    if (!shared_) return;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->transmitting == on) return;
        shared_->transmitting = on;
    }
    shared_->wake.notify_all();
}

HamlibRig::Status HamlibRig::status() const
{
    if (!shared_) return Status();
    std::lock_guard<std::mutex> lock(shared_->mutex);
    Status s = shared_->status;
    if (!s.open && shared_->retryAtMs != 0)
    {
        uint64_t now = nowMs();
        int seconds = shared_->retryAtMs > now ? (int)((shared_->retryAtMs - now + 999) / 1000) : 0;
        s.problem += seconds > 0 ? "; trying again in " + std::to_string(seconds) + " s" : "; trying again";
    }
    return s;
}

void HamlibRig::run()
{
    // The thread keeps its own hold on what it shares, past a stop().
    std::shared_ptr<Shared> shared = shared_;
    RIG* rig = nullptr;
    link::Reconnect again;
    bool keyed = false;
    int readFailures = 0;
    uint64_t nextPoll = 0;
    std::vector<Meter> meters;
    size_t nextMeter = 0;
    uint64_t nextMeterMs = 0;
    bool wasTx = false;
    const radio::RigSettings settings = shared->settings;

    // Closes the rig after a failure; tries again later.
    auto failed = [&](const std::string& why) {
        closeRig(rig);
        uint64_t now = nowMs();
        again.failed(now);
        bool wasKeyed = keyed;
        keyed = false;
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->wantPtt = false;
        shared->retryAtMs = now + link::Reconnect::waitMs(again.failures());
        shared->status.open = false;
        shared->status.keyed = false;
        shared->status.hz.reset();
        shared->status.meters = radio::MeterReading();
        shared->status.problem = why;
        if (wasKeyed) shared->status.pttFailures++;
        shared->status.version++;
    };

    for (;;)
    {
        bool want, tx;
        int poll;
        {
            std::unique_lock<std::mutex> lock(shared->mutex);
            if (shared->stopping) break;
            want = shared->wantPtt;
            tx = shared->transmitting || keyed;
            poll = shared->pollSeconds;
        }
        uint64_t now = nowMs();
        if (tx != wasTx)
        {
            // A new burst reads its meters from the first; between bursts
            // there are none.
            wasTx = tx;
            nextMeter = 0;
            nextMeterMs = now;
            if (!tx) shared->update([](Status& s) { s.meters = radio::MeterReading(); });
        }

        if (rig == nullptr)
        {
            if (!again.waiting() || again.due(now))
            {
                again.trying();
                std::string why;
                rig = openRig(settings, why);
                if (rig == nullptr)
                {
                    failed(why);
                }
                else
                {
                    again.connected();
                    readFailures = 0;
                    nextPoll = 0;
                    meters = metersOf(rig);
                    nextMeter = 0;
                    std::string name = std::string(rig->caps->mfg_name) + " " + rig->caps->model_name;
                    shared->update([&](Status& s) {
                        s.open = true;
                        s.rig = name;
                        s.problem.clear();
                        showMeters(s, meters);
                    });
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    shared->retryAtMs = 0;
                }
                continue;
            }
        }
        else if (want != keyed)
        {
            // PTT before anything else.
            ptt_t value = !want ? RIG_PTT_OFF
                                : settings.ptt == radio::PttMethod::CatData ? RIG_PTT_ON_DATA : RIG_PTT_ON;
            int r = rig_set_ptt(rig, RIG_VFO_CURR, value);
            if (r == RIG_OK)
            {
                keyed = want;
                shared->update([&](Status& s) {
                    s.keyed = keyed;
                    s.problem.clear();
                });
            }
            else
            {
                std::string why = std::string(want ? "Keying" : "Unkeying") + " failed: " + errorText(r);
                if (keyed || want) rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    shared->status.pttFailures++;
                }
                keyed = false;
                failed(why);
            }
            continue;
        }
        else if (tx && !meters.empty() && now >= nextMeterMs)
        {
            // One meter at a time, so a PTT change never waits on more
            // than one reading.
            size_t i = nextMeter++ % meters.size();
            Meter& meter = meters[i];
            nextMeterMs = now + METER_MS;
            value_t value;
            value.f = 0;
            int r = rig_get_level(rig, RIG_VFO_CURR, meter.level, &value);
            if (r == RIG_OK)
            {
                meter.failures = 0;
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->status.meters.*meter.field = (double)value.f;
                shared->status.*meter.count += 1;
                shared->status.meterReadings++;
            }
            else if (++meter.failures >= 2)
            {
                // A meter the backend lists but the rig won't read: left
                // alone until the rig is opened again.
                meters.erase(meters.begin() + (long)i);
                shared->update([&](Status& s) { showMeters(s, meters); });
            }
        }
        else if (!tx && now >= nextPoll)
        {
            freq_t freq = 0;
            int r = rig_get_freq(rig, RIG_VFO_CURR, &freq);
            nextPoll = now + (uint64_t)poll * 1000;
            if (r == RIG_OK && freq > 0)
            {
                readFailures = 0;
                uint64_t hz = (uint64_t)std::llround(freq);
                // News only when the dial moved, or a problem cleared.
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (shared->status.hz != hz || !shared->status.problem.empty())
                {
                    shared->status.hz = hz;
                    shared->status.problem.clear();
                    shared->status.version++;
                }
            }
            else if (++readFailures >= 3)
            {
                failed("The rig stopped answering: " + errorText(r == RIG_OK ? -RIG_EPROTO : r));
                continue;
            }
            else
            {
                shared->update([&](Status& s) { s.problem = "Reading the dial failed: " + errorText(r); });
            }
        }

        // Sleep until the next reading or try, or until woken.
        uint64_t until = rig == nullptr ? now + 250 : !tx ? nextPoll : meters.empty() ? now + 1000 : nextMeterMs;
        std::unique_lock<std::mutex> lock(shared->mutex);
        shared->wake.wait_for(lock, std::chrono::milliseconds(until > now ? until - now : 1), [&]() {
            return shared->stopping || (rig != nullptr && shared->wantPtt != keyed)
                   || (shared->transmitting || keyed) != tx;
        });
    }

    if (rig != nullptr && keyed) rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);
    closeRig(rig);
    shared->update([](Status& s) {
        s.running = false;
        s.open = false;
        s.keyed = false;
        s.hz.reset();
        s.meters = radio::MeterReading();
    });
}

} // namespace rig
} // namespace ragtime
