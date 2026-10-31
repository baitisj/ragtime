#!/bin/bash -e
#
# A pretend operating desk for voice's bench, on PipeWire with no sound
# card: a microphone, a speaker, and a radio whose two halves voice must
# leave out.
#
#   tools/voice/pretend_desk.sh up      # Pretend-microphone (a source hearing a hum and a voice-like buzz),
#                                       # Pretend-speaker (a sink), Pretend-radio in and out (one card), and
#                                       # a stand-in data2g-host recording from the radio and playing to it
#   tools/voice/pretend_desk.sh down
#
# The radio's halves share a device.id, as a USB codec's source and sink
# do, and the stand-in plays to the radio's output only, so the probe shows
# voice leaving out the input by its card. Then run
# build/tests/ragtime_voice_probe, or open Settings > Voice. Never point
# this at a real radio's sound card: it only creates and uses its own.
# What reached the speaker can be recorded from pretend_speaker.monitor.
#
# Headless (a cloud session), start PipeWire first: a session bus,
# pipewire, wireplumber and pipewire-pulse, with XDG_RUNTIME_DIR set.

STATE="${XDG_RUNTIME_DIR:-/tmp}/ragtime-pretend-desk"

case "$1" in
up)
    mkdir -p "$STATE"
    # Two seconds of a 150 Hz buzz with harmonics, voice-like, at about
    # -14 dBFS loudest, over a quiet hiss, looped into the room.
    python3 - "$STATE/voice.wav" <<'PY'
import sys, wave, numpy as np
rate = 48000
t = np.arange(rate * 2) / rate
buzz = sum(np.sin(2 * np.pi * 150 * k * t) / k for k in range(1, 12))
envelope = (np.sin(2 * np.pi * 1.5 * t) > 0).astype(float)
x = 0.12 * buzz * envelope + 0.001 * np.random.default_rng(1).standard_normal(t.size)
pcm = (np.clip(x, -1, 1) * 32767).astype(np.int16)
w = wave.open(sys.argv[1], "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
w.writeframes(pcm.tobytes()); w.close()
PY
    {
        pactl load-module module-null-sink sink_name=pretend_room channel_map=mono \
            sink_properties=node.description=Pretend-room
        pactl load-module module-remap-source master=pretend_room.monitor source_name=pretend_microphone \
            channel_map=mono source_properties=node.description=Pretend-microphone
        pactl load-module module-null-sink sink_name=pretend_speaker \
            sink_properties=node.description=Pretend-speaker
        pactl load-module module-null-sink sink_name=pretend_radio_out \
            "sink_properties=node.description=Pretend-radio-out device.id=9999"
    } > "$STATE/modules"
    # The radio's input: a virtual source on the same card as its output
    # (a remapped source can't carry a device.id).
    pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=pretend_radio_in \
        node.description=Pretend-radio-in media.class=Audio/Source/Virtual audio.position=[ MONO ] \
        device.id=9999 object.linger=true }" > /dev/null
    sleep 0.5
    (while true; do pw-play --target pretend_room "$STATE/voice.wav" || sleep 1; done) > /dev/null 2>&1 &
    echo $! > "$STATE/player"
    ln -sf "$(command -v pw-cat)" "$STATE/data2g-host"
    (while true; do "$STATE/data2g-host" --playback --target pretend_radio_out "$STATE/voice.wav" || sleep 1; done) \
        > /dev/null 2>&1 &
    echo $! > "$STATE/host"
    echo "Pretend desk up: pretend_microphone, pretend_speaker; the radio's pretend_radio_in and pretend_radio_out"
    ;;
down)
    for f in host player; do
        [ -f "$STATE/$f" ] && kill "$(cat "$STATE/$f")" 2> /dev/null || true
        rm -f "$STATE/$f"
    done
    pkill -f "pw-play --target pretend_room" 2> /dev/null || true
    pkill -f "data2g-host --playback --target pretend_radio_out" 2> /dev/null || true
    pw-cli destroy pretend_radio_in > /dev/null 2>&1 || true
    if [ -f "$STATE/modules" ]; then
        tac "$STATE/modules" | while read -r m; do pactl unload-module "$m" 2> /dev/null || true; done
    fi
    rm -f "$STATE/modules"
    echo "Pretend desk down"
    ;;
*)
    echo "usage: $0 up | down" >&2
    exit 1
    ;;
esac
