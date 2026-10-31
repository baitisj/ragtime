#!/bin/bash -e
#
# Cross-compiles Ragtime for 64-bit Windows and zips it as
# dist/Ragtime-<version>-beta-<hash>-windows-x86_64.zip: ragtime.exe (one
# file, with wxWidgets, Hamlib, the picture codecs and the C++ runtime
# linked in), README.txt, LICENSE.txt and the libraries' licences in
# licences/. There is no installer.
#
# Needs Ubuntu 24.04's g++-mingw-w64-x86-64-posix, cmake, git, zip, and
# autoconf, automake and libtool for Hamlib. The build clones wxWidgets,
# Hamlib, OpenJPEG, libwebp, libde265 and libheif into build-win (set
# RAGTIME_WX_SOURCE or RAGTIME_HAMLIB_SOURCE to a checkout to use that
# instead) and takes a while the first time. Set
# BUILD_DIR to use another build tree. For voice messages, set
# RAGTIME_SOTTO_DLL_DIR to a folder holding sotto.dll and the libcodec2
# DLL it links (a MinGW-w64 build of baitisj/sotto at the commit in
# packaging/sotto.sh) and, if it has them, licences/sotto.txt and
# licences/codec2.txt: they go beside ragtime.exe. Without it the zip has
# no voice coding, and the microphone deck says so. With Wine installed,
# `ctest --test-dir build-win` runs the unit tests as Windows programs.

cd "$(dirname "$(realpath "$0")")/../.."
ROOT=$PWD
. packaging/release-name.sh
NAME="Ragtime-$RAGTIME_RELEASE-windows-x86_64"
BUILD_DIR=${BUILD_DIR:-$ROOT/build-win}

cmake -S . -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/Toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DRAGTIME_BUILD_WX=ON ${RAGTIME_WX_SOURCE:+-DRAGTIME_WX_SOURCE="$RAGTIME_WX_SOURCE"} \
    -DRAGTIME_BUILD_HAMLIB=ON ${RAGTIME_HAMLIB_SOURCE:+-DRAGTIME_HAMLIB_SOURCE="$RAGTIME_HAMLIB_SOURCE"} \
    -DRAGTIME_BUILD_IMAGE_LIBS=ON
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
# The libraries linked in, each under its own licence (README.txt says
# where their source and Ragtime's are, to rebuild it with others).
mkdir "$WORK/$NAME/licences"
DEPS="$BUILD_DIR/_deps"
for pair in \
    "wxWidgets:$DEPS/wxwidgets-src/docs/licence.txt" \
    "Hamlib:$DEPS/hamlib/src/hamlib_build/COPYING.LIB" \
    "OpenJPEG:$DEPS/openjpeg/src/openjpeg_build/LICENSE" \
    "libwebp:$DEPS/webp/src/webp_build/COPYING" \
    "libde265:$DEPS/de265/src/de265_build/COPYING" \
    "libheif:$DEPS/heif/src/heif_build/COPYING" \
    "RNNoise:$DEPS/rnnoise-src/COPYING"; do
    sed 's/$/\r/' "${pair#*:}" > "$WORK/$NAME/licences/${pair%%:*}.txt"
done

if [ -n "$RAGTIME_SOTTO_DLL_DIR" ]; then
    cp "$RAGTIME_SOTTO_DLL_DIR"/sotto.dll "$WORK/$NAME/"
    cp "$RAGTIME_SOTTO_DLL_DIR"/libcodec2*.dll "$WORK/$NAME/" 2>/dev/null || true
    for l in sotto codec2; do
        if [ -f "$RAGTIME_SOTTO_DLL_DIR/licences/$l.txt" ]; then
            sed 's/$/\r/' "$RAGTIME_SOTTO_DLL_DIR/licences/$l.txt" > "$WORK/$NAME/licences/$l.txt"
        fi
    done
else
    echo "No RAGTIME_SOTTO_DLL_DIR: the zip goes without sotto.dll (no voice coding)" >&2
fi

mkdir -p dist
rm -f "dist/$NAME.zip"
(cd "$WORK" && zip -9 -r "$ROOT/dist/$NAME.zip" "$NAME")
echo "Done: dist/$NAME.zip"
