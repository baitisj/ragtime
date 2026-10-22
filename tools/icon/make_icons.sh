#!/bin/sh -e
# Renders Ragtime's icon, app/res/ragtime.svg (drawn by make_leaf_svg.py),
# into the files the builds use:
#   app/res/ragtime.png  256 px, for the AppImage, the desktop file and the
#                        window icon on Linux
#   app/res/ragtime.ico  16 to 256 px, for ragtime.exe and its windows
# Run it from anywhere after changing the SVG, and commit all three files.
# Needs rsvg-convert (librsvg2-bin) and ImageMagick.

cd "$(dirname "$0")/../../app/res"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

rsvg-convert -w 256 -h 256 ragtime.svg -o ragtime.png
for size in 16 20 24 32 40 48 64 128 256; do
    rsvg-convert -w $size -h $size ragtime.svg -o "$tmp/$size.png"
done
convert "$tmp/16.png" "$tmp/20.png" "$tmp/24.png" "$tmp/32.png" "$tmp/40.png" \
    "$tmp/48.png" "$tmp/64.png" "$tmp/128.png" "$tmp/256.png" ragtime.ico
