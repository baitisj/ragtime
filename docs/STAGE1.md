# Stage 1: parity with Glissando, and then some

Jeff said go on 2026-10-22. Stage 1 is build-order steps 1 to 7 in
`PLAN.md`: Ragtime does everything Glissando's Data2G mode does (the
"Functionally equivalent" checklist in `PLAN.md`), in three tabs, plus the
extras Jeff asked for during the mock-ups (the "and then some" below).
When it lands, Glissando can drop its Data2G mode.

Each work package below is sized for one builder and one or a few PRs.
The order shows what depends on what; packages on the same line can run
side by side.

| Package | Needs | Can run beside |
| --- | --- | --- |
| A. Repo skeleton and CI | nothing | |
| B. Protocol v0 and core library | A | C, D |
| C. Globus widget | A | B, D |
| D. Gearshift model and popup | A | B, C |
| E. App shell and Broadcast tab | B (interfaces), C, D | |
| F. Sessions | E | |
| G. Session files and the Files tab | F | H |
| H. Broadcast files with repair | E, B | G |
| I. Packaging and first release | E onward | |

## A. Repo skeleton and CI

Done in the skeleton PR: see "Layout" and "Build and test" in
`CLAUDE.md`. The Windows cross build moved to package I.

- CMake project: `core/` (socket-free library, no wx), `app/` (wx),
  `tests/`, `tools/`, `docs/`. C++17.
- GitHub Actions on Ubuntu 24.04: build core and app, run ctest. A second
  job builds core in the `gcc:16` image with `-Werror`.
- wxWidgets as Glissando builds it (system packages on Linux, the MinGW
  cross build for Windows).

## B. Protocol v0 and the core library

- Write `docs/PROTOCOL.md` v0 before the code: the records from PLAN.md's
  "On the air" (UTF-8 text records with callsign, sequence number and
  time; a hello record with protocol version and the records each end
  takes; file offer, answer, data, saved, cancel; the seal digest list
  that broadcast repair uses). Every record carries the callsign; no
  all-zero frames.
- Port from Glissando, with their tests, adapted to the new records:
  `Data2GLink` (KISS, command lines, `MODES`, the session stream),
  `Sha256`, `HamText` (the ham-table Huffman coder, its table generator
  and phrases), `Data2GFileTransfer`. Read CLAUDE.md's licensing note
  before copying a file.
- A fake `data2g-host` for tests: KISS, command and session ports on
  localhost, scripted replies (`BCAST PORT`, `MODES`, `PTT`, `BUSY`,
  `CONNECTED`, `DISCONNECTED`, `BUFFER`, ACKMODE tags).

## C. Globus widget

- Port `Globe` (wx-free projection and Natural Earth tables) from
  Glissando, then draw it in the new theme: light blue oceans `#A8D4F2`,
  near-white land `#F7F5EE`, dark blue coasts and borders `#143068`, US
  state and Canadian province lines thin and light
  (`tools/globe/us_canada_admin1_lines.json`, Natural Earth, simplified to
  0.25°).
- Lit from the subsolar point: Lambert shading on the day side, a
  twilight blend from 3° above the horizon to 12° below, so the greyline
  shows. `tools/globe/render_sunlit_globe.py` is the reference renderer;
  `docs/images/globe-w1aw.png` is the look.
- The picked station's path as a great circle, home and station dots, and
  a caption `W1AW · FN31 · 3,951 km · 81°`. Keep Glissando's spin, drag
  and zoom if they port cleanly.
- The pane's header is the **Globus** toggle (blue when on) and
  `22:36 UTC / 15:36 PDT` (UTC and local time of the lighting).

## D. Gearshift model and popup

- `docs/GEARBOX.md` is the spec. Model in core (thresholds table, best
  modes, gear ladders, Auto's pick from a path S/N with a 3 dB margin);
  the popup in app (the Submode Map as a chart, Width and Path chips, gear
  list, Auto).
- Until the host reports thresholds, the table is built in, from the
  Submode Map figures (`/mnt/project-files/d2g-client/submode_thresholds.json`
  in the Claude project; ask Jeff before committing it, since it is
  Data2G's data).
- Width caps today are only `BW500` and `BW2300`; 200 Hz and 1.2 kHz are
  greyed with "needs data2g-host support" until Andrew adds them.

## E. App shell and the Broadcast tab

- One main window: connection bar, station list on the left, tabs in the
  middle, Globus on the right, status bar. Both dividers drag and are
  remembered; Globus off gives the messages the full width.
- Connection bar: host address and state (Connected, Reconnecting in
  8 s), my callsign, group `D2G`, the width button and the one-press
  500 Hz button, Accept sessions, Pause sending, TX light from `PTT`, and
  an **Auto ALC** checkbox with a level meter (disabled until the host
  takes a TX level command).
- Station list: columns Call, Status, Heard, Locator, Distance, Bearing,
  Key, Services, chosen from a Columns menu and sortable. With Globus on,
  the narrow list shows locator and distance under the last-heard time.
- Broadcast tab: messages with Queued, Sending, Sent, Not sent (ACKMODE,
  two minutes to give up), the Gear button by Send, "@W1AW" from the
  station list, Pause sending.
- Seal badges: the icons are in `docs/images/seals/`. Signing comes in
  Stage 2, so hide the badge until then rather than marking every message
  Unsigned, but leave room for it in the message row and the Key column.

## F. Sessions

- Call (with Calling, try 2 of 5, Cancel), incoming sessions (caller on
  the station list at once), Accept sessions, the Session tab popping open
  and greying out, its status strip, Link quiet with a timer, Leave
  (`DISCONNECT`) and Leave now (`ABORT`). No auto-close while you are in a
  session.
- Messages in a session settle by the host's `BUFFER` count, as in
  Glissando. Broadcast messages held by a session show that they are
  waiting.

## G. Session files and the Files tab

- Offer cards in the Session tab (Save, Save as..., Decline, expiry),
  drag and drop, a send queue you can reorder, auto-accept list,
  whole-file SHA-256, `.part` files kept 7 days across restarts and
  resumed.
- Files tab: two panes, newest first. "In progress and parts" (date,
  file, size, progress, From or To, state, expiry) and "Completed" (date,
  file, size, From or To, result, saved path). States: Waiting for answer,
  Moving, Stalled with a timer, Repairing, Part kept. Rate over the last
  2 minutes, one finish estimate.
- Right-click menus as in PLAN.md's Stage 1 section.

## H. Broadcast files with repair

- Broadcast a file with the estimate box and the 300 KB cap; receive
  automatically or on request.
- Seal-driven repair (PLAN.md "Seals, receipts and repairs"): the sender
  lists digests, listeners ask for missing pieces with a random backoff
  and suppression, the sender resends the union. Signing the seals waits
  for Stage 2, but the digests are there from the start.
- Right-click on a share: the first, greyed line shows when repairs end
  ("Repairs open until 15:42 (1 h 10 min)"), then Keep open 30 min
  longer, 2 hours longer, until..., and Close repairs now.

## I. Packaging and the first release

- Linux AppImage and portable Windows zip (strip with
  `x86_64-w64-mingw32-strip`), as Glissando. `SHA256SUMS-<version>`.
- The MinGW-w64 Windows cross build, with wxWidgets built from source
  (Glissando's `docs/APP.md` recipe), and a CI job for it.
  Smoke-test headless (Xvfb, Wine). Jeff tags and publishes.
- Before calling Stage 1 done, Jeff runs the two-host bench on Hamlet:
  two real `data2g-host`s and a Ragtime on each.

## Waiting on Andrew (via Jeff)

- 200 Hz and 1.2 kHz width caps; a TX level command for Auto ALC; SNR
  reports for broadcast bursts; thresholds in `MODES`; whether the polar
  modes carry data and can be picked with `BCAST MODE`.
- The Hand primitive (`HAND SEND`, `HAND LISTEN`), which he will build;
  it belongs to the D2G net (Stage 2), not Stage 1.
