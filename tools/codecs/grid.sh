#!/bin/sh
# grid.sh OUT.jpg "CAPTION" PICTURE ["CAPTION" PICTURE]...
# Puts pictures side by side, two to a row, each captioned above, as in
# the codec doc's comparison grids (ImageMagick).
set -e
out=$1; shift
tmp=$(mktemp -d)
n=0
while [ $# -ge 2 ]; do
    convert "$2" -resize 640x480! -gravity north -background white -splice 0x28 \
        -pointsize 20 -annotate +0+3 "$1" "$tmp/$(printf %02d $n).png"
    n=$((n + 1)); shift 2
done
i=0; rows=""
while [ $i -lt $n ]; do
    a=$tmp/$(printf %02d $i).png; b=$tmp/$(printf %02d $((i + 1))).png
    if [ -f "$b" ]; then convert "$a" "$b" +append "$tmp/row$i.png"; else cp "$a" "$tmp/row$i.png"; fi
    rows="$rows $tmp/row$i.png"; i=$((i + 2))
done
convert $rows -append -quality 92 "$out"
rm -r "$tmp"
