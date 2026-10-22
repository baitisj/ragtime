# ragtime_core: the socket-free engines (protocol records, Data2G link,
# file transfer, broadcast repair, text coding, Globus projection, the
# gearbox model). No wxWidgets here: everything in core is unit-tested
# under ctest without a display.

configure_file(Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/ragtime/Version.h @ONLY)

add_library(ragtime_core STATIC
    src/Version.cpp
    # Package B (protocol v0, Data2G link, file transfer, HamText, SHA-256):
    # sources under src/protocol/, src/link/, src/text/, src/files/.
    # Package C: Globus, the globe (projection, Natural Earth tables, light).
    src/globus/Globe.cpp
    src/globus/Sun.cpp
    # Package D (gearbox model): src/gearbox/.
    src/gearbox/Gearbox.cpp
    src/gearbox/Json.cpp
    src/gearbox/Submodes.cpp
)

target_include_directories(ragtime_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)
ragtime_warnings(ragtime_core)
