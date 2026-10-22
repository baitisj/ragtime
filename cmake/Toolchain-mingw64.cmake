# Cross-compiles ragtime.exe for 64-bit Windows on Ubuntu with the
# posix-thread MinGW-w64 GCC (package g++-mingw-w64-x86-64-posix). Use it
# with -DRAGTIME_BUILD_WX=ON, since there is no Windows wxWidgets to find:
#
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/Toolchain-mingw64.cmake \
#       -DCMAKE_BUILD_TYPE=Release -DRAGTIME_BUILD_WX=ON
#
# packaging/windows/make-zip.sh does this and zips the result.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Link the C++ runtime and winpthread in, so ragtime.exe (and each test)
# needs no MinGW DLLs beside it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# ctest runs the cross-built tests under Wine when it is installed.
find_program(RAGTIME_WINE NAMES wine wine64)
if(RAGTIME_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${RAGTIME_WINE})
endif()
