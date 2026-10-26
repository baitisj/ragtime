#include "RigControl.h"

#include <utility>

#include "HostConnection.h"
#include "RigConnection.h"

using namespace ragtime;

namespace
{

// The rig's thread is looked at this often, and the 180 s cap checked.
constexpr int TICK_MS = 100;

} // namespace

RigControl::RigControl(std::function<void()> changed)
    : changed_(std::move(changed))
    , rigctld_(std::make_unique<RigConnection>([this]() {
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
    stop();
}

void RigControl::apply(const radio::RigSettings& settings)
{
    bool reopen = !started_ || !radio::sameRig(settings, settings_);
    settings_ = settings;
    started_ = true;
    follower_.setEnabled(settings.source == radio::RigSource::Hamlib && settings.keyRig);
    if (reopen)
    {
        rigctld_->stop();
        hamlib_.stop();
        seenFailures_ = 0;
        if (settings.source == radio::RigSource::Rigctld)
            rigctld_->start(settings.rigctldHost, settings.rigctldPort);
        else if (settings.source == radio::RigSource::Hamlib)
            hamlib_.start(settings);
    }
    else
    {
        hamlib_.setPollSeconds(settings.pollSeconds);
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
    follower_.setEnabled(false);
    hamlib_.setPtt(false);
    hamlib_.stop();
    rigctld_->stop();
    started_ = false;
}

void RigControl::hostPtt(bool on)
{
    // rigctld isn't asked while we transmit; nor is the Hamlib rig.
    rigctld_->setKeyed(on);
    follower_.host(on, HostConnection::nowMs());
    sync();
}

void RigControl::hostLost()
{
    follower_.hostLost();
    sync();
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
    return text;
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
    hamlib_.setPtt(follower_.keyed());
    if (s.version != seenVersion_ || follower_.version() != seenFollower_)
    {
        seenVersion_ = s.version;
        seenFollower_ = follower_.version();
        if (changed_) changed_();
    }
}
