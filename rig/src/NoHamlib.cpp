// HamlibRig built without Hamlib (-DRAGTIME_HAMLIB=OFF): the Rig page
// greys its Hamlib choice, and a rig started anyway only says why.
#include "ragtime/rig/HamlibRig.h"

namespace ragtime
{
namespace rig
{

namespace
{

const char* const WHY = "This Ragtime was built without Hamlib";

} // namespace

bool available()
{
    return false;
}

std::string hamlibVersion()
{
    return WHY;
}

std::vector<Model> models()
{
    return {};
}

std::optional<Model> model(int)
{
    return std::nullopt;
}

std::vector<BackendParam> backendParams(int)
{
    return {};
}

struct HamlibRig::Shared
{
    Status status;
};

HamlibRig::HamlibRig() = default;

HamlibRig::~HamlibRig()
{
    stop();
}

void HamlibRig::start(const radio::RigSettings&)
{
    shared_ = std::make_shared<Shared>();
    shared_->status.running = true;
    shared_->status.problem = WHY;
    shared_->status.version = 1;
}

void HamlibRig::stop()
{
    shared_.reset();
}

void HamlibRig::setPollSeconds(int) {}

void HamlibRig::setPtt(bool) {}

void HamlibRig::setTransmitting(bool) {}

void HamlibRig::setSwrTrip(bool) {}

void HamlibRig::clearSwrTrip() {}

void HamlibRig::setDummyLevelForTest(const std::string&, double) {}

HamlibRig::Status HamlibRig::status() const
{
    return shared_ ? shared_->status : Status();
}

void HamlibRig::run() {}

} // namespace rig
} // namespace ragtime
