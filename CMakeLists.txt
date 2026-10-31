# Ragtime: a chat and file-transfer client for Data2G.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#   ctest --test-dir build --output-on-failure
#
# -DRAGTIME_APP=OFF builds only the core and rig libraries and their tests,
# with no wxWidgets needed; -DRAGTIME_HAMLIB=OFF builds without Hamlib
# (libhamlib-dev), and so without Settings > Rig's Hamlib choice;
# -DRAGTIME_PIPEWIRE=OFF builds without the Ear's listening on Linux (libpipewire-0.3-dev; on Windows the Ear
# listens through WASAPI); -DRAGTIME_PICTURES=OFF builds without inline
# pictures (libopenjp2-7-dev, libwebp-dev, libheif-dev), and
# -DRAGTIME_HEIF=OFF without only HEIF; -DRAGTIME_RNNOISE=OFF builds
# without voice noise reduction (RNNoise, fetched with git). The Windows build cross-compiles with MinGW-w64: see
# cmake/Toolchain-mingw64.cmake and packaging/windows/make-zip.sh.
cmake_minimum_required(VERSION 3.16)

project(ragtime VERSION 0.2.0 LANGUAGES CXX)

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
if(WIN32 OR APPLE)
    set(RAGTIME_PIPEWIRE_DEFAULT OFF)
else()
    set(RAGTIME_PIPEWIRE_DEFAULT ON)
endif()
option(RAGTIME_PIPEWIRE "The Ear listens through PipeWire (Linux; Windows uses WASAPI); needs libpipewire-0.3-dev to build." ${RAGTIME_PIPEWIRE_DEFAULT})
option(RAGTIME_PICTURES "Inline pictures: JPEG 2000 (OpenJPEG) and WebP (libwebp); needs libopenjp2-7-dev and libwebp-dev." ON)
option(RAGTIME_HEIF "Pictures read from HEIF files (libheif with libde265); needs libheif-dev." ON)
option(RAGTIME_RNNOISE "Noise reduction for voice with RNNoise, fetched with git and built in (denoise/)." ON)
option(RAGTIME_BUILD_IMAGE_LIBS "Fetch and build OpenJPEG, libwebp, libde265 and libheif statically instead of using the system's (the Windows build)." OFF)
if(UNIX AND NOT APPLE AND NOT CMAKE_CROSSCOMPILING)
    set(RAGTIME_BUILD_SOTTO_DEFAULT ON)
else()
    set(RAGTIME_BUILD_SOTTO_DEFAULT OFF)
endif()
option(RAGTIME_BUILD_SOTTO "Build libsotto for voice when configuring, if none is found (packaging/build-sotto.sh; Linux)." ${RAGTIME_BUILD_SOTTO_DEFAULT})

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(RagtimeWarnings)

# libsotto, loaded at run time for voice: built here if none is found.
if(RAGTIME_BUILD_SOTTO)
    include(BuildSotto)
endif()

if(RAGTIME_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

# Noise reduction for the voice chain: RNNoise, which is C.
if(RAGTIME_RNNOISE)
    enable_language(C)
endif()
add_subdirectory(denoise)

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

# PipeWire's headers, for the Ear (audio/). Only the headers: libpipewire
# is loaded at run time.
if(RAGTIME_PIPEWIRE)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PIPEWIRE libpipewire-0.3)
    endif()
    if(NOT PIPEWIRE_FOUND)
        message(FATAL_ERROR "PipeWire's headers not found: install libpipewire-0.3-dev (Ubuntu, Debian), or pass "
                            "-DRAGTIME_PIPEWIRE=OFF to build without the Ear's listening.")
    endif()
endif()
add_subdirectory(audio)

# The picture codecs, as ragtime::openjpeg, ragtime::webp and ragtime::heif,
# for the image library.
if(NOT RAGTIME_PICTURES)
    set(RAGTIME_HEIF OFF)
endif()
if(RAGTIME_PICTURES)
    if(RAGTIME_BUILD_IMAGE_LIBS)
        include(BuildImageLibs)
    else()
        find_package(PkgConfig)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(OPENJPEG IMPORTED_TARGET libopenjp2>=2.5)
            pkg_check_modules(WEBP IMPORTED_TARGET libwebp)
            if(RAGTIME_HEIF)
                pkg_check_modules(HEIF IMPORTED_TARGET libheif)
            endif()
        endif()
        if(NOT OPENJPEG_FOUND OR NOT WEBP_FOUND)
            message(FATAL_ERROR "OpenJPEG 2.5 or libwebp not found: install libopenjp2-7-dev and libwebp-dev (Ubuntu, "
                                "Debian), or pass -DRAGTIME_PICTURES=OFF to build without inline pictures.")
        endif()
        if(RAGTIME_HEIF AND NOT HEIF_FOUND)
            message(FATAL_ERROR "libheif not found: install libheif-dev (Ubuntu, Debian), or pass -DRAGTIME_HEIF=OFF "
                                "to build without reading HEIF pictures.")
        endif()
        add_library(ragtime::openjpeg INTERFACE IMPORTED)
        target_link_libraries(ragtime::openjpeg INTERFACE PkgConfig::OPENJPEG)
        add_library(ragtime::webp INTERFACE IMPORTED)
        target_link_libraries(ragtime::webp INTERFACE PkgConfig::WEBP)
        if(RAGTIME_HEIF)
            add_library(ragtime::heif INTERFACE IMPORTED)
            target_link_libraries(ragtime::heif INTERFACE PkgConfig::HEIF)
        endif()
    endif()
endif()
add_subdirectory(image)

if(RAGTIME_APP)
    add_subdirectory(app)
endif()

if(RAGTIME_TESTS)
    add_subdirectory(tests)
endif()
