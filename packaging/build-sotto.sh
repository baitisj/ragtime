#!/bin/bash -e
#
# Builds libsotto at the commit pinned in sotto.sh, with the Codec 2 fork it
# links, and puts in OUT_DIR (default build-sotto/out):
#   libsotto.so.0, libcodec2.so.1.2 (what libsotto needs at run time)
#   licences/sotto.txt (MIT), licences/codec2.txt (LGPL-2.1)
# Needs cmake, make, a C compiler, git, and python3 with numpy (sotto writes
# its codebook from the NumPy file at build time). Set SOTTO_SOURCE to a
# checkout to build that instead of cloning.

cd "$(dirname "$(realpath "$0")")/.."
ROOT=$PWD
. packaging/sotto.sh
WORK=${SOTTO_WORK:-$ROOT/build-sotto}
OUT=${1:-$WORK/out}
SRC=${SOTTO_SOURCE:-$WORK/sotto}

if [ -z "$SOTTO_SOURCE" ]; then
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$SOTTO_REPO" "$SRC"
    fi
    git -C "$SRC" fetch --quiet origin "$SOTTO_COMMIT" 2>/dev/null || git -C "$SRC" fetch --quiet origin
    git -C "$SRC" -c advice.detachedHead=false checkout --quiet "$SOTTO_COMMIT"
fi

# Codec 2's library only, from sotto's own fork.
C2="$WORK/codec2"
cmake -S "$SRC/codec/codec2" -B "$C2" -DCMAKE_BUILD_TYPE=Release -DUNITTEST=OFF > /dev/null
cmake --build "$C2" --target codec2 -j"$(nproc)" > /dev/null

make -C "$SRC/c" --no-print-directory clean > /dev/null
make -C "$SRC/c" --no-print-directory libsotto.so CODEC2_LIB="$C2/src" > /dev/null

mkdir -p "$OUT/licences"
cp -L "$SRC/c/libsotto.so.0" "$OUT/libsotto.so.0"
C2LIB=$(ldd "$OUT/libsotto.so.0" | awk '/libcodec2/ {print $3}')
cp -L "$C2LIB" "$OUT/$(basename "$C2LIB")"
cp "$SRC/LICENSE" "$OUT/licences/sotto.txt"
cp "$SRC/codec/codec2/COPYING" "$OUT/licences/codec2.txt"
echo "libsotto ($(git -C "$SRC" rev-parse --short HEAD)) and $(basename "$C2LIB") in $OUT"
