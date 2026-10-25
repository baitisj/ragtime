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
  W1AW's Ragtime answers on the group by itself, and the message's badge
  turns from a grey check (its tooltip "Sent; no ack from W1AW yet") to
  a green double check ("Acknowledged by W1AW"; Jeff, 2026-10-24,
  MOCKUPS.md "Message state badges"). No automatic retries:
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
  **SNR** is a column (below, "SNR from data2g-host").
- The **status bar**: channel clear or busy, or what of ours is going
  out (Jeff, 2026-10-23: "indicate if we're sending a seal ... just a
  little snippet"): "Sending: seal", "Sending: “CQ CQ de AG7EW…”",
  "Sending: map.png, 5–8 of 30", "Busy, next: ack", "Held: QRV" while a
  session holds the group, "Paused: …", with "(+2)" for bursts waiting
  behind it. Each burst is labelled where it is made
  (`HostLink::broadcast`'s label; `link::channelLine`), and the tooltip
  lists them all, with the session's bytes not yet acknowledged. Then the
  mode the host last sent
  in, and a line that shows the newest transfer, or the host's refusals
  and undecoded bursts. Clicking it no longer opens Files (Jeff,
  2026-10-25; View > Files, Ctrl+3, does). While a seal of ours waits
  (our files, and our messages with "Seal my broadcast messages" on) the
  line ends in a countdown, "Next seal in 0:42", then "Seal due" while a
  repair round or Pause sending holds it (`Seals::nextSealMs`).
  At its right end, data2g-host's address and state (Connected,
  Reconnecting in 8 s), with the last error as its tooltip. It sat at the
  connection bar's left end until Jeff moved it (2026-10-23).
- **Light or dark (Jeff, 2026-10-23).** A button at the status bar's right
  end, beside the size grip, switches: a moon in the light, a sun in the
  dark. The choice is kept (`/Window/Theme`); until the first click the
  desktop's theme stands. On GTK it is the theme's own dark variant,
  falling back to Adwaita's when the theme has none; windows that keep a
  colour from when they were made take it again (`followThemeBackground`,
  `followThemeText` in `Theme.h`). wxWidgets 3.2 has no dark mode on
  Windows, so the button isn't there. The colours that mean something
  (delivered and sent green, not delivered red, the Link quiet strip's
  amber, links, the station dots, the gearshift's too-hard gears) come in
  a light and a dark shade (`goodText`, `badText`, `quietText`,
  `accentText`, `quietSurface`, `htmlLink` in `Html.h`), so they read on
  both (Jeff found the Link quiet strip and Delivered hard to see in the
  dark, 2026-10-23).
- **The chat lists' scrollbar sits beside the rows (Jeff, 2026-10-23).**
  GTK's overlay scrollbar faded in over the rows' right ends whenever a
  message came, covering "Sent" and "Delivered"; the Broadcast and
  Session lists and the narrow station list turn overlay scrolling off
  (`scrollbarBeside` in `Theme.h`).
- **The Broadcast tab carries on (Jeff, 2026-10-23).** What is said on
  D2G is kept on disk (`broadcast::BroadcastLog`, one file,
  `<data folder>/broadcast.log`, or `<config>.broadcast.log` beside a
  `--config` file), so Ragtime opens with the earlier lines, greyed, a
  rule where it last closed ("Ragtime closed 01:15"), and carries on
  below. Broadcast files show as one line each with how they ended.
  Earlier messages of ours can be re-sent, as the same item, while the
  callsign is the same; a line heard again isn't listed twice. Ours that
  were still going when Ragtime closed come back Not sent. The file is
  cut to its later half when it passes 1 MB, and the tab shows the newest
  2000 rows.

Left for F: Call and Leave on the Station menu and the station's
right-click menu, the session partner pinned at the top of the list and
"In session with you", Status QRL and QRV broadcasts on entering and
leaving, the Session tab, and the session data port in `HostConnection`.

The brief:

- One main window: connection bar, station list on the left, tabs in the
  middle, Globus on the right, status bar. Both dividers drag and are
  remembered; Globus off gives the messages the full width.
- Connection bar: my callsign, group `D2G`, the 500 Hz
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
  Stage 2, so no identity badge until then, rather than marking every
  message Unsigned; room is left for it in the Key column. The message
  row's badge shows the seal's own state (Awaiting, Sealed, Repairing,
  Held, Mismatch; `docs/MOCKUPS.md`, Jeff's ask of 2026-10-23), from
  `Seals::ourMark` and `Seals::theirMark`.

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
- **Sessions carry on (Jeff, 2026-10-22).** What is said in a session is
  kept on disk, a file per station (`session::SessionLog`, in
  `<data folder>/sessions/`, or `<config>.sessions/` beside a `--config`
  file). Calling a station, or a session from it, brings back the earlier
  sessions with it, greyed, each closed by a rule ("Session with W1AW
  ended 14:32 · 22 min"), and the new session carries on below. Session
  files that finished show there as one line each. A station's file is
  cut to its later half when it passes 1 MB, and the tab shows the newest
  2000 rows.

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

- **Pacing.** No more file bytes wait at data2g-host than two of the
  session mode's longest bursts, between 1 and 8 KB, and a FileData
  carries at most half of that. A burst in a session runs at most 24 s
  on FSK and 12 s on OFDM (Andrew), so the longest is as many of the
  mode's codewords as fit in that time, from the airtimes in `MODES`:
  about 4.5 KB for `w48-16qam-r1/2` (so the 8 KB cap), 684 B for
  `qpsk-r1/3`, 847 B for `n10-16qam-r1/2`, and 266 B for
  `fsk32r62-r1/3` (so the 1 KB floor). The mode is the one data2g-host
  last reported (`MODE`) in the session; until it has, the window is
  10 s of the session's acknowledged rate (1 KB until the rate is
  known). Since 2026-10-25 (Jeff's pick, after Andrew found the fast
  modes starved: a window of about 1 KB is less than one of their
  bursts, so the host idled into CSMA between them, and a miss shrank
  it further). Before that it was the acknowledged rate alone: a minute
  of it until 2026-10-24, then 10 s, between 1 and 16 KB. Once
  data2g-host can drop what it holds (`CLEANTXBUFFER`, Data2G #118),
  the plan is a flat 8 KB, cut back on a cancel or to slip a message
  in (Andrew). Until then a message typed mid-file goes out behind at
  most the window, not behind the whole file.
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
- **A kept part isn't Completed** (Jeff, 2026-10-23). The file stays in
  In progress and parts as Part kept, its ending held with the part (a
  `K` line's last two fields). It goes into Completed as it ended when
  the part is deleted, runs out of time or is pushed out by newer ones,
  or as Saved when carried on. A log from before this moves a matching
  Completed entry back onto its part when it loads. The state says how
  it ended: "Part kept, cancelled by AG7EW".
- **A cancelled send stops here and stays in Parts** (Jeff, 2026-10-24).
  data2g-host sends what it has been handed and can't take it back, so
  Cancel hands it nothing more, and the row reads "Stopping, 1,135 bytes
  still going out" until those are acknowledged, then "Part sent,
  cancelled", with the far end's share of the file in its bar. Any send
  that ends unfinished with some of it gone (cancelled either end, or
  the session lost) is a part sent like this, not Completed: its menu
  has **Send the rest to W1AW** (the file offered again; the far end
  carries on from its part), **Keep 7 days longer** and **Forget it**,
  which puts it into Completed as it ended, as running out of time
  does. Its log line is an `S` line, a `K` line's fields with the file
  sent for the path and no part file; older Ragtimes keep it as it is.
  A FileSaved that crosses the cancel ends it as Sent.
- **A receiver keeps what came of a piece still arriving** (2026-10-24),
  so the bar never goes back when it cancels: 3,000 of 3,741 shown is
  3,000 kept, where it used to keep only the 1,244 in whole pieces.
- **Closing Ragtime** mid-transfer fails it and keeps the part.
- **An offer in the background** raises a desktop notification and
  counts on the Session tab's badge.

The brief:

- Offer cards in the Session tab (Save, Save as..., Decline, expiry),
  drag and drop, a send queue you can reorder, auto-accept list,
  whole-file SHA-256, `.part` files kept 7 days across restarts and
  resumed.
- Files tab: two panes, newest first. "In progress and parts" (date,
  file, size, progress, de (From or To), state, expiry) and "Completed" (date,
  file, size, bps, de (From or To), result, saved path; bps is bits a second
  over the whole transfer, first byte to last: a broadcast send's first
  pass, a broadcast receive from first heard to saved). States: Waiting for answer,
  QRL (moving), Stalled with a timer, Repairing, Part kept. Rate over the
  last 2 minutes, one finish estimate: a moving session file reads
  "QRL ETA 14:32", a broadcast one "Receiving, ETA 14:32".
- Right-click menus as in PLAN.md's Stage 1 section.

## H. Broadcast files with repair

Done in the package H PR: `broadcast::Airtime` (what bursts cost in
Data2G mode airtimes, packing records into frames and bursts, the file
estimate), `broadcast::Seals` (sealing what we broadcast, the listening
window and resend rounds; on the listening side, asking with backoff and
suppression), `broadcast::BroadcastFiles` (sending and receiving files on
D2G; its rows and finished files feed the Files tab through
`files/FileRows.h`), all in core/src/broadcast/ with
`tests/BroadcastFilesTest.cpp`; in the window, the file button (a floppy disk since 2026-10-23) on the Broadcast
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
  received folder.
- **Stop and Reject** (Jeff's test with Andrew, 2026-10-22). Stopping our
  share ends its first pass but leaves repairs open, so a fresh request
  on the group for the rest is answered there without asking ("fresh" is
  the seal's sensitivity window). On theirs, Stop goes back to
  Incomplete with what came kept, and Ask again carries on; Reject
  file... is only on the right-click menu, asks first, and is logged as
  Declined.
- **Asking again** (Jeff's test, 2026-10-23). A listener that gave up
  asking sat at "Repairing, 9 pieces missing" for good: Ask again
  couldn't undo the give-up, and the file never went Incomplete, since
  that waited for a seal already forgotten. Now a give-up shows the file
  as Incomplete, Ask again (also on a file still being received) starts
  asking afresh under its seal, and once the seal is over 2 hours old
  the item is greyed with the reason.
- **Incomplete.** A file still missing pieces after a give-up, once its
  seal is over 2 hours old, or 2 h 10 min after anything new came, is
  shown incomplete, and kept 7 days.
- **Parts on disk** (Jeff, 2026-10-23: "Is the state of the in-progress
  and parts list saved to disk? this should be kept there, too."). A
  broadcast file not yet whole is kept in `broadcast-parts` beside the
  session logs (`<config>.broadcast-parts` with `--config`), one text
  file each (`BroadcastParts`): the announce, state, save path, the
  pieces, and the seals heard over it. A restart reads them back onto
  the Files tab (not as chat cards: the chat's old cards are greyed
  history), and a seal still fresh is asked under. Heard ones go after
  24 hours, incomplete ones after 7 days. Our own shares in Repairing
  aren't kept across a restart yet: their repairs end when Ragtime
  closes.
- **Repair gears and Repair refused** (Jeff's idea, 2026-10-24, his
  choices on the proposal doc). A request carries the S/N its asker
  heard the sender at (the lowest in 15 minutes) in the spare bits of
  its flags byte, so it costs nothing on the air and 0.1 still works with
  it. Each item goes again in the fastest gear every station that asked
  for it can copy, with Auto's 3 dB margin, never faster than it first
  went, even below a gear the operator chose by hand. Gears slower than
  the round's own take at most 5 minutes a round, set in Settings ("Slow
  repairs", 1 to 60). A heard request keeps a listener quiet only if its
  asker's gear is no faster than the listener's. The sender refuses, in
  a RepairRefused, what it won't serve (closed, cancelled, no longer held,
  out of its window), and the listener's file shows "Not repaired:
  AG7EW cancelled it", with Ask again greyed. PROTOCOL.md "Seals and
  repair" has the rules; the airtime figures were in the proposal: six
  lines and their seal are about 6 s in `qpsk-r1/3` and 94 s in
  `fsk16r25-r1/3`.
- **Repaired lines in their place** (Jeff, 2026-10-24: repaired
  messages "did not get placed in the correct order in the chat"). A
  line heard after a later one of its sender's goes above that one, by
  the record time among the other stations' lines there, never above
  the rule; the Broadcast log's M line names the row it went above, so
  the order survives a restart.

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

## SNR from data2g-host

Data2G PR #98 ends `BCAST HEARD` and `BCAST * MISSED` with an SNR and
adds VARA's `SN` for the session partner (`docs/PROTOCOL.md`).

- `link::parseCommandLine` takes the SNR as the last word when it has no
  letter in it; HostLink's `HeardFrom` and `Missed` events carry it.
- In a session HostLink asks `SN` when the partner's burst has likely
  come (BUFFER going down, session bytes in), at most every 5 s, and
  reports a new figure as `SessionSnr`. A WRONG to SN is never shown as a
  refusal.
- `Stations` keeps each station's last SNR (the SNR column, sortable,
  blank from an older host) and the mean of its last 5 bursts.
- **Auto** shifts for the weakest station heard in the last 15 minutes
  (`Stations::weakestPath`, `PATH_WINDOW_MS`): a broadcast goes to every
  station, so it has to reach the weakest. The gearshift names the
  station. It is how well we hear them; they may hear us a little better
  or worse, since local noise differs, and the 3 dB margin covers some
  of that.
- A MISSED shows in the status bar: "Heard a E2C burst at +3.1 dB that
  didn't decode; it may not be D2G's".
- `ragtime_fake_air` reports SNRs: each bot at its own strength, +12 to
  −11 dB, give or take 2 dB a burst; `--no-snr` plays an older host.

## Waiting on Andrew (via Jeff)

- 200 Hz and 1.2 kHz width caps; a TX level command for Auto ALC;
  thresholds in `MODES`; whether the polar modes carry data and can be
  picked with `BCAST MODE`.
- SNR reports: answered by Data2G PR #98 (merged 2026-10-23), used as
  below.
- The Hand primitive (`HAND SEND`, `HAND LISTEN`), which he will build;
  it belongs to the D2G net (Stage 2), not Stage 1.
