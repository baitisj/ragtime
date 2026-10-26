# Ragtime: a chat and file-transfer client for Data2G.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#   ctest --test-dir build --output-on-failure
#
# -DRAGTIME_APP=OFF builds only the core and rig libraries and their tests,
# with no wxWidgets needed; -DRAGTIME_HAMLIB=OFF builds without Hamlib
# (libhamlib-dev), and so without Settings > Rig's Hamlib choice. The Windows build cross-compiles with MinGW-w64: see
# cmake/Toolchain-mingw64.cmake and packaging/windows/make-zip.sh.
cmake_minimum_required(VERSION 3.16)

project(ragtime VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RAGTIME_APP "Build the wxWidgets app (needs wxWidgets 3.2)." ON)
option(RAGTIME_TESTS "Build the unit tests and register them with ctest." ON)
option(RAGTIME_WERROR "Treat compiler warnings as errors." ON)
option(RAGTIME_BUILD_WX "Fetch and build wxWidgets statically instead of using the system's (the Windows build)." OFF)
option(RAGTIME_HAMLIB "Rig control with Hamlib (Settings > Rig); needs libhamlib-dev." ON)
option(RAGTIME_BUILD_HAMLIB "Fetch and build Hamlib statically instead of using the system's (the Windows build)." OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(RagtimeWarnings)

if(RAGTIME_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

# Hamlib, as ragtime::hamlib, for the rig library.
if(RAGTIME_HAMLIB)
    if(RAGTIME_BUILD_HAMLIB)
        include(BuildHamlib)
    else()
        find_package(PkgConfig)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(HAMLIB IMPORTED_TARGET hamlib)
        endif()
        if(NOT HAMLIB_FOUND)
            message(FATAL_ERROR "Hamlib not found: install libhamlib-dev (Ubuntu, Debian), or pass "
                                "-DRAGTIME_HAMLIB=OFF to build without rig control.")
        endif()
        add_library(ragtime::hamlib INTERFACE IMPORTED)
        target_link_libraries(ragtime::hamlib INTERFACE PkgConfig::HAMLIB)
    endif()
endif()
add_subdirectory(rig)

if(RAGTIME_APP)
    add_subdirectory(app)
endif()

if(RAGTIME_TESTS)
    add_subdirectory(tests)
endif()
