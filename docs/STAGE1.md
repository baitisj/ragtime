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

Done in the package B PR: `docs/PROTOCOL.md` is the spec, and
CLAUDE.md's Status lists what is where. For the packages after it:

- **E** opens the host with `link::HostLink` (KISS and command ports),
  builds frames with `protocol::encodeFrame` and numbers broadcasts with
  a `protocol::Numbering` saved across restarts.
- **F** reads the session data port with `protocol::StreamReader`
  (opened by `protocol::streamOpening` and a Hello), with a fresh
  Numbering each session. Reset the reader when a session ends, not when
  `CONNECTED` arrives: the far end's Hello can come first.
- **G** drives `files::SessionFiles` from the session (records, pieces,
  `BUFFER` acknowledgements); keeping part files across restarts is
  still to do.
- **H** has the Seal, RepairRequest, FileAnnounce and FilePiece records
  and `protocol::recordDigest`; the behaviour is its own.

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

Built in the Globus PR: `core/include/ragtime/globus/` (`Globe.h`, the
projection, framing, rolling and map lines ported from Glissando; `Sun.h`,
the subsolar point, the lighting and the header's time), the generated
tables in `core/src/globus/`, the pane in `app/src/globus/`, and
`tests/GlobusTest.cpp`. Left for package E: wiring the pane to the station
list and the operator's locator, the draggable divider, moving the toggle
to the tab strip while the globe is hidden, and View > Globus. Until then
hiding the globe leaves the pane's header with the unlit toggle.

- Port `Globe` (wx-free projection and Natural Earth tables) from
  Glissando, then draw it in the new theme: light blue oceans `#A8D4F2`,
  near-white land `#F7F5EE`, dark blue coasts and borders `#143068`,
  state and province lines thin and light (Natural Earth 1:10m,
  simplified to 0.25°, by `tools/globe/gen_admin1_lines.py`) for the
  United States, Canada, Mexico, Russia, China, India, Brazil, Argentina
  and Australia (Jeff, 2026-10-22).
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

Done in the gearshift PR, except wiring the popup to the connection bar's
width button and the Gear button by Send, and feeding it `MODES` and
burst S/N, which come with package E.

- `docs/GEARBOX.md` is the spec. Model in core (thresholds table, best
  modes, gear ladders, Auto's pick from a path S/N with a 3 dB margin);
  the popup in app (the Submode Map as a chart, Width and Path chips, gear
  list, Auto).
- Nothing of Data2G's is built in (committing the Submode Map figures is
  still Jeff's call). Ragtime reads the operator's own data2g-host
  `mode_thresholds.json` or the Submode Map's JSON, with widths and
  speeds from `MODES`: `GEARBOX.md`, "Where Ragtime gets the figures".
- Width caps today are only `BW500` and `BW2300`; 200 Hz and 1.2 kHz are
  greyed with "needs data2g-host support" until Andrew adds them.

## E. App shell and the Broadcast tab

Done in the package E PR: `core/src/broadcast/` (`Stations`, the station
list; `BroadcastChat`, the Broadcast tab's engine), `link::Reconnect`, the
window in `app/src/`, and the bench `ragtime_fake_air` (CLAUDE.md, "The
bench"). Choices made there, for Jeff to keep or change:

- **"@W1AW" messages** (PLAN.md question 6, kept as recommended): a
  line starting `@W1AW` is a Text addressed to W1AW asking for an Ack.
  W1AW's Ragtime answers on the group by itself, and the message shows
  "Acknowledged by W1AW", or "Sent, no ack yet". No automatic retries:
  **Re-send** is on the right-click menu of each of your messages, as in
  Glissando, once it has settled (Sent, Not sent, Cancelled or
  Acknowledged). On the Broadcast tab it goes as the same record, so a
  station that heard it already doesn't list it twice, and W1AW
  acknowledges it again. On the Session tab it goes as a new message
  (the session's stream already resends what the link loses), and only
  while a session is up; with none, the item is greyed and says so.
- **Check-ins.** A Hello rides along in the first frame of the first
  message after the group opens, after the callsign, locator or Accept
  sessions changes, and every 10 minutes after that; Station > Check in
  sends one alone. Ragtime never transmits on its own except to answer an
  `@` message with an Ack.
- **Long messages** split between words into several Texts, one frame
  each, all in one burst (`PROTOCOL.md`, "Broadcast").
- **Pause sending** holds new bursts as Queued and survives a restart; a
  burst the host already has still goes.
- The **Broadcast tab label** counts messages heard while it is out of
  sight, "Broadcast (3)".
- **Auto ALC** and its meter are there, greyed, with a tooltip saying why.
  **SNR** is in the Columns menu, greyed, "needs a host report".
- The **status bar**: channel clear or busy, the mode the host last sent
  in, and a line that will show the newest transfer (clicking it opens
  Files); until then it shows the host's refusals and undecoded bursts.

Left for F: Call and Leave on the Station menu and the station's
right-click menu, the session partner pinned at the top of the list and
"In session with you", Status QRL and QRV broadcasts on entering and
leaving, the Session tab, and the session data port in `HostConnection`.

The brief:

- One main window: connection bar, station list on the left, tabs in the
  middle, Globus on the right, status bar. Both dividers drag and are
  remembered; Globus off gives the messages the full width.
- Connection bar: host address and state (Connected, Reconnecting in
  8 s), my callsign, group `D2G`, the gearshift button and the 500 Hz
  switch, Accept sessions, Pause sending, TX light from `PTT`, and
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

Done in the package F PR: `core/src/session/SessionChat.cpp` (the Session
tab's engine, and `StreamAccount`, the `BUFFER` bookkeeping), Status QRL
and QRV in `BroadcastChat::announce`, the session partner in `Stations`,
the session data port in `HostConnection`, the tab in
`app/src/SessionPage`, and in the bench, calls that take tries, session
bytes that take airtime and can fade, and bots that ragchew
(`ragtime_fake_air --call AG7EW --fade`). Choices made there, for Jeff to
keep or change:

- **Try 2 of 5** is counted from the host's `PTT ON` reports while
  calling, since data2g-host doesn't report its calls. A host that hasn't
  settled a call in 3 minutes is stopped with `ABORT`, as Glissando does.
- **Link quiet** shows only while our bytes wait for acknowledgement and
  nothing has come back for 30 s. A session where nobody types just shows
  how long since the far end was last heard, without the amber.
- **A farewell.** Leave writes a Status QRV into the session before
  `DISCONNECT`, so the far end can say "Session ended by AG7EW" rather
  than "Link lost" (`PROTOCOL.md`, "Sessions").
- **The buttons follow the moment:** Cancel while calling, Leave in a
  session, Leave now once leaving (as PLAN.md describes it), and Call
  again once it has ended. All are on the Station menu too (Ctrl+K,
  Ctrl+L, Ctrl+Shift+L).
- **Calling a busy station** (its QRL heard, or its check-in says no
  sessions) asks first: Call anyway, Message instead, or Cancel. "Tell me
  when free" is not built yet.
- **Calling while in a session** asks "Leave it and call KC2G?", then
  calls once the first session has ended.
- **Pause sending** holds session messages as Queued too.
- The Session tab keeps the last session's messages, greyed, until the
  next call or incoming session. There is no idle limit (PLAN.md question
  7 leaves it off by default; it isn't built).

Left for G: files plug into `session::SessionTraffic` (records in and
out, acknowledgements, the far end's Hello, the session's end, and
`busy()`, which makes Leave ask first). `SessionFiles` already has the
matching calls; `tests/SessionTest.cpp` shows a stand-in.

The brief:

- Call (with Calling, try 2 of 5, Cancel), incoming sessions (caller on
  the station list at once), Accept sessions, the Session tab popping open
  and greying out, its status strip, Link quiet with a timer, Leave
  (`DISCONNECT`) and Leave now (`ABORT`). No auto-close while you are in a
  session.
- Messages in a session settle by the host's `BUFFER` count, as in
  Glissando. Broadcast messages held by a session show that they are
  waiting.

## G. Session files and the Files tab

Done in the package G PR: `files::FileTraffic` (core/src/files/), which
plugs `SessionFiles` into the session as its `SessionTraffic`;
`files/FileRows.h`, the Files tab's rows and its lasting log, shared with
package H; the Files tab (`app/src/FilesPage`); offer cards and the send
queue in the Session tab; the received folder and auto-accept list in
Settings > Station; and in the bench, bots that save what you send them
and offer their bulletin (`ragtime_fake_air --call AG7EW`, then type
"send the file again"; `--rate` sets how fast session bytes cross).
Choices made there, for Jeff to keep or change:

- **Pacing.** No more file bytes wait at data2g-host than about a minute
  of the session's acknowledged rate (1 to 16 KB, 4 KB until the rate is
  known), and a FileData carries at most half of that. A message typed
  mid-file goes out behind at most that much, not behind the whole file.
- **Offers wait for the far end's Hello,** and go only if it takes
  session files; a file queued for a station that doesn't ends Not taken.
- **Leave** cancels what is under way, so the far end hears why and keeps
  its part; files still queued stay queued for the next session with that
  station, shown as "Waiting for a session with W1AW" in Files (Remove
  from the queue is on the row's menu).
- **Save** puts the file straight into the received folder (default
  "Ragtime received" in the documents folder) under the name offered,
  numbered if it's taken; **Save as...** asks. With a part kept from
  before, both carry on in it.
- **Send file outside a session** asks "Call W1AW and send them once
  you're connected?", then queues the files and calls. Files dropped on
  the Session tab go only while a session is up.
- **Ask to send the rest** (a kept part) and **Ask W1AW to send it
  again** (a failed file in Completed) type "Please send FRED.TXT again:
  I have 4,096 of 7,000 bytes kept, so only the rest needs to come." into
  the session; with none up, they offer to call and send it once
  connected. **Send it again** is on a file of ours that didn't arrive.
- **Stalled** shows once nothing has moved for 30 s, with its timer. Rate
  and the finish estimate show in the Progress and State columns.
- **The log.** Completed and the kept parts live in a plain text file
  beside the `--config` file (`a.conf.files`), or `files.log` in the user
  data folder: the last 500 finished transfers, and up to 8 parts kept 7
  days each. Lines it doesn't know (package H's, say) are kept as they
  are.
- **Closing Ragtime** mid-transfer fails it and keeps the part.
- **An offer in the background** raises a desktop notification and
  counts on the Session tab's badge.

The brief:

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

Done in the package H PR: `broadcast::Airtime` (what bursts cost in
Data2G mode airtimes, packing records into frames and bursts, the file
estimate), `broadcast::Seals` (sealing what we broadcast, the listening
window and resend rounds; on the listening side, asking with backoff and
suppression), `broadcast::BroadcastFiles` (sending and receiving files on
D2G; its rows and finished files feed the Files tab through
`files/FileRows.h`), all in core/src/broadcast/ with
`tests/BroadcastFilesTest.cpp`; in the window, File... on the Broadcast
tab, file cards among the messages, File > Broadcast a file... and
Receive broadcast files automatically, and the share's menu on its card
and its Files row; the seal chain kept per callsign across restarts. The
bench's hosts can lose frames (`ragtime_fake_air --loss 20`) and its
first bot can share a bulletin a minute in and every 6 minutes
(`--files`). PROTOCOL.md "Seals and repair" and "Broadcast files" say
what goes on the air. Choices made there, for Jeff to keep or change:

- **When a seal goes.** 60 s after our last item when nothing more is on
  the way, at once when a file's first pass ends, and at most 10 minutes
  after the oldest unsealed item. Files are always sealed. Chat is not,
  unless the operator ticks "Seal my broadcast messages" in Settings (off
  by default, Jeff's pick on 2026-10-22): at Data2G's lowest rungs a seal
  is 30 to 60 s of air of its own, and plain chat must not wait on one.
  Chat between files leaves gaps in the numbers a seal lists; the seals
  either side of a gap are made together and share a keying.
- **Zero digests** for items whose burst was Not sent, so nobody asks for
  them.
- **Seals go again.** Each resend round ends with its seals, which opens
  the next window; and a seal over a file goes again after a window with
  no requests, at most twice more. On the bench at 20% frame loss a lost
  seal or a lost request otherwise left a listener stuck for good.
  Text-only seals aren't repeated, to save airtime.
- **Who asks.** A listener asks only about overs it was there for: it
  holds an item or a piece under the seal, or an older seal or item of
  that station. A station that just tuned in doesn't ask for the past;
  pressing Receive on a file it heard does, at once (within 10 s).
- **Backoff in our own mode.** A listener spreads its request over the
  window less its own request's airtime and 2 s, and gives up under a
  seal after 5 requests that brought nothing.
- **The sensitivity rule** as written in PROTOCOL.md: newest 3 seals, or
  4 × the items' airtime, never past 2 hours; Keep open and Close repairs
  now override it. Close repairs closes the whole seal (the other items
  under it too).
- **Refuse over 2 hours of airtime, warn over 15 minutes,** naming the
  faster gears the host offers. Stations will not confirm receipt.
- **Pacing.** 3 s between a file's bursts, the announce again every 20
  pieces, a Not sent burst tried once more.
- **Receive automatically** is off by default (File menu), into the
  received folder. Ignore is logged as Declined.
- **Incomplete.** A file still missing pieces 2 h 10 min after anything
  new came is shown incomplete. Broadcast parts are kept in memory only
  (24 hours on screen), not in the files log across restarts.

The brief:

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

Done in the package I PR (see "Releases" in `CLAUDE.md`):

- An app icon: the leaf from the cover of Scott Joplin's Maple Leaf Rag
  (1899), tipped as it is there, going digital block by block, on the
  cover's cream with its red rim (Jeff's pick, 2026-10-22).
  `tools/icon/make_leaf_svg.py` draws `app/res/ragtime.svg`. It is the
  window icon on both systems, the exe's icon and the AppImage's.
- `packaging/linux/make-appimage.sh`: linuxdeploy with its GTK plugin,
  as Glissando, so the AppImage brings its own GTK theme.
- `packaging/windows/make-zip.sh`: wxWidgets 3.2.9 (the series Ubuntu
  24.04 ships, so the code sees the same API on both) cloned and built
  statically, and the MinGW runtime linked in, so the zip holds just
  `ragtime.exe` (about 12 MB), `README.txt` and `LICENSE.txt`. The exe
  carries wx's manifest: modern controls, per-monitor DPI awareness.
- CI jobs for both; the Windows job runs the core unit tests under Wine.
  That caught one test saving `two.bin` over its own `TWO.BIN`.
- Smoke-tested headless: the AppImage under Xvfb, and `ragtime.exe` under
  Wine chatting with `ragtime_fake_air` over loopback.

## Waiting on Andrew (via Jeff)

- 200 Hz and 1.2 kHz width caps; a TX level command for Auto ALC; SNR
  reports for broadcast bursts; thresholds in `MODES`; whether the polar
  modes carry data and can be picked with `BCAST MODE`.
- The Hand primitive (`HAND SEND`, `HAND LISTEN`), which he will build;
  it belongs to the D2G net (Stage 2), not Stage 1.
