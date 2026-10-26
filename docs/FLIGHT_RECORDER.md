# The flight recorder

Ragtime writes down everything that crosses the link to data2g-host, with
UTC times to the millisecond. When something odd happens on a fading path,
the tester picks **Help > Save a report** and sends one zip file instead of
a recollection, and a builder plays the host's side of it back to a
Ragtime on the bench with `ragtime_fake_air --replay`, at the exact
spacing the tester's Ragtime saw.

It is idea 2 of the "Ideas before signatures" doc (2026-10-25). Glissando's
best fixes came from logs with timestamps (PLAN.md, "What the air taught
us"), and the bugs still to find in sessions and cuts (Data2G PR #118)
are the ones only a real path shows.

## What is recorded

`HostConnection` is the only code with sockets, so it is the one place
that taps the recorder (`link::FlightRecorder`, in core). One line a
thing, `<UTC time> <tag> <what>`, where `>` is to the host and `<` from
it:

| Tag | What |
| --- | --- |
| `up` | the KISS and command ports are both connected: the address |
| `down` | the connection ended: why ("data2g-host closed its command port", "closed by Ragtime") |
| `cmd>`, `cmd<` | a command-port line each way, printable ASCII as it is, a backslash doubled, anything else `\xNN` |
| `kiss>`, `kiss<` | a KISS frame each way, the bytes between FENDs in hex, exactly as on the wire (command byte and escapes included) |
| `data-up`, `data-down` | the session data port (command port + 1) |
| `data>`, `data<` | session bytes each way: **the count only** |
| `note` | Ragtime opened or closed, Help > Save a report pressed, the head of each file, and each burst's transmit meters, read through Hamlib or rigctld (`note rig TX 12 s: ALC peak 61%, mean 44%; SWR 1.3; 38 W`), a change to data2g-host's TX level and why (`note TX level -2.0 dB: Auto ALC`, `set in Settings`, `Auto ALC off`, `Ragtime closed`), an SWR trip and its clearing (`note rig SWR trip: ...`), and how rigctld is answering, when it changes: connected, the first dial reading, an error, or every address failed (`note rigctld 127.0.0.1:4532: no rigctld: 127.0.0.1:4532 refused, [::1]:4532 refused`) |

```
2026-10-25T16:38:10.875Z up 127.0.0.1 KISS 8100 commands 8300
2026-10-25T16:38:10.876Z cmd> MYCALL AG7EW
2026-10-25T16:38:10.881Z cmd< BCAST PORT 2
2026-10-25T16:38:45.582Z kiss< 20a0021094218fdbdc0000006ade308d01077a22d8
2026-10-25T16:38:45.584Z cmd< BCAST 2 HEARD W1AW 13.3
2026-10-25T16:38:58.766Z data< 47
```

Lines are cut where the bytes say (CR or LF for commands, FEND for KISS),
not where a socket read happened to end, so a line split across two reads
is one line, timed when it was whole. Each line is flushed as it is
written: the lines just before a crash are the ones that matter.

Session bytes are counted, not kept. Commands and KISS frames are what
D2G puts on the air anyway (broadcasts are public, and command lines are
the host's chatter), but a session can carry a file the operator sent to
one station. Keeping them would let `--replay` play sessions too; it is a
one-line change if that is wanted.

## Where, and for how long

`appDataDir()/flight` (`~/.local/share/ragtime/flight` on Linux,
`%APPDATA%\Ragtime\flight` on Windows), or `<config>.flight` beside a
`--config` file, so two Ragtimes on a bench keep their own.
**Help > Open the flight recorder folder** opens it.

Files are named by the time they began, `flight-20261025-163810-586.log`,
and a new one starts at each UTC day and at 5 MB. When a file starts, the
recorder drops every file whose last possible day ended more than 7 days
ago, and then the oldest files until all of them fit in 50 MB. It never
drops the file it is writing. Other files in the folder are left alone.

How big is that? A line is about 60 bytes, plus 2 bytes of hex for each
KISS byte. The bench, with three bots chatting all the time and a session
up, wrote 4 KB in two minutes, about 3 MB a day; a real day on D2G is far
quieter. The 50 MB cap is for a station left running for a week with a
session moving files (session byte counts are a line each read).

## Help > Save a report

A zip, by default `ragtime-report-AG7EW-20261025-1640Z.zip` in the
documents folder, with:

- `README.txt`: what is inside, and how to replay it.
- `report.txt`: the Ragtime version, the system, the callsign, data2g-host's
  address and state, and the last connection error.
- `settings.txt`: every setting, `/Station/Callsign = AG7EW`, read through
  wxConfig, so it is the same from the registry on Windows.
- `flight/`: the recorder's files.

**Never a private key.** Ragtime has no keys yet, but signatures are
coming (PLAN.md). A setting whose name holds "private", "secret",
"password", "passphrase", "token", "seed", "recovery" or "mnemonic" is
written as `(left out)` (`link::reportableSetting`), and the report takes
nothing from `appDataDir()` but the flight folder. Whoever adds keys:
keep them out of both, and name their settings so the rule catches them.

## Replaying

    ragtime_fake_air --replay FILE|FOLDER... [--from TIME] [--speed X]

plays the host's side of a recording (a report's `flight/` folder, or some
of its files) on host A's ports, 127.0.0.1:8100 and 8300, in place of the
fake air and its bots. Point a Ragtime at it, with its own `--config`
file.

- The recording is cut into connections, each from an `up` to a `down`.
  When the Ragtime connects, the next connection plays: each `cmd<` line
  to the command port and each `kiss<` frame to the KISS port, as long
  after the Ragtime's first command line as it came after the recorded
  one. Anchoring on the first command line, not on the connect, keeps the
  host's `OK`s and `BCAST PORT` behind the questions they answer.
- At a `down` the ports close, and the next connection plays when the
  Ragtime reconnects (2 s later). A recording that breaks off without a
  `down` (Ragtime closed, or crashed) closes the ports there too; at the
  very end they stay open.
- What the Ragtime writes is read and printed, never answered: the
  recording answers. So a replay shows where the new Ragtime asks
  something the old one didn't.
- `--from 2026-10-25T16:20` starts at that recorded time (UTC; seconds and
  milliseconds optional), skipping the opening; `--speed 4` plays four
  times as fast.
- Session bytes weren't kept, so the data port takes a connection and
  prints the recorded counts, but carries nothing.

It prints each line it plays and each line the Ragtime writes, with the
recorded time:

```
16:38:10.875 Ragtime connected: playing connection 1 of 1, recorded ...
16:38:10.875 ragtime> MYCALL AG7EW
16:38:10.881 host< OK
16:38:45.582 host< KISS frame, 21 bytes
16:38:58.766 host< 47 session bytes (counted, not recorded)
```

The unit tests (`ragtime_flight_recorder_test`, `ragtime_replay_test`)
record a HostLink hearing another station through the fake hosts, replay
it into a fresh HostLink, and check it hears the same frame from the same
call.
