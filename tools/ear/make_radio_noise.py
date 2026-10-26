#!/usr/bin/env python3
"""Writes a pretend radio's receive audio for the Ear's bench: band noise
through an SSB filter (300 to 2700 Hz by default), a weak drifting
carrier, and Data2G-like bursts filling 350 to 2650 Hz (or 1250 to 1750 Hz
with --narrow) every few seconds. 48 kHz mono, 16 bits.

    tools/ear/make_radio_noise.py radio.wav [--seconds 60] [--low 300] [--high 2700]

tools/ear/pretend_radio.sh plays it into a PipeWire source for the Ear.
Needs numpy.
"""

import argparse
import wave

import numpy as np

RATE = 48000


def band(x, low, high, skirt=60.0):
    """Shapes x's spectrum: flat from low to high, falling 0.28 dB a Hz
    outside, as a DSP filter's skirt does, down to -skirt dB."""
    spectrum = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / RATE)
    outside = np.where(f < low, low - f, np.where(f > high, f - high, 0.0))
    gain_db = -np.minimum(skirt, outside * 0.28)
    return np.fft.irfft(spectrum * 10 ** (gain_db / 20), len(x))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--low", type=float, default=300)
    p.add_argument("--high", type=float, default=2700)
    p.add_argument("--narrow", action="store_true")
    a = p.parse_args()

    rng = np.random.default_rng(73)
    n = int(a.seconds * RATE)
    t = np.arange(n) / RATE
    noise = rng.normal(0, 0.02, n) * (1.0 - 0.2 * np.linspace(0, 1, n))
    signal = noise.copy()
    # A weak carrier drifting up 40 Hz over the minute.
    signal += 0.01 * np.sin(2 * np.pi * (1080 * t + 40 / (2 * a.seconds) * t * t))
    # Bursts: 4 s on, 7 s off.
    low, high = (1250, 1750) if a.narrow else (350, 2650)
    burst = band(rng.normal(0, 0.12, n), low, high, skirt=80)
    gate = ((t % 11.0) > 2.0) & ((t % 11.0) < 6.0)
    ramp = np.convolve(gate.astype(float), np.ones(480) / 480, mode="same")
    signal += burst * ramp
    # The rig's filter, then its own hiss after it.
    out = band(signal, a.low, a.high) + rng.normal(0, 0.0005, n)
    out = np.clip(out / max(1e-9, np.max(np.abs(out))) * 0.7, -1, 1)
    with wave.open(a.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((out * 32767).astype("<i2").tobytes())


if __name__ == "__main__":
    main()
