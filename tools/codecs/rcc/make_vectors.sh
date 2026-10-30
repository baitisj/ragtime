#!/bin/sh
# make_vectors.sh WORKDIR PICTURE.png NAME OUTDIR: test vectors for the C++
# decoder from a picture Cool-chic has fitted: NAME.rcc, its latents
# (NAME.rcc.latents.txt), the whole decode (NAME.png) and decodes of the
# first N bytes at several cuts (NAME-N.png), with what each decoded in
# NAME.manifest. Run with COOLCHIC set to a Cool-chic 5.0.1 checkout, in
# the Python environment that has its requirements.
set -e
[ $# -eq 4 ] || { sed -n '2,7p' "$0"; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
work=$(realpath "$1"); pic=$(realpath "$2"); name=$3; out=$(realpath -m "$4")
mkdir -p "$out"
cd "${COOLCHIC:-./Cool-Chic}"
export PYTHONPATH=".:$here"
quiet() { grep -v -i -E "warn|^ *warnings\.warn|Decoding frame"; }
python "$here/rcc_write.py" "$work" "$pic" "$out/$name.rcc" 2>&1 | quiet
size=$(stat -c%s "$out/$name.rcc")
{
    echo "# $name.rcc: $size bytes. For each decode: the bytes it had, then each"
    echo "# grid (coarsest first) as HxW and how many of its values were good."
    python "$here/rcc_read.py" "$out/$name.rcc" "$out/$name.png" 2>&1 | quiet
    for frac in 30 45 60 75 90; do
        n=$((size * frac / 100))
        python "$here/rcc_read.py" "$out/$name.rcc" "$out/$name-$n.png" "$n" 2>&1 | quiet
    done
} > "$out/$name.manifest"
cat "$out/$name.manifest" | grep bytes
