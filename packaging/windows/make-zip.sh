#!/bin/bash -e
#
# Cross-compiles Ragtime for 64-bit Windows and zips it as
# dist/Ragtime-<version>-beta-<hash>-windows-x86_64.zip: ragtime.exe (one
# file, with wxWidgets and the C++ runtime linked in), README.txt and
# LICENSE.txt. There is no installer.
#
# Needs Ubuntu 24.04's g++-mingw-w64-x86-64-posix, cmake, git and zip. The
# build clones wxWidgets into build-win (set RAGTIME_WX_SOURCE to a
# checkout to use that instead) and takes a while the first time. Set
# BUILD_DIR to use another build tree. With Wine installed,
# `ctest --test-dir build-win` runs the unit tests as Windows programs.

cd "$(dirname "$(realpath "$0")")/../.."
ROOT=$PWD
. packaging/release-name.sh
NAME="Ragtime-$RAGTIME_RELEASE-windows-x86_64"
BUILD_DIR=${BUILD_DIR:-$ROOT/build-win}

cmake -S . -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/Toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DRAGTIME_BUILD_WX=ON ${RAGTIME_WX_SOURCE:+-DRAGTIME_WX_SOURCE="$RAGTIME_WX_SOURCE"}
cmake --build "$BUILD_DIR" -j"$(nproc)"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/$NAME"
cp "$BUILD_DIR/app/ragtime.exe" "$WORK/$NAME/"
# Unstripped, the exe is half again as big.
x86_64-w64-mingw32-strip "$WORK/$NAME/ragtime.exe"
# Notepad copes with Unix line ends these days, but older ones don't.
sed 's/$/\r/' packaging/windows/README.txt > "$WORK/$NAME/README.txt"
sed 's/$/\r/' LICENSE > "$WORK/$NAME/LICENSE.txt"

mkdir -p dist
rm -f "dist/$NAME.zip"
(cd "$WORK" && zip -9 -r "$ROOT/dist/$NAME.zip" "$NAME")
echo "Done: dist/$NAME.zip"
