#!/bin/bash -e
#
# Builds dist/Ragtime-<version>-beta-<hash>-x86_64.AppImage.
#
# By default it configures and builds a Release tree in build-appimage. Set
# BUILD_DIR to package a tree you have already built (it must hold
# app/ragtime). The first run downloads linuxdeploy and its GTK plugin into
# packaging/linux/tools.
#
# The AppImage carries the libraries of the system it was built on, so build
# it on the oldest Linux it should run on (Ubuntu 24.04: glibc 2.39).

cd "$(dirname "$(realpath "$0")")/../.."
ROOT=$PWD
. packaging/release-name.sh
ARCH=$(uname -m)
OUT="$ROOT/dist/Ragtime-$RAGTIME_RELEASE-$ARCH.AppImage"

if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$ROOT/build-appimage"
    cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DRAGTIME_TESTS=OFF
    cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

TOOLS="$ROOT/packaging/linux/tools"
mkdir -p "$TOOLS"
if [ ! -x "$TOOLS/linuxdeploy-$ARCH.AppImage" ]; then
    curl -fL -o "$TOOLS/linuxdeploy-$ARCH.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"
    chmod +x "$TOOLS/linuxdeploy-$ARCH.AppImage"
fi
if [ ! -x "$TOOLS/linuxdeploy-plugin-gtk.sh" ]; then
    curl -fL -o "$TOOLS/linuxdeploy-plugin-gtk.sh" \
        "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
    chmod +x "$TOOLS/linuxdeploy-plugin-gtk.sh"
fi
export PATH="$TOOLS:$PATH"
# No FUSE in containers and CI: let the tools unpack themselves instead.
export APPIMAGE_EXTRACT_AND_RUN=1

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# The icon's file name must match Icon= in the desktop file.
cp app/res/ragtime.png "$WORK/ragtime.png"

cd "$WORK"
export LDAI_OUTPUT="Ragtime.AppImage"
"linuxdeploy-$ARCH.AppImage" \
    --appdir AppDir \
    --executable "$BUILD_DIR/app/ragtime" \
    --desktop-file "$ROOT/packaging/linux/ragtime.desktop" \
    --icon-file ragtime.png \
    --plugin gtk \
    --output appimage

mkdir -p "$ROOT/dist"
mv Ragtime.AppImage "$OUT"
"$OUT" --version
echo "Done: $OUT"
