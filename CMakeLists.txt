# Ragtime: a chat and file-transfer client for Data2G.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#   ctest --test-dir build --output-on-failure
#
# -DRAGTIME_APP=OFF builds only the core library and its tests, with no
# wxWidgets needed. The Windows build cross-compiles with MinGW-w64: see
# cmake/Toolchain-mingw64.cmake and packaging/windows/make-zip.sh.
cmake_minimum_required(VERSION 3.16)

project(ragtime VERSION 0.1.0 LANGUAGES CXX)

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

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(RagtimeWarnings)

if(RAGTIME_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

if(RAGTIME_APP)
    add_subdirectory(app)
endif()

if(RAGTIME_TESTS)
    add_subdirectory(tests)
endif()
