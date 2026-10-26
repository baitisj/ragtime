#include "RigControl.h"

#include <cstdio>
#include <utility>

#include "HostConnection.h"
#include "ragtime/radio/Bands.h"
#include "RigConnection.h"

using namespace ragtime;

namespace
{

// The rig's thread is looked at this often, and the 180 s cap checked.
constexpr int TICK_MS = 100;

} // namespace

RigControl::RigControl(std::function<void()> changed, std::function<void()> meters,
                       std::function<void(const std::string&)> tripped)
    : changed_(std::move(changed))
    , metersChanged_(std::move(meters))
    , trippedChanged_(std::move(tripped))
    , rigctld_(std::make_unique<RigConnection>([this]() {
        syncRigctld();
        if (changed_) changed_();
    }))
    , timer_(this)
{
    Bind(wxEVT_TIMER, &RigControl::onTimer, this);
}

RigControl::~RigControl()
{
    // The window is going: no more news for it.
    changed_ = nullptr;
    metersChanged_ = nullptr;
    trippedChanged_ = nullptr;
    rigctld_->setNote(nullptr);
    stop();
}

void RigControl::setNote(std::function<void(const std::string&)> note)
{
    rigctld_->setNote(std::move(note));
}

void RigControl::apply(const radio::RigSettings& settings)
{
    bool reopen = !started_ || !radio::sameRig(settings, settings_);
    settings_ = settings;
    started_ = true;
    follower_.setEnabled(settings.source == radio::RigSource::Hamlib && settings.keyRig);
    bool rigctldTrip = settings.source == radio::RigSource::Rigctld && settings.swrTrip;
    if (rigctldTrip != rigctldTrip_.enabled()) rigctldTrip_.setEnabled(rigctldTrip);
    rigctld_->setSwrWatch(rigctldTrip);
    if (reopen)
    {
        rigctld_->stop();
        hamlib_.stop();
        seenFailures_ = 0;
        seenAlc_ = seenSwr_ = seenPower_ = seenTrips_ = seenStrength_ = 0;
        noise_.clear();
        if (settings.source == radio::RigSource::Rigctld)
            rigctld_->start(settings.rigctldHost, settings.rigctldPort);
        else if (settings.source == radio::RigSource::Hamlib)
            hamlib_.start(settings);
        hamlib_.setFilterWatch(filterWatch_);
    }
    else
    {
        hamlib_.setPollSeconds(settings.pollSeconds);
        hamlib_.setSwrTrip(settings.swrTrip);
    }
    if (settings.source == radio::RigSource::Hamlib)
        timer_.Start(TICK_MS);
    else
        timer_.Stop();
    sync();
    if (changed_) changed_();
}

void RigControl::stop()
{
    timer_.Stop();
    meters_.transmitting(false, HostConnection::nowMs());
    follower_.setEnabled(false);
    hamlib_.setPtt(false);
    hamlib_.stop();
    rigctld_->stop();
    started_ = false;
}

void RigControl::hostPtt(bool on)
{
    // rigctld isn't asked the dial while we transmit; the Hamlib rig is
    // asked its meters instead.
    hostPtt_ = on;
    rigctld_->setKeyed(on);
    follower_.host(on, HostConnection::nowMs());
    sync();
    syncRigctld();
}

void RigControl::hostLost()
{
    hostPtt_ = false;
    rigctld_->setKeyed(false);
    follower_.hostLost();
    sync();
    syncRigctld();
}

std::optional<uint64_t> RigControl::dialHz() const
{
    switch (settings_.source)
    {
    case radio::RigSource::Rigctld: return rigctld_->dialHz();
    case radio::RigSource::Hamlib: return hamlib_.status().hz;
    case radio::RigSource::None: break;
    }
    return std::nullopt;
}

const char* RigControl::meterPath() const
{
    return settings_.source == radio::RigSource::Rigctld ? "rigctld" : "Hamlib";
}

bool RigControl::readsAlc() const
{
    if (settings_.source == radio::RigSource::Rigctld) return rigctld_->readsAlc();
    if (settings_.source != radio::RigSource::Hamlib) return false;
    rig::HamlibRig::Status s = hamlib_.status();
    return s.open && s.readsAlc;
}

bool RigControl::readsSwr() const
{
    if (settings_.source == radio::RigSource::Rigctld) return rigctld_->readsSwr();
    if (settings_.source != radio::RigSource::Hamlib) return false;
    rig::HamlibRig::Status s = hamlib_.status();
    return s.open && s.readsSwr;
}

void RigControl::setFilterWatch(bool on)
{
    filterWatch_ = on;
    rigctld_->setFilterWatch(on);
    hamlib_.setFilterWatch(on);
}

std::optional<radio::RigFilter> RigControl::filter() const
{
    switch (settings_.source)
    {
    case radio::RigSource::Rigctld: return rigctld_->filter();
    case radio::RigSource::Hamlib: return hamlib_.status().filter;
    case radio::RigSource::None: break;
    }
    return std::nullopt;
}

bool RigControl::canSetFilter() const
{
    if (!settings_.earFilters) return false;
    switch (settings_.source)
    {
    case radio::RigSource::Hamlib: return hamlib_.status().open;
    case radio::RigSource::Rigctld: return rigctld_->up();
    case radio::RigSource::None: break;
    }
    return false;
}

void RigControl::setFilter(const radio::RigFilter& filter)
{
    // Only with "Let the Ear change the rig's filters" ticked; through a
    // shared rigctld too, as Jeff allowed (2026-10-26).
    if (!settings_.earFilters) return;
    if (settings_.source == radio::RigSource::Hamlib) hamlib_.setFilter(filter);
    if (settings_.source == radio::RigSource::Rigctld) rigctld_->setFilter(filter);
}

std::string RigControl::filterProblem() const
{
    switch (settings_.source)
    {
    case radio::RigSource::Hamlib: return hamlib_.status().filterProblem;
    case radio::RigSource::Rigctld: return rigctld_->filterProblem();
    case radio::RigSource::None: break;
    }
    return std::string();
}

std::string RigControl::rigKey() const
{
    switch (settings_.source)
    {
    case radio::RigSource::Hamlib: return "hamlib-" + std::to_string(settings_.model);
    case radio::RigSource::Rigctld: return "rigctld";
    case radio::RigSource::None: break;
    }
    return std::string();
}

bool RigControl::readsNoise() const
{
    if (settings_.source == radio::RigSource::Rigctld) return rigctld_->readsStrength();
    if (settings_.source != radio::RigSource::Hamlib) return false;
    rig::HamlibRig::Status s = hamlib_.status();
    return s.open && s.readsStrength;
}

std::optional<double> RigControl::noiseDb() const
{
    if (!readsNoise()) return std::nullopt;
    return noise_.dbOverS9(HostConnection::nowMs());
}

void RigControl::noiseReading(double dbOverS9, uint64_t nowMs)
{
    // Another band, another noise.
    std::optional<uint64_t> hz = dialHz();
    std::string band = hz ? radio::bandOf(*hz) : std::string();
    if (band != noiseBand_)
    {
        noiseBand_ = band;
        noise_.clear();
    }
    noise_.reading(dbOverS9, nowMs);
}

void RigControl::clearSwrTrip()
{
    if (!tripped_) return;
    tripped_ = false;
    tripText_.clear();
    hamlib_.clearSwrTrip();
    rigctldTrip_.clear();
    follower_.clearLockOut();
    sync();
    if (changed_) changed_();
}

bool RigControl::readsMeters() const
{
    if (settings_.source == radio::RigSource::Rigctld) return rigctld_->readsMeters();
    if (settings_.source != radio::RigSource::Hamlib) return false;
    rig::HamlibRig::Status s = hamlib_.status();
    return s.open && (s.readsAlc || s.readsSwr || s.readsPower);
}

std::string RigControl::describe() const
{
    if (settings_.source == radio::RigSource::Rigctld) return rigctld_->describe();
    if (settings_.source != radio::RigSource::Hamlib) return "No rig: Settings > Rig";
    rig::HamlibRig::Status s = hamlib_.status();
    std::string where = "Hamlib, " + (s.rig.empty() ? "rig " + std::to_string(settings_.model) : s.rig)
                        + (settings_.port.empty() ? std::string() : " on " + settings_.port) + ": ";
    std::string text;
    if (!s.open)
        text = where + (s.problem.empty() ? std::string("opening") : s.problem);
    else if (!s.problem.empty())
        text = where + s.problem;
    else if (s.keyed)
        text = where + "keyed";
    else
        text = where + "reading the dial every " + std::to_string(settings_.pollSeconds) + " s";
    if (settings_.keyRig)
    {
        text += follower_.keyed() ? "\nRagtime is keying the rig: data2g-host said PTT ON."
                                  : "\nRagtime keys the rig when data2g-host says PTT ON.";
        if (!follower_.why().empty()) text += "\nUnkeyed by Ragtime: " + follower_.why() + ".";
    }
    if (tripped_) text += "\nSWR trip: " + tripText_ + ".";
    return text;
}

void RigControl::syncRigctld()
{
    if (settings_.source != radio::RigSource::Rigctld || !started_) return;
    uint64_t now = HostConnection::nowMs();
    uint64_t metersBefore = meters_.version();
    bool tx = hostPtt_ && rigctld_->up();
    if (tx != meters_.transmitting()) rigctldTrip_.transmitting(tx);
    meters_.transmitting(tx, now);
    for (double dB : rigctld_->takeStrength()) noiseReading(dB, now);
    bool trippedNow = false;
    for (const radio::MeterReading& reading : rigctld_->takeReadings())
    {
        if (!tx) continue;
        meters_.reading(reading, now);
        if (reading.swr && rigctldTrip_.reading(*reading.swr, now) && !tripped_)
        {
            char ratio[32];
            std::snprintf(ratio, sizeof ratio, "SWR %.1f", *reading.swr);
            tripped_ = true;
            // Ragtime sets nothing through a shared rigctld.
            tripText_ = std::string(ratio) + " through rigctld; the rig itself was left alone";
            trippedNow = true;
        }
    }
    if (meters_.version() != metersBefore && metersChanged_) metersChanged_();
    if (trippedNow && trippedChanged_) trippedChanged_(tripText_);
}

void RigControl::onTimer(wxTimerEvent&)
{
    follower_.tick(HostConnection::nowMs());
    sync();
}

void RigControl::sync()
{
    rig::HamlibRig::Status s = hamlib_.status();
    // Keying failed, or the rig closed while keyed: unkey, and hold.
    if (s.pttFailures != seenFailures_)
    {
        seenFailures_ = s.pttFailures;
        follower_.rigFailed(s.problem);
    }
    // The SWR trip: the rig's thread has unkeyed it and turned its power
    // down; keep it unkeyed until the operator clears the trip.
    bool trippedNow = false;
    if (s.swrTrips != seenTrips_)
    {
        seenTrips_ = s.swrTrips;
        if (s.swrTripped && !tripped_)
        {
            tripped_ = true;
            tripText_ = s.tripText;
            follower_.lockOut("the SWR went over 3:1");
            trippedNow = true;
        }
    }
    hamlib_.setPtt(follower_.keyed());

    // The meters, burst by burst, from a Hamlib rig that is open.
    bool hamlib = settings_.source == radio::RigSource::Hamlib && started_;
    bool tx = hamlib && s.open && (hostPtt_ || follower_.keyed());
    hamlib_.setTransmitting(hamlib && hostPtt_);
    uint64_t metersBefore = meters_.version();
    uint64_t now = HostConnection::nowMs();
    meters_.transmitting(tx, now);
    // Only the meters read since last time, each reading counted once.
    radio::MeterReading fresh;
    if (s.alcReadings != seenAlc_) fresh.alc = s.meters.alc;
    if (s.swrReadings != seenSwr_) fresh.swr = s.meters.swr;
    if (s.powerReadings != seenPower_)
    {
        fresh.watts = s.meters.watts;
        fresh.power = s.meters.power;
    }
    seenAlc_ = s.alcReadings;
    seenSwr_ = s.swrReadings;
    seenPower_ = s.powerReadings;
    if (tx) meters_.reading(fresh, now);
    if (s.strengthReadings != seenStrength_)
    {
        seenStrength_ = s.strengthReadings;
        if (hamlib && s.strength) noiseReading(*s.strength, now);
    }
    if (meters_.version() != metersBefore && metersChanged_) metersChanged_();
    if (trippedNow && trippedChanged_) trippedChanged_(tripText_);

    if (s.version != seenVersion_ || follower_.version() != seenFollower_)
    {
        seenVersion_ = s.version;
        seenFollower_ = follower_.version();
        if (changed_) changed_();
    }
}
