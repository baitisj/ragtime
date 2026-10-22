# The gearshift

How Ragtime picks a Data2G mode. Jeff asked for a gearshift built on the
Data2G Submode Map (2026-10-22). This replaces Glissando's five tempo
speeds.

## What the operator sees

One popup, opened from the width button on the connection bar or the
Gear button by Send. The popup *is* the Submode Map:

- Speed across (bits per second, log scale from 20 to 10k), the S/N each
  mode needs down the side (harder further down), one dot per mode,
  coloured by modulation: FSK `#0f766e`, QPSK `#1f5fbf`, 16-QAM `#7c3aed`,
  64-QAM `#b8326b`, 256-QAM `#9a3412`, polar `#a16207`. Hovering a dot
  shows its name, width, speed and threshold.
- **Width** chips, narrowest to widest: 200 Hz, 500 Hz, 1.2 kHz, 2.4 kHz
  (the map's four groups). A width is a cap: 2.4 kHz still uses the
  narrow modes. Modes wider than the cap are hollow grey circles. The
  one-press 500 Hz button stays on the connection bar.
- **Path** chips: Calm (no fading, AWGN), Good, Poor, Disturbed (the ITU
  F.1487 fading channels). The path picks the gearbox, because modes rank
  very differently once the path fades: polar modes rise on a good fading
  path and the QAM modes sink.
- The **gears** are numbered knobs joined by a heavy line; every other
  best mode sits on a thin dashed line. The current gear is filled blue.
- **Auto**: the path's S/N, from the station's last few bursts, is a
  dashed line; everything needing more than that, less a 3 dB margin, is
  shaded. Auto takes the fastest gear above the shading (ringed in green
  when Auto is off, as a suggestion).
- The gear list beside the map: gear, mode, speed, S/N on this path, how
  long 1 KB takes, and how many dB short the path is.
- Leave out the `ack-` modes: they carry the host's own ARQ control.

The connection bar shows `2.4 kHz ▼` and `500 Hz`; the button by Send
shows `Gear 3 · polar-k96-f4 · 139 bit/s ▼`.

## How the gears are chosen

For each width cap and path:

1. Keep the modes no wider than the cap that have a threshold on this
   path (a missing threshold means the mode never reached 90% copy).
2. The best modes are those no other mode beats on both speed and S/N:
   sort by threshold, and keep each mode faster than every one before it.
   That leaves 2 to 26 modes, too many to shift through.
3. Gear 1 is the most robust best mode. Each next gear is the most robust
   best mode at least 1.8 times faster than the gear below, so every shift
   roughly doubles the speed. The fastest best mode is always the top
   gear (it replaces the last gear if it is less than 1.3 times faster).

`tools/gearbox/gearbox_ladder.py` does exactly this. On every path,
2.4 kHz gets 8 gears.

## The figures

From the Data2G Submode Map (Jeff shared it 2026-10-22): the 10% failure
point of the smallest ARQ data burst (one control codeword and one data
codeword), 200 trials per point, S/N as average transmitted power over
noise in 2500 Hz. Longer bursts need a little more. Speed is the raw
payload rate without CRC or burst overhead.

Whether the polar modes can carry broadcast and session data, and be
picked with `BCAST MODE`, is an open ask: they are gears 2 and 3 on a good
fading path.

## Where Ragtime gets the figures

Nothing of Data2G's is built in (Jeff, 2026-10-22: "Nothing built in,
find Data2G"). Ragtime reads the figures at run time, each source adding
to or updating the one before:

1. **A Data2G checkout.** On launch Ragtime looks for data2g-host's own
   `data2g/codes_data/mode_thresholds.json` under `$DATA2G_HOME`, then
   under `Data2G` or `data2g` in the home folder or in its `src`, `git`,
   `code`, `projects`, `Projects`, `repos` or `dev` folder. Data2G is run
   from a clone (`uv sync`), so every operator running data2g-host has
   this file. Its form is `{"channels": ["awgn", "mpg", "mpp", "mpd"],
   "modes": {name: [dB, ...]}}`, 99 for a threshold never reached. It has
   no widths or speeds; those come from `MODES`.
2. **A thresholds file** picked with **Load thresholds...** in the popup
   (remembered as `/Gearshift/ThresholdsFile`), or else
   `submode_thresholds.json` in Ragtime's data folder
   (`~/.local/share/ragtime` on Linux, or `$XDG_DATA_HOME/ragtime`;
   `%APPDATA%\Ragtime` on Windows). Either form works: Data2G's
   own, or the Submode Map's JSON as `tools/gearbox/gearbox_ladder.py`
   reads it (`{name: {"hz", "mod", "rate", "bps", "th": {"awgn", ...}}}`,
   null for never), which carries widths and speeds, so it gives gears
   before the host connects.
3. **The host's `MODES` list.** Gears use only the modes the host offers.
   Its width and speed win over the file's: the speed is the payload rate
   from its airtimes, each codeword past the first adding
   (time at most codewords − time at one) / (most codewords − 1) seconds
   for its bytes. Thresholds in `MODES`, once Andrew adds them, win too.

With no thresholds found, the popup says where Ragtime looked. With
Data2G's file but no host yet, it says the gears appear once data2g-host
is connected.

The figures move as Data2G improves. Data2G's own file at `674c268`
(2026-10-22) differs from the Submode Map by up to a dB or so, enough to
change some ladders (with the map's speeds): on a Calm path at 500 Hz and
wider, `n4-qpsk-r1/3` and `n4-qpsk-r2/3` become gears 2 and 3, and on a
Good path at 1.2 kHz and wider the ladder starts at `polar-k96-f8`, with
`polar-k192-f8` in place of `polar-k96-f4`, one gear fewer.
Reading the operator's own file keeps the gears in step with the host they
run. The tables below are the Submode Map's.

## In the code

- `core/include/ragtime/gearbox/Submodes.h`: `SubmodeTable` (both file
  forms, `setFacts` and `setThreshold` for `MODES`),
  `payloadBitsPerSecond`, paths, modulations and their colours.
- `core/include/ragtime/gearbox/Gearbox.h`: `bestModes`, `gearLadder`
  (the same ladders as `gearbox_ladder.py`, checked on all 16 gearboxes),
  `autoGearIndex` with the 3 dB margin, `PathSnr` (the mean of the last
  5 bursts), the widths and their host commands, the labels, and
  `Gearshift`, the popup's state. Changing the width or path keeps the
  chosen mode if it is still a gear, or else takes the fastest gear no
  faster than it, so the gearbox never shifts up on its own. Auto with no
  S/N reports yet holds the chosen gear.
- `app/src/GearshiftDialog.*`: the popup. `app/src/Thresholds.*`: finding
  and reading the file. Until package E adds the width and Gear buttons,
  Settings > Gearshift (Ctrl+G) opens it and the status bar shows the gear.
- `tests/GearboxTest.cpp` uses made-up placeholder figures, never
  Data2G's.

## Every gearbox

**200 Hz, Calm path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `n4-qpsk-r1/5` | 51 | −10.1 dB |
| 2 | `n4-qpsk-r1/2` | 134 | −7.9 dB |
| 3 | `n4-qpsk-r2/3` | 176 | −6.8 dB |

**500 Hz, Calm path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −12.3 dB |
| 2 | `fsk8r50-r1/2` | 72 | −10.1 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −7.9 dB |
| 4 | `n10-qpsk-r1/2` | 333 | −3.5 dB |
| 5 | `n10-16qam-r1/2` | 672 | +1.2 dB |
| 6 | `n10-64l-r3/4` | 1544 | +8.6 dB |
| 7 | `n10-256l-r3/4` | 2067 | +13.4 dB |

**1200 Hz, Calm path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −12.3 dB |
| 2 | `fsk8r50-r1/2` | 72 | −10.1 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −7.9 dB |
| 4 | `qpsk-r1/5` | 319 | −4.9 dB |
| 5 | `qpsk-r1/2` | 806 | +0.1 dB |
| 6 | `n10-256l-r3/4` | 2067 | +13.4 dB |

**2400 Hz, Calm path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −12.3 dB |
| 2 | `fsk8r50-r1/2` | 72 | −10.1 dB |
| 3 | `fsk32r62-r1/2` | 151 | −7.9 dB |
| 4 | `qpsk-r1/5` | 319 | −4.9 dB |
| 5 | `w48-qpsk-r1/5` | 639 | −1.6 dB |
| 6 | `w48-qpsk-r1/2` | 1611 | +2.4 dB |
| 7 | `w48-16qam-r1/2` | 3278 | +7.6 dB |
| 8 | `w48-256l-r5/8` | 8222 | +18.5 dB |

**200 Hz, Good path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `n4-qpsk-r1/5` | 51 | −1.3 dB |
| 2 | `n4-qpsk-r1/2` | 134 | +0.3 dB |
| 3 | `n4-16qam-r1/3` | 176 | +2.1 dB |

**500 Hz, Good path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −3.0 dB |
| 2 | `n4-qpsk-r1/3` | 88 | +0.1 dB |
| 3 | `n4-16qam-r1/3` | 176 | +2.1 dB |
| 4 | `n10-qpsk-r1/2` | 333 | +5.7 dB |
| 5 | `n10-16qam-r1/2` | 672 | +10.9 dB |
| 6 | `n10-64l-r3/4` | 1544 | +18.9 dB |
| 7 | `n10-256l-r3/4` | 2067 | +25.8 dB |

**1200 Hz, Good path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −3.0 dB |
| 2 | `polar-k96-f8` | 69 | −1.9 dB |
| 3 | `polar-k96-f4` | 139 | −1.2 dB |
| 4 | `qpsk-r1/5` | 319 | +1.1 dB |
| 5 | `qpsk-r1/2` | 806 | +6.1 dB |
| 6 | `n10-256l-r3/4` | 2067 | +25.8 dB |

**2400 Hz, Good path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −3.0 dB |
| 2 | `polar-k96-f8` | 69 | −1.9 dB |
| 3 | `polar-k96-f4` | 139 | −1.2 dB |
| 4 | `qpsk-r1/5` | 319 | +1.1 dB |
| 5 | `w48-qpsk-r1/5` | 639 | +4.1 dB |
| 6 | `w48-qpsk-r1/2` | 1611 | +8.4 dB |
| 7 | `w48-16qam-r1/2` | 3278 | +13.8 dB |
| 8 | `w48-256l-r5/8` | 8222 | +25.8 dB |

**200 Hz, Poor path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `n4-qpsk-r1/2` | 134 | −2.8 dB |
| 2 | `n4-16qam-r1/3` | 176 | −2.1 dB |

**500 Hz, Poor path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −7.8 dB |
| 2 | `fsk8r50-r1/2` | 72 | −4.8 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −2.8 dB |
| 4 | `n10-qpsk-r1/2` | 333 | +1.6 dB |
| 5 | `n10-16qam-r1/2` | 672 | +11.6 dB |

**1200 Hz, Poor path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −7.8 dB |
| 2 | `fsk8r50-r1/2` | 72 | −4.8 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −2.8 dB |
| 4 | `qpsk-r1/5` | 319 | −1.0 dB |
| 5 | `qpsk-r1/2` | 806 | +4.1 dB |
| 6 | `16qam-r1/2` | 1639 | +9.9 dB |

**2400 Hz, Poor path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −7.8 dB |
| 2 | `fsk8r50-r1/2` | 72 | −4.8 dB |
| 3 | `n4-qpsk-r1/2` | 134 | −2.8 dB |
| 4 | `qpsk-r1/5` | 319 | −1.0 dB |
| 5 | `w48-qpsk-r1/5` | 639 | +2.6 dB |
| 6 | `w48-qpsk-r1/2` | 1611 | +7.2 dB |
| 7 | `w48-16qam-r1/2` | 3278 | +12.4 dB |
| 8 | `w48-256l-r5/8` | 8222 | +26.2 dB |

**200 Hz, Disturbed path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `n4-qpsk-r1/3` | 88 | +1.8 dB |
| 2 | `n4-16qam-r1/3` | 176 | +4.1 dB |

**500 Hz, Disturbed path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −8.2 dB |
| 2 | `fsk8r50-r1/2` | 72 | −5.6 dB |
| 3 | `n10-qpsk-r1/3` | 222 | +0.9 dB |
| 4 | `n10-16qam-r1/3` | 439 | +4.6 dB |
| 5 | `n10-16qam-r1/2` | 672 | +8.9 dB |

**1200 Hz, Disturbed path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −8.2 dB |
| 2 | `fsk8r50-r1/2` | 72 | −5.6 dB |
| 3 | `polar-k192-f8` | 153 | −2.2 dB |
| 4 | `qpsk-r1/5` | 319 | −0.2 dB |
| 5 | `qpsk-r1/2` | 806 | +4.2 dB |
| 6 | `16qam-r1/2` | 1639 | +10.3 dB |

**2400 Hz, Disturbed path**

| Gear | Mode | bit/s | S/N |
| --- | --- | --- | --- |
| 1 | `fsk16r25-r1/3` | 32 | −8.2 dB |
| 2 | `fsk8r50-r1/2` | 72 | −5.6 dB |
| 3 | `polar-k192-f8` | 153 | −2.2 dB |
| 4 | `qpsk-r1/5` | 319 | −0.2 dB |
| 5 | `w48-qpsk-r1/5` | 639 | +3.4 dB |
| 6 | `w48-qpsk-r1/2` | 1611 | +7.0 dB |
| 7 | `w48-16qam-r1/2` | 3278 | +11.9 dB |
| 8 | `w48-64l-r2/3` | 6556 | +23.1 dB |
