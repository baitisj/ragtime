#include <cstring>
#include <string>

#include "Check.h"
#include "ragtime/Version.h"

int main()
{
    const std::string version = ragtime::versionString();
    const std::string expected = std::to_string(RAGTIME_VERSION_MAJOR) + "."
                                 + std::to_string(RAGTIME_VERSION_MINOR) + "."
                                 + std::to_string(RAGTIME_VERSION_PATCH);
    CHECK_EQ(version, expected);
    CHECK_EQ(version, std::string(RAGTIME_VERSION_STRING));
    CHECK(std::strlen(ragtime::versionString()) >= 5);
    return ragtime_test::finish();
}
