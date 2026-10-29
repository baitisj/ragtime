#!/bin/sh
# coolchic.sh PICTURE.png LAMBDA lop|mop|hop|vhop OUT.cool
# Fits Cool-chic to one picture (2000 steps, about 17 minutes on two CPU
# cores), writes the bitstream, decodes it beside it as OUT.png and prints
# the size, PSNR and how many bits/pixel the network weights took.
# COOLCHIC names a Cool-chic checkout (default ./Cool-Chic), run in the
# Python environment that has its requirements.
set -e
[ $# -eq 4 ] || { sed -n '2,7p' "$0"; exit 2; }
pic=$(realpath "$1"); lambda=$2; cfg=$3; out=$(realpath -m "$4")
cc=${COOLCHIC:-./Cool-Chic}
work=${out%.cool}.work
mkdir -p "$work"
cd "$cc"
start=$(date +%s)
python cc_encode.py -i "$pic" -o "$out" --workdir "$work" --lmbda "$lambda" \
    --n_itr 2000 --dec_cfg_residue "cfg/dec/intra/$cfg.cfg" > "${out%.cool}.log" 2>&1
echo "encoded in $(( $(date +%s) - start )) s"
python cc_decode.py -i "$out" -o "${out%.cool}.png" | grep "time ="
echo "$(stat -c%s "$out") bytes"
grep -A1 nn_bpp "${out%.cool}.log" | grep -v -i warn | tail -1 |
    awk '{print "weights " $2 " bpp, latents " $3 " bpp, PSNR " $5 " dB"}'
