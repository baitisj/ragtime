#!/usr/bin/env python3
"""Build Ragtime's gear ladders from Data2G submode thresholds.

Usage: gearbox_ladder.py THRESHOLDS.json [--markdown]

THRESHOLDS.json maps a mode name to {"hz", "mod", "rate", "bps", "th"},
where "th" holds the 10% burst-failure S/N (dB in 2500 Hz) for "awgn",
"mpg", "mpp" and "mpd" (null when the mode never reached 90%). The figures
come from the Data2G Submode Map; the client should take them from the
host's MODES report once data2g-host offers it (an ask for Andrew).

For each width cap and path, the best modes are those no other mode beats
on both speed and S/N. Gear 1 is the most robust of them, and each next
gear is the most robust best mode at least RATIO times faster than the gear
below, so every shift roughly doubles the speed. ack- modes carry the
host's own ARQ control and are never gears.
"""
import json
import sys

RATIO = 1.8
WIDTHS = [200, 500, 1200, 2400]
PATHS = [("awgn", "Calm"), ("mpg", "Good"), ("mpp", "Poor"), ("mpd", "Disturbed")]


def best_modes(modes, path, width):
    usable = sorted((m["th"][path], -m["bps"], name) for name, m in modes.items()
                    if m["hz"] <= width and m["th"].get(path) is not None)
    fastest, best = 0, []
    for _, neg_bps, name in usable:
        if -neg_bps > fastest:
            fastest = -neg_bps
            best.append(name)
    return best


def gears(modes, path, width, ratio=RATIO):
    best = best_modes(modes, path, width)
    ladder = [best[0]]
    for name in best[1:]:
        if modes[name]["bps"] >= ratio * modes[ladder[-1]]["bps"]:
            ladder.append(name)
    # The fastest best mode is always the top gear.
    if ladder[-1] != best[-1]:
        if modes[best[-1]]["bps"] >= 1.3 * modes[ladder[-1]]["bps"]:
            ladder.append(best[-1])
        else:
            ladder[-1] = best[-1]
    return ladder


def main():
    modes = json.load(open(sys.argv[1]))
    modes = {n: m for n, m in modes.items() if "ack-" not in n}
    markdown = "--markdown" in sys.argv
    for path, label in PATHS:
        for width in WIDTHS:
            ladder = gears(modes, path, width)
            if markdown:
                print(f"\n**{width} Hz, {label} path**\n")
                print("| Gear | Mode | bit/s | S/N |\n| --- | --- | --- | --- |")
                for i, n in enumerate(ladder, 1):
                    print(f"| {i} | `{n}` | {modes[n]['bps']:.0f} | {modes[n]['th'][path]:+.1f} dB |")
            else:
                print(path, width, " | ".join(ladder))


if __name__ == "__main__":
    main()
