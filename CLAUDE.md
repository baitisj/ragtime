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
from "ragchew". Three ideas frame everything:

- **Welcome to the D2G net.** D2G is a net that never closes: check in,
  net control asks for traffic, traffic gets passed, and every burst
  teaches the net who hears whom.
- **Sometimes we ragchew.** Operators enter a ragchew (a session) on
  purpose and leave it on purpose, and are off traffic duty meanwhile.
- **Keep in touch.** Every burst heard teaches Ragtime when and on which
  band each friend gets through, so it can say who is likely around now
  and when to check in to find them (Jeff, 2026-10-25: a core value, to
  stress in the docs). Recent work and the path strips are where it
  shows.

Read `docs/PLAN.md` first: it is the full design, with Jeff's decisions
and open questions. `docs/STAGE1.md` is the work now under way.
`docs/PROTOCOL.md` is what goes on the air (records, frames, sessions,
text coding).
`docs/GEARBOX.md` and `docs/MOCKUPS.md` cover the gearshift and the
screens. `docs/DATA2G.md` is what data2g-host does at the edges Ragtime
touches. `docs/FLIGHT_RECORDER.md` is the host-link recorder, the report
and `--replay`. `docs/HOW_SEALS_WORK.md` is the plain-words primer on seals and
repair (its figures are SVGs in `docs/images/how-seals-work/`); change it
with the code when the seal rules change.

## Status

- 2026-10-22: name chosen (Ragtime), repository created, MIT licence,
  C++17 and wxWidgets chosen. Stage 1 (parity with Glissando's Data2G
  mode, and then some) has started.
- Package A (skeleton and CI) gives the layout below, a placeholder main
  window with the three tabs, and CI. Packages B, C and D build on it.
- Package B (protocol v0 and core library): `docs/PROTOCOL.md`, the
  records and streams (`core/src/protocol/`), HamText table 0
  (`core/src/text/`, identical to Glissando's), `Data2GLink` and
  `HostLink` (`core/src/link/`), `Sha256` and `SessionFiles`
  (`core/src/files/`), and a fake data2g-host for tests
  (`tests/FakeHost.*`). Ported from Glissando: Data2GLink, Sha256,
  HamText with its generator, Data2GFileTransfer (now `SessionFiles`).
- Package D (gearshift) gives `core/src/gearbox/` and the popup
  (`app/src/GearshiftDialog.*`). It reads Data2G's thresholds from the
  operator's own file at run time; none are committed
  (`docs/GEARBOX.md`).
- Package E (app shell and Broadcast tab) gives `core/src/broadcast/`
  (`Stations`, the station list model; `BroadcastChat`, the Broadcast
  tab's engine, and `BroadcastLog`, which keeps it on disk so Ragtime
  opens with what came before, a rule where it last closed),
  `link::Reconnect`, the window around them
  (`app/src/MainFrame`, `ConnectionBar`, `StationList`, `BroadcastPage`,
  `SettingsDialog`, `HostConnection` for the sockets) and the bench
  `ragtime_fake_air` (below).
- Package F (sessions) gives `core/src/session/` (`SessionChat`, the
  Session tab's engine, with `StreamAccount` for the `BUFFER` counts),
  QRL and QRV broadcasts (`BroadcastChat::announce`), the session partner
  pinned in `Stations`, the session data port in `HostConnection`, and
  `app/src/SessionPage`. `SessionLog` keeps each station's sessions on
  disk (`sessionLogDir()` in `Prefs`), so a new session with a station
  carries on below the earlier ones, a rule where each ended.
- Package G (session files and the Files tab) gives `files::FileTraffic`
  (`core/src/files/`, `SessionFiles` plugged into the session: pacing,
  Leave, rates, the log), `files/FileRows.h` (the Files tab's rows and
  its lasting log, `FileHistory`, which package H fills too), the Files
  tab (`app/src/FilesPage`), offer cards and the send queue in
  `SessionPage`, and the files settings in `Prefs` and
  `SettingsDialog`.
- Package H (broadcast files with repair) gives, in `core/src/broadcast/`,
  `Airtime` (burst costs in Data2G mode airtimes, frame and burst
  packing, the file estimate), `Seals` (sealing what we broadcast,
  listening windows, resend rounds, and asking for repairs with backoff
  and suppression) and `BroadcastFiles` (files on D2G, feeding the Files
  tab through `FileRows.h`, with `BroadcastParts` keeping files not yet
  whole on disk across restarts, in `broadcastPartsDir()`); file cards and the file button (a floppy disk, drawn in `DrawnIcon`) in `BroadcastPage`,
  the File menu items and share menus in `MainFrame`, and the seal chain
  in `Prefs`. Since 2026-10-24 a repair request carries the asker's S/N,
  repairs go gear by gear (`Seals::setLadder`, the per-round cap in
  Settings), and a sender refuses what it won't repair (RepairRefused,
  `0x42`). Since 2026-10-25 the S/N is the mean of the last 5 bursts,
  repairs have a floor, a per-file gear and an airtime cap
  (`setRepairFloor`, `setRepairGear`, `setRepairCapSeconds`), and a
  closed share can re-open (`BroadcastFiles::reopen`). Since 2026-10-25 repairs are Reed-Solomon parity
  (`files/Parity`, PROTOCOL.md "Parity"): requests (`0x43`) ask how
  many repair symbols they want, files get parity pieces and sealed
  chat ItemParity (`0x44`), and `Seals::takeRebuilt` hands back lines
  rebuilt from it. 0.1's request (`0x41`) is skipped, so 0.1 can't
  repair with it. A station that heard a file but lost every seal over
  it asks for the seal by the file, under seal 0 (`SEAL_LATE_MS`).
- Package I (packaging and the first release) gives the app icon
  (`app/res/`, drawn by `tools/icon/`), the Linux
  AppImage and the portable Windows zip (`packaging/`), the MinGW-w64
  cross build against a static wxWidgets (`cmake/Toolchain-mingw64.cmake`,
  `-DRAGTIME_BUILD_WX=ON`), and their CI jobs. 0.1.0-beta is the first
  release; 0.1.1-beta adds the session file cancel fix (#49) with the
  same protocol. Stage 1 is done once Jeff has run the two-host bench on
  Hamlet.
- Data2G PR #118 (CLEANTXBUFFER, DATA2G ON) gives cuts, for 0.1.2:
  `HostLink` asks for DATA2G ON (WRONG from an older host is fine),
  `SessionChat` cuts and mends the stream from `session::StreamLog`,
  and `FileTraffic` keeps a flat 8 KB window where the host can cut
  (docs/PROTOCOL.md, "Cuts"). The fake hosts cut too (`--no-cut` for an
  older one).
- Data2G PR #121 (broadcast v3, open 2026-10-25) frames each broadcast
  frame with COBS and a zero, so a lost codeword loses only the frames
  it touches. Ragtime counts frames framed (`link::framedBytes`,
  `link::burstDataBytes`, for v2 and v3 alike), makes file pieces k
  codewords less 3 bytes, and keeps its bursts long; shorter bursts on fading
  paths (ragtime #68's first version) were dropped for it
  (docs/DATA2G.md, "Loss").
- The dial, the path log and the watch list (2026-10-25,
  `docs/STAGE1.md`): `core/src/radio/` (`DialReader`, which only ever
  asks rigctld `\chk_vfo`, `f` and, while transmitting, the meters
  `+l ALC`, `+l SWR` and `+l RFPOWER_METER_WATTS`, naming `currVFO` for
  a `--vfo` one; `Bands`, the US data segments; `PathLog`, a line a
  burst heard, appended, and each station's 24-hour strip by band),
  `broadcast::WatchList` (bells, and Tell me when free), and in the app
  `RigConnection`, `PathStripView`, `PathsDialog` and `WatchDialog`.
  Receive only outside the US data segments ticks PLAN.md's last parity
  box.
- Recent work (2026-10-25, `docs/STAGE1.md`): `radio::RecentWork` ranks
  the path log's stations, by default by (1 + days worked) × (1 + days
  heard at this hour on this band); contacts (a session, an `@` message
  either way) are `work` lines in the path log. The pane is
  `app/src/RecentWorkPane`, under the station list in the `left_`
  splitter.
- Rig control (2026-10-26, Andrew's suggestion, `docs/STAGE1.md`):
  Settings > Rig picks no rig, the dial from rigctld (read only), or
  the rig opened with Hamlib (`radio::RigSettings`, `radio::hamlibConf`;
  `rig::HamlibRig` in `rig/`, on its own thread), with "Key the rig when
  data2g-host keys" (`radio::PttFollower`, off by default) following
  the host's PTT ON/OFF. `app/src/RigControl` holds whichever is
  picked; Settings is now two tabs, Station and Rig. While the host
  transmits, the rig's ALC, SWR and RF power are read, through Hamlib or
  rigctld (`radio::TxMeters`, burst by burst) for the Auto ALC meter and the
  flight recorder; Auto ALC itself still waits for a host TX level
  command, but each burst works out what it would do. The SWR bar sits
  under the ALC meter (`app/src/SwrBar`), and the SWR trip (Settings >
  Rig, off by default, `radio::SwrTrip`) sends the host ABORT and holds
  sending until the operator clears it; with Hamlib it also unkeys the
  rig and sets its power to 0 (through rigctld it sets nothing).
- The flight recorder (2026-10-25, idea 2 of "Ideas before
  signatures"): `link::FlightRecorder` keeps every command line, KISS
  frame and session byte count to and from the host, with millisecond
  UTC times, in `flightRecorderDir()` (7 days, 50 MB). Help > Save a
  report zips it with the settings (`app/src/Report`), never a setting
  named like a secret (`link::reportableSetting`): keep any future
  private key out of wxConfig names that rule misses, and out of the
  flight folder. `ragtime_fake_air --replay` plays it back
  (`tests/ReplayHost`).
- Data2G PR #98 (merged 2026-10-23) gives SNRs: on `BCAST HEARD` and
  `MISSED`, and `SN` for the session partner. Ragtime shows them in the
  station list's SNR column and feeds the gearshift's Auto the weakest
  station heard in 15 minutes (`Stations::weakestPath`). Hosts from
  before it still work, without either (`docs/STAGE1.md`, "SNR from
  data2g-host").
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
  command port + 1 (8301). `docs/DATA2G.md` sums up the host as Ragtime
  meets it, read from Data2G's source at a pinned commit.
- Don't guess how Data2G behaves: read it (Andrew's advice, 2026-10-25).
  `git clone https://github.com/arodland/Data2G` works in a cloud
  session; start with its `docs/broadcast.md`, `docs/arq.md`,
  `data2g/host.py`'s docstring and `data2g/kisslink.py`. Reading is fine,
  copying code is not. When something you need isn't in
  `docs/DATA2G.md`, or Data2G has moved past its commit, read the source
  and update that page in the same PR.
- Never start `data2g-host`; the operator runs it. Ragtime never touches
  the sound card. It keys the rig only with "Key the rig when
  data2g-host keys" ticked (Settings > Rig, Hamlib only, off by
  default), following the host's PTT ON/OFF and unkeying by itself on
  a dropped link, a rig failure, quitting and after 180 s (Andrew's
  suggestion, 2026-10-26, for rigs whose drivers can't be opened twice).
  With the SWR trip ticked, on two readings over 3:1 it sends the host
  ABORT, and with Hamlib also unkeys the rig, whoever keyed it.
- Glissando dropped its own Data2G mode on 2026-10-22 (glissando PR
  #113), so Ragtime is the only Data2G client. Never change Glissando's
  chirp modem behaviour from this project.

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
- Plain chat must work on a poor link with no seal or signature holding
  it up. Never call seals cheap: at Data2G's lowest working rungs, 350
  bytes take 30 to 60 s to send reliably (Andrew, 2026-10-22).
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
  operator asks. Ragtime reads the dial and the transmit meters from
  rigctld (`\chk_vfo`, `f` and `+l` levels only, never a set command:
  the operator's rigctld is shared), or from the rig it
  opens with Hamlib, where PTT is the only thing it sets, and the SWR
  trip (off by default) the only thing that sets the power, to 0. It
  never keys through rigctld (NET rigctl is left out of the Hamlib
  list).

## Layout

| Path | What |
| --- | --- |
| `core/` | `ragtime_core`, the socket-free library: no wx, no sockets in the engines, all unit-tested. Headers in `core/include/ragtime/`, sources in `core/src/`, one subfolder per area: `protocol/`, `link/`, `text/`, `files/` (package B; G adds `FileRows`, `FileTraffic`), `globus/` (C), `gearbox/` (D), `broadcast/` (E, with `BroadcastLog`; H adds `Airtime`, `Seals`, `BroadcastFiles`), `session/` (F, and `SessionLog`; `StreamLog` for cuts), `radio/` (the dial from rigctld, the US data segments, the path log, Recent work, the rig settings and the PTT follower). Add sources to `core/CMakeLists.txt` |
| `rig/` | `ragtime_rig`: `rig::HamlibRig`, the rig opened with Hamlib on a thread of its own, Hamlib's rig list and the serial ports. Kept out of core (no ports there); with `-DRAGTIME_HAMLIB=OFF` a stand-in that says Hamlib is missing |
| `app/` | The wxWidgets app (`app/src/`). It draws and wires; logic goes in core. `MainFrame` is the one main window and owns the engines; `HostConnection` (data2g-host) and `RigConnection` (rigctld, read only) are the only code with sockets, and `RigControl` holds the rig as Settings > Rig has it; `Prefs` holds every wxConfig key, and `appDataDir()` is where Ragtime keeps its own files (never wx's `GetUserDataDir()`: on Linux that is `~/.Ragtime`, the settings file); `app/src/globus/` is the Globus pane (`GlobusPanel`, `GlobusView`, `GlobusToggle`) |
| `tests/` | ctest unit tests for core, plain executables on `tests/Check.h` (`CHECK`, `CHECK_EQ`). Add one with `ragtime_test(<name> <source>)` in `tests/CMakeLists.txt`. `FakeHost.*` is a fake data2g-host on loopback sockets (POSIX, so its test and the bench build only `if(UNIX)`); `FakeAirMain.cpp` is the bench `ragtime_fake_air`, and `ReplayHost.*` its `--replay` |
| `tools/` | Reference scripts: `icon/` (`make_leaf_svg.py` draws the icon, the Maple Leaf Rag's leaf going digital, as `app/res/ragtime.svg`; `make_icons.sh` renders its PNG and ICO), `globe/` (sunlit globe renderer, and the generators of the map tables in `core/src/globus/`: coasts and borders from Natural Earth, the big lakes from `ne_110m_lakes.geojson`, state and province lines from Natural Earth's 1:10m admin-1 lines, fetched for the run, for the countries in `gen_admin1_lines.py`), `gearbox/` (ladders), `ham_table/` (text table 0 generator and corpus: `gen_table.py --check`), `gcc16/check.py` (GCC 16 check) |
| `cmake/` | CMake helpers. `ragtime_warnings(<target>)` gives every target `-Wall -Wextra` and, with `RAGTIME_WERROR` (on by default), `-Werror`; `Toolchain-mingw64.cmake` (the Windows cross build), `BuildWxWidgets.cmake` (wxWidgets 3.2.9 cloned and built statically with `-DRAGTIME_BUILD_WX=ON`), `BuildHamlib.cmake` (Hamlib 4.5.5 cloned and built statically with autotools, `-DRAGTIME_BUILD_HAMLIB=ON`), `EmbedFile.cmake` (the window icon built in) |
| `packaging/` | Release recipes: `linux/make-appimage.sh` (with `ragtime.desktop`), `windows/make-zip.sh` (with the zip's `README.txt`). Both write to `dist/`, named by `release-name.sh` |
| `docs/` | The plan and specs. Change them in the same PR as the code they describe |

The version is `project(ragtime VERSION ...)` in the top-level
`CMakeLists.txt`; core gets it from the generated `ragtime/Version.h`.

## Build and test

Ubuntu 24.04 needs `build-essential cmake pkg-config libwxgtk3.2-dev
libgtk-3-dev libhamlib-dev` (GTK for the ham phrase highlight in the
entry lines, `PhraseHighlight`; Hamlib for Settings > Rig, or
`-DRAGTIME_HAMLIB=OFF` to build without it).

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j$(nproc)
    ctest --test-dir build --output-on-failure

Tests are on by default (`-DRAGTIME_TESTS=OFF` turns them off), and
every test runs headless, the app included (`ragtime --version`).
`-DRAGTIME_APP=OFF` builds only core, the rig library and their tests,
with no wx needed. Rig tests use Hamlib's dummy rig (model 1), never a
real radio.

CI (`.github/workflows/build.yml`) runs only the Ubuntu 24.04 build with
tests on each PR (Jeff's choice, 2026-10-22: threads already build and
test before pushing). Nothing runs on a push to `main`. The full set runs
only when started by hand (Actions > Build and test > Run workflow, or
the GitHub `actions_run_trigger` tool on the branch): Ubuntu, the same
build and tests in the `gcc:16` image with `-Werror` at `-O2`
(RelWithDebInfo) and `-O3` (Release), the Windows cross build with the
unit tests run under Wine, and the AppImage. The last two keep the zip and
the AppImage as the run's artifacts. Start a full run on a PR that
changes CMake, packaging or platform code, and always on a release PR. Before pushing C++, run `tools/gcc16/check.py` (needs docker;
start `dockerd &` in a cloud session): it compiles every source in
`gcc:16` at both levels without apt, which the cloud sandbox can't reach.

**The bench.** `build/tests/ragtime_fake_air` runs two fake data2g-hosts
on one shared air (A: KISS 8100, commands 8300; B: 8110 and 8310) and a
few bot stations that check in, chat and answer `@` messages. Point a
Ragtime at each host with its own settings file, `ragtime --config
a.conf`, to try the window with no Data2G and no radio. The bots that
take sessions answer a Call and ragchew (send them 73 and they leave);
`--call AG7EW` has one call you half a minute in and offer you its
bulletin 20 s later, and `--fade` gives their sessions a 45 s fade every
two minutes, for Link quiet. Bots save the files you send them (in the
temp folder) and offer their bulletin again when you mention a file;
`--rate BPS` caps session bytes (400 a second by default) so transfers
take a while. `--replay FOLDER` plays a flight recording (a report's
`flight/`) on host A's ports instead of the air, with `--from TIME` and
`--speed X` (docs/FLIGHT_RECORDER.md). The bots seal what they broadcast, answer repair requests
and take broadcast files without asking; `--loss PCT` has hosts A and B
lose that share of the group frames they hear, so repairs happen, and
`--files` has the first bot broadcast a bulletin file a minute in and
every 6 minutes. A call
from host A or B is answered on its second try. The hosts report SNRs,
each bot at its own strength; `--no-snr` plays a host from before Data2G
PR #98. `--rig PORT` plays a rigctld for the dial (14.105 MHz, or
`--dial HZ`; `--dial-hop` moves it outside the data segments one minute
in three; `--rig-vfo` plays a rigctld started with `--vfo`; it answers
the meters too, its SWR 1.3 or `--rig-swr X`) and prints anything it is
sent but `\chk_vfo`, `f` and the meters. For Hamlib with no
radio, pick maker Hamlib, model Dummy in Settings > Rig: a pretend rig
at 145 MHz, which "Key the rig" keys at the fake hosts' PTT. Headless:
`Xvfb :91 &`, `DISPLAY=:91 ragtime ...`, `xdotool` to click and type,
`import -window root` for a screenshot. Never point the bench at Jeff's
radio PC's real host ports while his data2g-host runs.

**Windows.** `packaging/windows/make-zip.sh` cross-compiles with Ubuntu's
`g++-mingw-w64-x86-64-posix`, cloning wxWidgets and Hamlib into
`build-win` the first time (or set `RAGTIME_WX_SOURCE` and
`RAGTIME_HAMLIB_SOURCE` to checkouts; Hamlib needs `autoconf automake
libtool`), and
`ctest --test-dir build-win` runs the tests under Wine (`wine64`). The exe
links everything in, so it needs no DLLs beside it. Windows file names
ignore case: a test that saves `two.bin` beside `TWO.BIN` overwrites it.
The bench's fake hosts are POSIX, but `wine ragtime.exe` talks to a Linux
`ragtime_fake_air` on loopback.

## Releases

1. A PR sets the version, `project(ragtime VERSION ...)`, and gets a full
   CI run, started by hand on its branch (no `[skip ci]`).
2. After Jeff merges it, build from the merge commit:
   `packaging/linux/make-appimage.sh` and `packaging/windows/make-zip.sh`.
   Or take the AppImage and zip from a full CI run on that commit (the
   artifacts download from `*.blob.core.windows.net`); those jobs print
   each file's sha256sum in the log and the run summary, so check the
   download against it. Files are named
   `Ragtime-<version>-beta-<hash>-...` (`release-name.sh`).
3. Write `SHA256SUMS-<version>-beta` beside them (`sha256sum`), and
   smoke-test headless: the AppImage under Xvfb and `ragtime.exe` under
   Wine, each against `ragtime_fake_air`. Put the files in the project's
   `ragtime-release/` folder.
4. Draft release notes: say whether it talks to the last release (a
   protocol change breaks that), that data2g-host is the operator's to run
   (never bundled or started), and how the gearshift finds Data2G's
   `mode_thresholds.json`.
5. Jeff tags `v<version>-beta` on the merge commit and publishes.

## Code

- C++17, wxWidgets, CMake, the same toolchain as Glissando. The AppImage
  and the Windows zip follow Glissando's recipes (its `docs/APP.md`),
  simpler for having no audio or codec2. There is no Mac or ARM
  build yet.
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
PulseAudio null sinks) is Jeff's to run there. Data2G no longer needs
PyTorch: its releases are the native `data2g-host` and `data2g-gui`, and
`--audio-io pipe:IN,OUT` joins two hosts with fifos, no sound card
(`docs/DATA2G.md`).

## Cloud sandbox quirks

- Blocked hosts include `codeload.github.com` and GitHub archive and
  release downloads; `git clone` works. Point CMake `ExternalProject`
  URLs at local clones for a cloud-only build, and never commit that.
- Tag pushes are refused and a session can't create releases; Jeff tags
  and publishes in the GitHub UI.
- WebFetch of outside pages needs Jeff's OK.
