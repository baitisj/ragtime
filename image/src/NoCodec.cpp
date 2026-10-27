// The stand-in for a Ragtime built with -DRAGTIME_PICTURES=OFF: no
// pictures are made or shown, and the picture button says why.
#include "ragtime/image/Codec.h"

namespace ragtime
{
namespace image
{

bool available()
{
    return false;
}

bool heifAvailable()
{
    return false;
}

std::string versions()
{
    return "no picture codecs";
}

Load load(const uint8_t*, size_t, picture::Image&, std::string&)
{
    return Load::NotMine;
}

bool encode(const picture::Image&, uint64_t, Coded&, std::string& error)
{
    error = "this Ragtime was built without pictures (-DRAGTIME_PICTURES=OFF)";
    return false;
}

bool decode(const uint8_t*, size_t, picture::Image&, std::string& error, int)
{
    error = "this Ragtime was built without pictures (-DRAGTIME_PICTURES=OFF)";
    return false;
}

} // namespace image
} // namespace ragtime
