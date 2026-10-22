# Working on Ragtime

Notes for a fresh Claude (or any newcomer) picking this project up. They
sum up what the planning sessions learned and what the owner, Jeff (AG7EW,
GitHub `baitisj`), has decided. Where these notes and the code disagree,
the code wins. Fix the note.

## What this is

Ragtime is a plain, friendly desktop chat and file-transfer client for
[Data2G](https://github.com/arodland/Data2G), Andrew Rodland's (KC2G) HF
data modem. It is a sibling of Glissando (`baitisj/glissando`), Jeff's
chirp-mode chat app, and takes its name from the same musical family and
from "ragchew". Two ideas frame everything:

- **Welcome to the D2G net.** D2G is a net that never closes: check in,
  net control asks for traffic, traffic gets passed, and every burst
  teaches the net who hears whom.
- **Sometimes we ragchew.** Operators enter a ragchew (a session) on
  purpose and leave it on purpose, and are off traffic duty meanwhile.

Read `docs/PLAN.md` first: it is the full design, with Jeff's decisions
and open questions. `docs/STAGE1.md` is the work now under way.
`docs/GEARBOX.md` and `docs/MOCKUPS.md` cover the gearshift and the
screens.

## Status

- 2026-10-22: name chosen (Ragtime), repository created, MIT licence,
  C++17 and wxWidgets chosen. Stage 1 (parity with Glissando's Data2G
  mode, and then some) has started.
- Package A (skeleton and CI) gives the layout below, a placeholder main
  window with the three tabs, and CI. Packages B, C and D build on it.
- The live design draft was the Claude Doc "Data2G client: plan"
  (https://claude.ai/code/artifact/87db2cd2-f5bc-4030-ba8d-99bef19c027d,
  private to Jeff). `docs/PLAN.md` is its 2026-10-22 snapshot and is now
  the record; change it in the same PR as the code it describes.
- Mock-ups: a private design canvas,
  https://claude.ai/artifact/4zWdK2uc5du4u4ryRjERNS. `docs/MOCKUPS.md`
  writes down everything a builder needs from it.

## Rules Jeff has set

**Data2G**

- Never pull Data2G code in: no vendoring, submodule, linking or copied
  snippets. Ragtime talks to a separately running `data2g-host` over TCP:
  KISS on 8100, commands on 8300, and the session byte stream on the
  command port + 1 (8301). Glissando's `docs/DATA2G.md` describes the
  command lines and reports as Glissando uses them.
- Never start `data2g-host`; the operator runs it. Ragtime never touches
  the sound card or PTT for data.
- Glissando keeps its own Data2G mode only until Ragtime is functionally
  equivalent (checklist in `docs/PLAN.md`), then drops it. Until then
  Glissando's Data2G mode gets fixes only. Never change Glissando's chirp
  modem behaviour from this project.

**Protocol**

- A new protocol from scratch, with no Glissando compatibility or
  interop. Broadcast group `D2G`, never `GLISS`.
- Every record carries the sender's callsign, a sequence number and the
  time, so a listener who fades in mid-stream knows who it is. No session
  tags in place of callsigns.
- Text is coded with Glissando's ham-table Huffman code (`HamText`), but
  signatures and seals cover the exact UTF-8 bytes typed, with no
  normalisation.
- Signed, never encrypted (Part 97, 97.113(a)(4)). No TLS on mesh links.
- No chain of custody or relay stamps: the originator's signature is
  enough.
- Never send an all-zero frame: it passes the CRC.
- 300 KB hard cap on any item over HF. Nodes relay up to 32 KB unattended
  by default.
- Sensitivity and timing windows are defined in Data2G modes and their
  airtimes (from the host's `MODES`), never in Glissando tempos.

**Sessions and the screen**

- A session (ragchew) starts only when the operator presses **Call** or
  accepts one, and ends only on **Leave**. Selecting things never opens or
  closes one, and a quiet link shows as quiet, not closed.
- Three tabs in this order: Broadcast, Session (pops open when a session
  starts, greyed when none), Files.
- The window works at 1280×720, and every dialog fits a 1366×768 screen
  (Jeff's radio PC has a short screen).
- Plain but fun: system theme and standard widgets, no Chaotica art. The
  globe ("Globus"), the gearshift and the 1950s radio vocabulary are
  welcome. "Curmudgeons" and "lids" are flavour words for the docs and UI
  copy, never defined terms in the spec.
- The radio is the source of truth for frequency: retune only when the
  operator asks.

## Layout

| Path | What |
| --- | --- |
| `core/` | `ragtime_core`, the socket-free library: no wx, no sockets in the engines, all unit-tested. Headers in `core/include/ragtime/`, sources in `core/src/`, one subfolder per area: `protocol/`, `link/`, `text/`, `files/` (package B), `globus/` (C), `gearbox/` (D). Add sources to `core/CMakeLists.txt` |
| `app/` | The wxWidgets app (`app/src/`). It draws and wires; logic goes in core. `MainFrame` is the one main window |
| `tests/` | ctest unit tests for core, plain executables on `tests/Check.h` (`CHECK`, `CHECK_EQ`). Add one with `ragtime_test(<name> <source>)` in `tests/CMakeLists.txt`. Fake hosts for link tests go here too |
| `tools/` | Reference scripts: `globe/` (sunlit globe renderer, admin-1 lines), `gearbox/` (ladders), `gcc16/check.py` (GCC 16 check) |
| `cmake/` | CMake helpers. `ragtime_warnings(<target>)` gives every target `-Wall -Wextra` and, with `RAGTIME_WERROR` (on by default), `-Werror` |
| `docs/` | The plan and specs. Change them in the same PR as the code they describe |

The version is `project(ragtime VERSION ...)` in the top-level
`CMakeLists.txt`; core gets it from the generated `ragtime/Version.h`.

## Build and test

Ubuntu 24.04 needs `build-essential cmake libwxgtk3.2-dev`.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j$(nproc)
    ctest --test-dir build --output-on-failure

Tests are on by default (`-DRAGTIME_TESTS=OFF` turns them off), and
every test runs headless, the app included (`ragtime --version`).
`-DRAGTIME_APP=OFF` builds only core and its tests, with no wx needed.

CI (`.github/workflows/build.yml`) runs on every PR and every push to
`main`: the Ubuntu 24.04 build with tests, and the same build and tests in
the `gcc:16` image with `-Werror` at `-O2` (RelWithDebInfo) and `-O3`
(Release). Before pushing C++, run `tools/gcc16/check.py` (needs docker;
start `dockerd &` in a cloud session): it compiles every source in
`gcc:16` at both levels without apt, which the cloud sandbox can't reach.

There is no Windows CI yet. Package I adds the MinGW-w64 cross build;
keep the CMake free of Linux-only assumptions meanwhile.

## Code

- C++17, wxWidgets, CMake, the same toolchain as Glissando, so its
  AppImage and MinGW-w64 Windows zip recipes carry over (Glissando
  `docs/APP.md`). There is no Mac or ARM build yet.
- Engines worth porting from Glissando (logic and tests, adapted to the
  new records): `Data2GLink` (KISS, command lines, `MODES`, session
  stream), `Data2GFileTransfer`, `Data2GBroadcast`, `Sha256`, `HamText`
  with `prototype/ham_table/` (table generator, phrases), and `Globe`
  (`app/src/gui/glissando/Globe.*`, `LandOutlines.h`, `BorderLines.h`,
  and `prototype/globe/` generators).
- **Licensing.** Ragtime is MIT. The files above live under Glissando's
  `app/` (LGPL-2.1 directory) but were written in that project (check
  `git log` before porting a file: its authors should be Claude or Jeff);
  Jeff relicenses them as MIT here. Never port `FrameCodec`, anything
  from freedv-gui, or anything with other contributors' work. Andrew's
  one Glissando commit (58c58ab) touched only `FrameCodec` and
  `Data2GTransport`, which are not ported.
- Include every standard header you use (`<algorithm>` for `std::min`,
  and so on). Jeff builds with GCC 16 and `-Werror`; new code must be
  warning-free there. Check in the `gcc:16` Docker image.
- Keep socket-free engines separate from wx, with unit tests under ctest,
  as Glissando's engines are. Fake hosts in tests; never a real
  `data2g-host` in CI.

## How Jeff works

- He merges PRs himself, sometimes within minutes. Check a PR's live
  state before pushing more to it; a push after a merge strands the work.
- PR descriptions open with a "Before:" paragraph and an "After:"
  paragraph, then "How".
- "Don't compile yet": hold builds and CI until he says the batch is
  done. "No CI" means `[skip ci]` commits, but never on a release PR.
- Lead with the answer, in plain words. He likes professor-style
  explanations of the maths.
- Don't comment on or push to other contributors' PRs unless Jeff asks.
  Andrew (arodland) is the Data2G author; his asks and answers go through
  Jeff.

## Jeff's radio PC (Hamlet)

Some work runs on Jeff's Linux machine, wired to his IC-7100 through
Hamlib. Never launch a radio app with his configuration or click its GUI
without his OK. The system `rigctld` (localhost:4532, shared with fldigi)
is his: never kill or restart it, and send only read-only queries. Tests
never use the radio's USB audio codec. Read `RADIO_SETUP.md` in his
Glissando checkout first. The two-host bench (two `data2g-host`s on
PulseAudio null sinks) is Jeff's to run there, since Data2G needs PyTorch.

## Cloud sandbox quirks

- Blocked hosts include `codeload.github.com` and GitHub archive and
  release downloads; `git clone` works. Point CMake `ExternalProject`
  URLs at local clones for a cloud-only build, and never commit that.
- Tag pushes are refused and a session can't create releases; Jeff tags
  and publishes in the GitHub UI.
- WebFetch of outside pages needs Jeff's OK.
