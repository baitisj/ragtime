#!/bin/bash -e
#
# A pretend radio for the Ear's bench, on PipeWire with no sound card:
#
#   tools/ear/pretend_radio.sh up [radio.wav]   # a source "Pretend-radio" playing the file in a loop,
#                                               # and a stand-in data2g-host recording from it
#   tools/ear/pretend_radio.sh down
#
# The stand-in is pw-cat under the name data2g-host, so the Ear finds it
# as it finds the real one. Then open Ragtime's Ear (View > Ear), or run
# build/tests/ragtime_ear_probe. Never point this at a real radio's sound
# card: it only creates and uses its own source.
#
# Headless (a cloud session), start PipeWire first: a session bus,
# pipewire, wireplumber and pipewire-pulse, with XDG_RUNTIME_DIR set.

HERE="$(dirname "$(realpath "$0")")"
STATE="${XDG_RUNTIME_DIR:-/tmp}/ragtime-pretend-radio"

case "$1" in
up)
    WAV="${2:-$STATE/radio.wav}"
    mkdir -p "$STATE"
    [ -f "$WAV" ] || python3 "$HERE/make_radio_noise.py" "$WAV"
    # The "air" is a null sink the file plays into; the radio is a source
    # remapped from the air's monitor, an Audio/Source as a sound card's is.
    pactl load-module module-null-sink sink_name=pretend_air channel_map=mono \
        sink_properties=node.description=Pretend-air > "$STATE/modules"
    pactl load-module module-remap-source master=pretend_air.monitor source_name=pretend_radio channel_map=mono \
        source_properties=node.description=Pretend-radio >> "$STATE/modules"
    sleep 0.5
    (while true; do pw-play --target pretend_air "$WAV" || sleep 1; done) > /dev/null 2>&1 &
    echo $! > "$STATE/player"
    ln -sf "$(command -v pw-cat)" "$STATE/data2g-host"
    "$STATE/data2g-host" --record --target pretend_radio --rate 48000 --channels 1 /dev/null > /dev/null 2>&1 &
    echo $! > "$STATE/host"
    echo "Pretend radio up: source pretend_radio, stand-in data2g-host pid $(cat "$STATE/host")"
    ;;
down)
    for f in host player; do
        [ -f "$STATE/$f" ] && kill "$(cat "$STATE/$f")" 2> /dev/null || true
        rm -f "$STATE/$f"
    done
    pkill -f "pw-play --target pretend_air" 2> /dev/null || true
    if [ -f "$STATE/modules" ]; then
        tac "$STATE/modules" | while read -r m; do pactl unload-module "$m" 2> /dev/null || true; done
    fi
    rm -f "$STATE/modules"
    echo "Pretend radio down"
    ;;
*)
    echo "usage: $0 up [radio.wav] | down" >&2
    exit 1
    ;;
esac
