# Working on Ragtime

Notes for a fresh Claude (or any newcomer) picking this project up. They
sum up what earlier work learned and what the owner, Jeff (AG7EW,
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

Read `README.md` first: it is what Ragtime does, for its users.
`docs/PROTOCOL.md` is what goes on the air (records, frames, sessions,
text coding, seals, roll calls, pictures and voice).
`docs/GEARBOX.md` covers the gearshift. `docs/DATA2G.md` is what data2g-host does at the edges Ragtime
touches. `docs/FLIGHT_RECORDER.md` is the host-link recorder, the report
and `--replay`. `docs/HOW_SEALS_WORK.md` is the plain-words primer on seals and
repair (its figures are SVGs in `docs/images/how-seals-work/`); change it
with the code when the seal rules change.
`docs/PICTURE_CODEC.md` is the spec for Cool-chic pictures, the second
picture kind beside JPEG 2000 (its reference code is `tools/codecs/rcc/`,
its test vectors `tests/data/coolchic/`). `docs/MACOS.md` is the Mac
port: building, testing, the release zip and what differs there.
`docs/ANDROID.md` is the Android port: its views, what it does and
does not do yet, how it is built, and demo mode;
`android/README.md` has its build and tests. `docs/ANDROID_LICENSES.md`
is what is in the APK and which notices it owes, with what Google Play
asks for;
`docs/ANDROID_PRIVACY.md` and `docs/ANDROID_RELEASE_NOTES.md` are copied
into the APK, so rebuild it after changing them.

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
  closed share can re-open (`BroadcastFiles::reopen`). Since 2026-11-15
  a file's repairs can have a gear per station (Repair gear for KC2G,
  with the station picked; `Seals::setStationRepairGear`). Since 2026-10-25 repairs are Reed-Solomon parity
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
  same protocol. 0.2.0-beta has everything since (parity repair, rig
  control, the Ear, reactions); it chats with 0.1 but can't repair with
  it. 0.3.0-beta was never tagged; 0.3.1-beta ships it with the
  fixes after it: Cool-chic pictures, voice messages, Hands and roll
  calls, and the thresholds from `MODES`. It chats with 0.2 (new
  records are skipped, pictures and voice arrive as plain files).
  0.4.0-beta adds the Mac zips, voice on Windows (`sotto.dll`), edits,
  repairs by Hand, repair gear per station, desktop notifications and
  the hot-frequency dial. It chats and repairs with 0.3.1 (SealRound is
  skipped there, and 0.3.1 asks for repairs by burst).
- The macOS port (2026-11-20) is Tim Vetter's (GitHub `finitor`), brought
  in with his commits as he wrote them: the Cocoa build (wxWidgets
  3.2.11 on the Mac, `-DRAGTIME_BUILD_WX=ON`), the app bundle
  (`packaging/macos/`), Core Audio voice and Ear (`platform/macos/`,
  macOS 14.2 or later), `/dev/cu.*` serial ports, sotto as dylibs in
  `Contents/Frameworks`, and the Mac tests (`tests/macos/`).
  `docs/MACOS.md` is the record. `packaging/macos/make-zip.sh` makes the
  release zip for the Mac it runs on (signed ad hoc, Hamlib and the
  picture libraries built statically), and the full CI run's `macos`
  jobs build, test and keep it for Apple Silicon and Intel. He is credited in the README's Contributors and the About box.
- The Android port (2026-11-21) is Tim Vetter's too: a phone app in
  Kotlin and Compose (`android/`) over the same core, with none of the
  wx app in it. `platform/android/` is its socket-free adapter
  (`android::AndroidClient`: `HostLink`, `BroadcastChat`, `SessionChat`,
  `Seals`, `BroadcastFiles`, `FileTraffic`, `Stations` and the
  gearshift, giving the screen one JSON snapshot and taking actions as
  strings), reached through JNI; the Kotlin side has the three TCP
  streams on one worker thread (`HostTransport`, `RadioModel`) and a
  foreground service that keeps the connection while the app is out of
  sight (`RadioService`). Four views share that one connection:
  Stations, Conversations, Field desk and Files (`docs/ANDROID.md`,
  "The four views"). It has Broadcast with `@` and
  acknowledgements, ragchews, session and broadcast files with seals
  and repair, inline pictures (JPEG 2000 sent, Cool-chic shown too; the
  picked picture is read by Android), the gearshift from the host's
  `MODES` thresholds alone (no thresholds file), Request roll call,
  and notifications of calls, `@` lines and file offers. Not yet: voice
  (clips arrive as files), reactions, edits, refusing a call, the dial
  and rig, the Ear, Globus and Recent work. Its Hello says what it
  takes: `takes` in `BroadcastChat`'s and `SessionChat`'s settings, the
  desktop's still `TAKES_ALL_V0`. Demo mode (a switch in Settings, for
  trying it, and for a store reviewer with no modem) runs
  `android::DemoAir` in the app: the tests' fake host and four made-up
  stations on loopback ports, which the app connects to as to a real
  host, so nothing on the screen is faked. The app never bundles or
  starts Data2G: the modem is a `data2g-host` on the network, or
  Data2G's own Android TNC, a separate app on the phone. It has its own
  `versionName` and `versionCode` (`android/app/build.gradle.kts`,
  0.2.0 build 3), not the project's version; it is debug-signed, built
  for ARM64 only, tested on one phone so far, in no CI job and not on
  Google Play.
- Data2G PR #118 (CLEANTXBUFFER, DATA2G ON) gives cuts, for 0.2.0:
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
- Data2G PR #122 (open 2026-10-26) gives DISCONNECTED reasons, TIMEOUT
  and TXPOWER. `HostLink` passes the reason on with `SessionEnded`,
  reads both values after DATA2G ON and sets TIMEOUT to a day
  (`RAGCHEW_IDLE_TIMEOUT_S`), and has `setTxPower` for the rig thread's
  TX level control; `SessionChat` turns the reasons into endings (Busy,
  NotListening, RefusedThere, IdleClosed, ClosedThere). The fake hosts
  give reasons and hang up quiet sessions (`--no-reasons` for an older
  one).
- Data2G PR #146 (the Hand, merged at e09e19d on 2026-11-11, after
  v0.4.3) gives the show of hands. Ragtime's link layer speaks it ahead of the merge
  (Jeff, 2026-11-10): `HostLink` sends KISS `REFS ON`, gives each heard
  frame its burst's Hand reference, and has `handSend`, `handListen`
  (the window named by its burst's first ACKMODE tag, the burst held
  until the host's OK) and `handCancel`, with the REF and HAND lines as
  events (docs/DATA2G.md, "Hands"). The fake hosts do references and
  Hands (`--no-hands` for an older one), and collide two in one channel.
  Request roll call (Ctrl+I, named Check in until 2026-11-15) calls the roll with them (Jeff, 2026-11-12; PROTOCOL.md "Roll
  calls"): the Hello goes with a RollCall
  record (`0x03`) and a window, and every Ragtime that hears it answers
  by Hand (`protocol/Hands.h`: callsign, square, S/N, flags in 63 bits;
  `BroadcastChat::answerRollCall`), unless in a ragchew or turned off in
  Settings; the row lists who answered. Everyone answers every round: no
  clock check and no `in` list since 2026-11-15, only one roll call of
  ours at a time. A window the host gives up on (30 s
  armed with the burst not gone) is armed again while the burst waits,
  else the row says no window (`HostLink`'s `HandLost`, `Roll::Lost`). Since 2026-11-16 a seal goes with a SealRound (`0x45`) and a Hand window, and a station missing something under it asks by Hand (PROTOCOL.md "Repairs by Hand", an early version); since 2026-11-26 (#185) the seal's listening window stays open until the Hand window ends and a listening window after it, a file's seal goes again once when nobody asks, not twice, and a station keeps its request bursts until a Hand window it heard of (a SealRound or a roll call) is over (`Seals::handsQuietUntilMs_`); since 2026-11-28 a file's seal (with its SealRound) rides in the burst that ends its first pass when it fits (`Seals::sendFileEnd`), not in a keying of its own straight after. Net control's traffic round is
  Stage 2, still to build.
- Data2G PR #186 (merged at d23a62b on 2026-11-14, in v0.5.1) puts
  mode_thresholds.json in the `MODES` answer (`CHANNELS`, then
  `THRESHOLD name dB...` lines). `HostLink` collects them into `ModeInfo::thresholds`, they win over the
  files' in the gearshift (`HostMode::hasThresholds`), and the popup then
  names data2g-host as the source and hides Load thresholds.
- The dial, the path log and the watch list (2026-10-25): `core/src/radio/` (`DialReader`, which only ever
  asks rigctld `\chk_vfo`, `f`, while transmitting the meters
  `+l ALC`, `+l SWR` and `+l RFPOWER_METER_WATTS`, and between bursts
  the S-meter `+l STRENGTH` for the band's noise (`radio::NoiseFloor`,
  the gearshift's QRN line), and while the Ear or the gearshift is
  open the filter
  (`+m`, `+l PBT_IN`, `+l PBT_OUT`, `+u MN`, `+l NOTCHF_RAW`, Jeff's
  OK 2026-10-26; `+l NOTCHF` from a rig with no NOTCHF_RAW, a Yaesu or
  a Kenwood, and `+l IF`, IF SHIFT, from one with no PBT, since
  2026-11-26; `+p DSP_IF_NOTCH_W`, the notch's width, from a rig with
  the manual notch, since 2026-11-28), naming
  `currVFO` for a `--vfo` one, and sets only the Ear's filter (`M`,
  `L PBT_IN`, `L PBT_OUT` or `L IF`, `L NOTCHF_RAW` or `L NOTCHF`,
  `P DSP_IF_NOTCH_W`, `U MN`, each only when it
  differs, with its box ticked) and a frequency the operator picks
  (`F`); `Bands`, the US data segments, and 60 m's channels (the dial exactly 1.5 kHz below a channel's centre, Jeff 2026-10-27) and band segment; `PathLog`, a line a
  burst heard, appended, and each station's 24-hour strip by band),
  `broadcast::WatchList` (bells, and Tell me when free), and in the app
  `RigConnection`, `PathStripView`, `PathsDialog` and `WatchDialog`.
  Outside the US data segments Ragtime only receives.
- Hot frequencies (2026-11-20, Eduardo N4EAC's idea, Jeff's ask): the
  dial beside the ear is a dropdown (`app/src/DialMenu`) of the
  operator's own list, `frequencies.txt` in `appDataDir()`
  (`hotFrequenciesPath()`; one MHz per line, `#` comments, seeded with
  Jeff's list, read again when it changes; `radio::HotFrequencies`).
  A pick tunes the rig (`RigControl::tune`: `F` through rigctld,
  `rig_set_freq` through Hamlib, held while transmitting, then read
  back); right-click gives Edit... (`$EDITOR` in a terminal, per
  platform: `radio::editorLaunches`) and Custom... (a typed frequency);
  Ctrl with the wheel steps 1 kHz, Shift 10 kHz.
- Search (2026-11-25, Jeff's ask): Ctrl+F or View > Search... opens a
  Search tab after Files, there only while open (`app/src/SearchPage`).
  `search::HistorySearch` (core) reads the Broadcast log and each
  station's session log with their history folders
  (`search::historySources`), file by file on the page's own thread,
  newest first: every term (a word, or "words in quotes") in the
  text, the sender or the `@` addressee, ASCII case folded; From and
  When narrow it. Hits list by place, Broadcast then each ragchew the
  latest first, with lines nearby for the pane below. A jump
  (`MainFrame::jumpToHit`) picks the line in the Broadcast tab, or in
  the Session tab's read-only review of that station (or the live
  session with it), with `FindStrip` over the lines: n of m, earlier
  and later, Back to search. With another station's ragchew up, the
  Session tab stays its own and the search says so.
- Recent work (2026-10-25): `radio::RecentWork` ranks
  the path log's stations, by default by (1 + days worked) × (1 + days
  heard at this hour on this band); contacts (a session, an `@` message
  either way) are `work` lines in the path log. The pane is
  `app/src/RecentWorkPane`, under the station list in the `left_`
  splitter. Since 2026-11-01 every path log line carries the station's
  locator and a new or moved one gets a `locator` line; a Recent work
  pick blinks yellow on Globus at its last square, and with no session
  up shows the earlier ragchews with it from `SessionLog` on the Session
  tab, read only, a page of its history at a time as it scrolls up
  (`SessionPage::review`, Jeff 2026-11-25: the tab
  "Session · W1AW (read only)", greyed on GTK as "Session · none" is,
  and the words in the typing line where a lost session puts its own);
  its tooltip gives the
  locator and distance, and Globus shows the locator under the pointer
  (`globus::locatorOf`, `globus::fromScreen`). Since 2026-11-02 the
  globe fills the pane and comes in on a picked path with room round it,
  north up when it can (`globus::globusZoom`), and a second click on a
  picked row unpicks it (`app/src/UnpickClick`).
- Rig control (2026-10-26, Andrew's suggestion):
  Settings > Rig picks no rig, the dial from rigctld (read only, but for
  the Ear's filter with its box ticked), or
  the rig opened with Hamlib (`radio::RigSettings`, `radio::hamlibConf`;
  `rig::HamlibRig` in `rig/`, on its own thread), with "Key the rig when
  data2g-host keys" (`radio::PttFollower`, off by default) following
  the host's PTT ON/OFF. `app/src/RigControl` holds whichever is
  picked; Settings is now two tabs, Station and Rig. While the host
  transmits, the rig's ALC, SWR and RF power are read, through Hamlib or
  rigctld (`radio::TxMeters`, burst by burst) for the Auto ALC meter and the
  flight recorder. With Data2G PR #122's `TXPOWER` (2026-10-26) the
  host's TX level is set in Settings > Station, live as the knob turns,
  and Auto ALC (`radio::AutoAlc`, off by default) moves it burst by
  burst to keep the ALC near 55% (Jeff, 2026-10-26), up to 6 dB over
  the level set there, and puts that back when unticked or on quit
  (a SIGTERM closes Ragtime as the close box does, `QuitSignal`); the host's level is the truth, never kept in Ragtime's settings. The SWR bar sits
  under the ALC meter (`app/src/SwrBar`), and the SWR trip (Settings >
  Rig, off by default, `radio::SwrTrip`) sends the host ABORT and holds
  sending until the operator clears it; with Hamlib it also unkeys the
  rig and sets its power to 0 (through rigctld it sets no power).
- The flight recorder (2026-10-25, idea 2 of "Ideas before
  signatures"): `link::FlightRecorder` keeps every command line, KISS
  frame and session byte count to and from the host, with millisecond
  UTC times, in `flightRecorderDir()` (7 days, 50 MB). Help > Save a
  report zips it with the settings (`app/src/Report`), never a setting
  named like a secret (`link::reportableSetting`): keep any future
  private key out of wxConfig names that rule misses, and out of the
  flight folder. `ragtime_fake_air --replay` plays it back
  (`tests/ReplayHost`).
- The Ear (2026-10-26, Jeff's idea): the ear button
  (or View > Ear) floats a waterfall and spectrum of what the radio
  hears (`app/src/EarFrame`, `EarScope`), with Data2G's passbands drawn
  on it. It records alongside data2g-host from the same PipeWire source
  (`ear::findHostCapture`, `audio::EarAudio` in `audio/`), passive on
  the host's card and only while open; on Windows through WASAPI in
  shared mode (`audio/src/WasapiEar.cpp`, the host found by its audio
  session's process, 2026-10-27). With no host and nothing picked it
  listens to the default input, rig or no rig (`ear::pickSource`). Step 2: it reads the rig's filter while open (mode,
  width, PBT; `radio::RigFilter`, through rigctld or Hamlib) and
  measures the real edges from the band's noise (`ear::EdgeFinder`),
  drawing both. Step 3: with "Let the Ear change the rig's filters"
  ticked (off by default; through Hamlib or rigctld, Jeff's OK
  2026-10-26), the edges are dragged on the scope and the filter is set
  at once, but never while data2g-host transmits (receiving is fine,
  Jeff 2026-11-15)
  (`ear::FilterEdit`, `ear::Calibration` in `ear/FilterControl`;
  `app/src/EarFilterBar`, `EarControls`): on trial for 15 s with a
  clock beside Undo and Keep, Keep pressed twice for a cut into Data2G's
  passband (with the 500 Hz switch on, only into its 200 Hz middle,
  `ear::data2gKept`), Disable (PBT centred, notch off) and four presets (tap
  recalls, hold stores, pushed in while the rig has them), the notch's
  bar dragged out of its NF OFF bay (`ear::NotchModel`, learned from the
  dip, `EdgeFinder::dip`; in Hz, NOTCHF, on a Yaesu or Kenwood) and
  drawn at the width measured from the dip (its -6 dB width,
  `Dip::widthHz`, learned per step in `NotchModel::widthsHz`); on a rig
  with a notch width control (Hamlib's ext level or parm named like
  NOTCH WIDTH, today the FTX-1's `DSP_IF_NOTCH_W`; through rigctld that
  one name only, `radio::NOTCH_WIDTH_PARM`, Jeff 2026-11-28) grips on
  the shading's sides, or the wheel over the bar, pick the rig's width
  steps (`ear::withNotchWidth`), which presets keep, Revert (the
  filter as the Ear found it), and Calibrate for the twin PBT (`ear::KnobModel`: Hz per knob,
  the real centre and width, and a cut in the audio that hides an edge;
  two turns each side, as an IC-7100 hides its low edge under a cut
  at 70 Hz). A rig with IF SHIFT and no PBT (a Yaesu, 2026-11-26) gets
  the same: the shift for the middle, width and shift together for one
  edge, Calibrate by shifts of 200 and 400 Hz each way (kept for every
  width, `ear::calibrationWidth`), and an FT-891's widths
  (`ear::widthSteps`); Hamlib 4.7 can't read an FT-891's shift once it
  is on, so `HamlibRig` takes it as written then. Quitting puts the filter back as found
  (`MainFrame::filterEdit_`). Span (Jeff, 2026-10-26): the picture goes
  to 4, 8 or 12 kHz, or Auto fits it past the filter's high edge
  (`ear::AutoSpan`; FM and AM start at 8 kHz, and on FM the Ear draws
  and sets no filter).
- Reactions (2026-10-26, Jeff's idea): the Reaction
  record (`0x14`) names one of 20 palette entries (12 emoji, 8 ham words,
  `protocol/Reactions.h`, frozen like text table 0). On D2G ours rides
  in our next burst within 30 s, else goes alone at most once a minute
  (`BroadcastChat::react`); in a session it goes into the stream at
  once (`SessionChat::react`). Chips under the line and a React submenu
  (`app/src/ReactionChips`); Hello takes bit 6, so `TAKES_ALL_V0` is
  `0x7F`.
- History kept for good (2026-11-25, Jeff): the Broadcast log and each
  station's session log roll into a history folder beside them past
  1 MB (`broadcast-history/`, `W1AW-history/`, one file per roll named
  for its first run's UTC time) instead of being cut, and are never
  deleted. `history::Pages` (`core/src/history/`) reads them back from
  the newest end; the tabs open with `HISTORY_PAGE` (500) rows, and
  `loadOlder()` puts the page before in front when the list scrolls
  near its top (`ChatList::onNearTop`, `showRowsAbove`). Past
  `MAX_MESSAGES` the oldest rows go back to the pages, except while
  `holdHistory` holds them (the list not following the newest).
  `ChatList` builds only the rows whose HTML changed (PR #190).
- Edits (2026-11-17, Jeff's ask, PROTOCOL.md "Edits"): a broadcast line of ours can be fixed
  until its seal goes, or would go with chat seals off (60 s quiet, 10
  minutes at most; `Seals::draft`). The fix is the same Text record
  (number and time) with flag bit 6, so the seal lists it and repairs
  bring it to a station that missed it (`BroadcastChat::edit`, Edit...
  and Up in the entry line). Broadcast only.
- Inline pictures (2026-10-27, Jeff's idea): the
  picture button left of the floppy on both tabs opens `PictureDialog`
  (the most airtime, the longest side, 640 by default), which shrinks
  the picture with `picture::downscale` (DPID) and codes it as
  progressive JPEG 2000 (`image::encode`) to fit the bytes that time
  takes at the gear. It goes as a file with kind `0x01` on FileOffer or
  FileAnnounce (docs/PROTOCOL.md, "Pictures"), kept as sent in
  `sentPicturesDir()`, sealed and repaired on D2G as any file. Pictures
  up to 64 KB come without asking and show under their card as they
  come (`PictureShelf` in `app/src/Pictures`); a click opens
  `PictureFrame`, with zoom. In a session the stop sign sends
  FileCancel `0x84` and the receiver keeps what came. From the air only
  a JPEG 2000 codestream or a Cool-chic stream is ever decoded.
- Cool-chic pictures (2026-10-29, Jeff's go-ahead): Orange's overfitted
  codec ported to C++ (`image/src/coolchic/`, BSD-3 notice kept; the
  decoder `image::coolchicDecode` and streaming `coolchic::Decoder`, the
  encoder `image::coolchicEncode`, PRs #108 and #109), bitstream in
  `docs/PICTURE_CODEC.md`. The picture dialog's Codec list offers it;
  it fits on its own thread to the airtime's bytes with the preview
  updating, Cancel asks first, then the arrival plays. The Reception
  Preview is a window of its own beside the main one, not modal (Jeff,
  2026-11-15). Kind `0x02`
  (`protocol::FILE_COOLCHIC`, told from JPEG 2000 by the bytes).
- Voice messages (2026-10-31, Jeff's idea): clips coded with sotto, Jeff's
  2,400 bit/s vocoder, go as files of kind `0x03`
  (`protocol::FILE_VOICE`). `core/src/voice/` has the `.sot` file (a
  7-byte header, sotto's 6-byte frames, silence runs, the pause gate,
  reading a prefix or a file with holes), the pitch strip drawn from the
  frames' bits, and the airtime sums (docs/PROTOCOL.md, "Voice"). Codec
  1 is sotto v3.6 (Jeff, 2026-10-31), frozen, 8 kHz; since 2026-11-02
  (Jeff) Ragtime sends codec 2, sotto v3.8: 16 kHz with a 4-8 kHz hiss
  band in the pitch field of all-unvoiced frames, the same 48-bit frame.
  `packaging/sotto.sh` pins sotto main with `SOTTO_VARIANT=v3.8`;
  Ragtime needs a library saying "sotto v3.8, shape 9-9-8-8" and plays
  codec 1 through its 8 kHz decoder, doubled (the 16 kHz one would read
  codec 1's pitch as loud hiss). libsotto (LGPL) is loaded at run time,
  never linked. Its test clip `tests/data/voice/` is LibriSpeech, CC BY
  4.0.
- Voice rates (2026-11-07, Jeff): the
  deck's Rate picks 2400, 1200, 600 or 400 bit/s, codecs 2 to 5
  (`voice::Codec`, `voice::codecOf`; 40, 80 and 120 ms superframes).
  Since 2026-11-07 (Jeff) codecs 3 to 5 carry v3.8's 4-8 kHz hiss band,
  16 kHz both ways: the pin (sotto e707134: the low rates pitch fixes, the 600/400 hiss cap, and since 2026-11-24 the better 2400 and 600/400 encoders, same frames) has `sotto_lr_enc16` /
  `_dec16` and `sotto_vlr_enc16` / `_dec16`, which `voice::Sotto` binds
  (a fixed-point build's NULL stubs leave them off).
- The voice chain (2026-10-31, voice PR 3): `voice::Chain` in `core/src/voice/` takes the
  microphone through Gain, 48 kHz, a high-pass, noise reduction, the
  leveller (with the Target knob's peak mode), 16 kHz and the limiter, with
  live meters (`takeReadings`) and a per-clip Level check (P.56 active
  level, loudest 10 ms, noise), aimed at sotto.h's figures (-26 dBFS
  active, -7.3 dBFS ceiling). The noise reducer is RNNoise 0.1.1 in
  `denoise/` (`ragtime_denoise`, fetched with git; 0.2 would download
  its model at build time). `ragtime_voice_chain`
  runs a WAV through it.
- Voice audio (2026-10-31, voice PR 2): `audio::VoiceAudio`, the
  microphone and speaker on PipeWire and WASAPI (`PipeWireVoice.cpp`,
  `WasapiVoice.cpp`; `PipeWireApi.h` and `WasapiCommon.h` shared with
  the Ear), clips played whole or as they come; `voice::devices`
  (`core/src/voice/Devices`) leaves out every card a data2g program
  uses, both halves, and the ones remembered (`/Voice/RadioCards`);
  `voice::checkMicrophone` for Settings > Voice's Test; the bench's
  `tools/voice/pretend_desk.sh` and `ragtime_voice_probe`.
- The microphone deck (2026-10-31, voice PR 4): the
  microphone button on both tabs (or View > Microphone deck) floats
  `app/src/VoiceDeck`, a drawn rack panel (`app/src/DeckParts`) over
  the voice chain: meters, Glissando-feel knobs (`core/src/deck/`,
  `deck::Knob`), the pitch scope, the Level check lamps, Push to talk
  (or the space bar) and Monitor. libsotto is loaded at run time, never
  linked (`voice::Sotto` in `audio/`, `$RAGTIME_SOTTO` to point at
  one); its commit is pinned in `packaging/sotto.sh` and
  `packaging/build-sotto.sh` builds it for the AppImage;
  `packaging/windows/build-sotto.sh` cross-builds `sotto.dll` and
  `libcodec2.dll` for the Windows zip (make-zip.sh runs it, or takes
  them from `RAGTIME_SOTTO_DLL_DIR`; before issue #166's fix the zip had
  none). Play and Save as WAV say in a box when sotto won't load.
- Voice cards (2026-10-31, voice PR 5 of the sotto plan,
  `app/src/VoiceCards`): a
  play button, the pitch strip and the length under the line on both
  tabs, Play on the Files tab, decoded with libsotto and played through
  the one `audio::VoiceAudio` `MainFrame` owns; `voice::PlayQueue`
  (core) queues them and plays arrivals while the deck is open; the
  Broadcast and session logs keep the file's path, so cards (and
  broadcast pictures) come back after a restart. Since 2026-11-03
  `voice::PlayWatch` (core) ends a clip by the clock when the speaker
  never says it has played it, and stops one it stopped taking, and
  `PipeWireVoice` runs the window's requests on its own thread, so the
  window never waits for PipeWire.
- Voice while it arrives (2026-10-31, voice PR 6): in a ragchew a clip plays as it comes
  (`voice::ArrivalPlayer`, core), starting once the bytes will keep
  ahead of the speech ("starts in 12 s" on the card) and covering a dry
  spell as sotto covers a lost frame; the stop sign on a session voice
  card coming in (FileCancel `0x84`, keeping what came); Save as WAV on
  the cards' menus (`voice::wavFile`).
- Data2G PR #98 (merged 2026-10-23) gives SNRs: on `BCAST HEARD` and
  `MISSED`, and `SN` for the session partner. Ragtime shows them in the
  station list's SNR column and feeds the gearshift's Auto the weakest
  station heard in 15 minutes (`Stations::weakestPath`). Hosts from
  before it still work, without either.

## Rules Jeff has set

**The repository** (2026-11-20)

- No localisms: no hostnames, absolute paths of anyone's machine, private
  links (Claude Docs, artifacts) or notes about one build sandbox. Write
  for someone who has only this repository. Paths in examples are
  relative to the checkout, or plainly made up (`/home/op/...`).
- No development-only documents: no handoffs, roadmaps, work logs or
  dated test reports. Docs describe what Ragtime is and does, how to
  build, test and release it, and what goes on the air.
- No one-off test setups: tests and benches are ones anyone can run from
  the checkout (ctest, `ragtime_fake_air`, the pretend radio). Setups tied
  to particular hardware or machines stay out.
- Every platform with a build has a release recipe in `packaging/` and a
  job in the full CI run that builds, tests and keeps its files.

**Data2G**

- Never pull Data2G code in: no vendoring, submodule, linking or copied
  snippets. Ragtime talks to a separately running `data2g-host` over TCP:
  KISS on 8100, commands on 8300, and the session byte stream on the
  command port + 1 (8301). `docs/DATA2G.md` sums up the host as Ragtime
  meets it, read from Data2G's source at a pinned commit.
- Don't guess how Data2G behaves: read it (Andrew's advice, 2026-10-25).
  Clone https://github.com/arodland/Data2G and start with its `docs/broadcast.md`, `docs/arq.md`,
  `data2g/host.py`'s docstring and `data2g/kisslink.py`. Reading is fine,
  copying code is not. When something you need isn't in
  `docs/DATA2G.md`, or Data2G has moved past its commit, read the source
  and update that page in the same PR.
- Never start `data2g-host`; the operator runs it. Ragtime never plays
  to the radio's sound card or opens its ALSA devices. Voice records
  and plays only on the operator's own microphone and speaker, never a
  card a data2g program uses (`voice::devices`). The Ear only records,
  through PipeWire (WASAPI's shared mode on Windows), alongside
  data2g-host, passive on the host's card and with a long latency so
  the host never notices. It keys the rig only with "Key the rig when
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

- A session (ragchew) starts only when the operator presses **Call**
  (or double-clicks a callsign, Jeff 2026-11-13) or accepts one, and
  ends only on **Leave**. Selecting things never opens or
  closes one, and a quiet link shows as quiet, not closed.
- The tabs start in this order: Broadcast, Session (pops open when a
  session starts, greyed when none), Files. More may follow Files
  (Jeff, 2026-11-25: add-ons are coming): Search, while it is open.
- The window works at 1280×720, and every dialog fits a 1366×768 screen.
- Plain but fun: system theme and standard widgets, no Chaotica art. The
  globe ("Globus"), the gearshift and the 1950s radio vocabulary are
  welcome. "Curmudgeons" and "lids" are flavour words for the docs and UI
  copy, never defined terms in the spec.
- The radio is the source of truth for frequency: retune only when the
  operator asks, by picking a frequency on the dial (the hot frequencies,
  Custom..., or Ctrl or Shift with the wheel), through rigctld (`F`) or
  Hamlib, never while transmitting. Ragtime reads the dial and the
  transmit meters from
  rigctld (`\chk_vfo`, `f`, `+m`, `+l` levels, `+u MN` and
  `+p DSP_IF_NOTCH_W` only: the operator's
  rigctld is shared), or from the rig it
  opens with Hamlib, where PTT is the only thing it sets unasked, the SWR
  trip (off by default) the only thing that sets the power, to 0. The
  Ear, with its own box ticked (off by default), is the only thing that
  sets the receive filter's width, PBT and manual notch, never while
  data2g-host transmits,
  through Hamlib or rigctld (`M`, `L PBT_IN`, `L PBT_OUT`,
  `L NOTCHF_RAW` and `U MN`, Jeff's OK 2026-10-26; for a Yaesu or a
  Kenwood, `L NOTCHF`, the notch in Hz, where there is no NOTCHF_RAW, and
  `L IF`, IF SHIFT, where there is no PBT, Jeff 2026-11-26; the notch's
  width, `P DSP_IF_NOTCH_W`, where the rig has that control, Jeff
  2026-11-28); it and a picked
  frequency are the only set commands Ragtime ever sends rigctld. It
  never keys through rigctld (NET rigctl is left out of the Hamlib
  list).

## Layout

| Path | What |
| --- | --- |
| `core/` | `ragtime_core`, the socket-free library: no wx, no sockets in the engines, all unit-tested. Headers in `core/include/ragtime/`, sources in `core/src/`, one subfolder per area: `protocol/`, `link/`, `text/`, `files/` (package B; G adds `FileRows`, `FileTraffic`), `globus/` (C), `gearbox/` (D), `broadcast/` (E, with `BroadcastLog`; H adds `Airtime`, `Seals`, `BroadcastFiles`), `session/` (F, and `SessionLog`; `StreamLog` for cuts), `history/` (the chat logs' rolled files, read back a page at a time), `search/` (the Search tab's search over the logs), `radio/` (the dial from rigctld, the US data segments, the path log, Recent work, the rig settings, the PTT follower and the hot frequencies), `picture/` (inline pictures with no codec: the image, DPID downscaling, EXIF orientation, what fits a number of seconds), `ear/` (the Ear's spectrum, finding data2g-host's sound card in PipeWire's graph, the host's process by its port, Data2G's passbands, the filter's edges from the noise, the filter model, its trials and Calibrate), `voice/` (the `.sot` voice file, the pitch strip, voice airtime; and the voice chain from the microphone to sotto: high-pass, leveller, limiter, resampler, P.56 and the Level check, with the noise reducer handed in; the microphones and speakers with the radio's card left out, and the microphone Test's verdict; no codec). Add sources to `core/CMakeLists.txt` |
| `denoise/` | `ragtime_denoise`: RNNoise as the voice chain's `voice::Denoiser` (`denoise::makeRnNoise`). RNNoise 0.1.1 (BSD-3) is fetched with git at configure time and its C built here without our warning flags; `-DFETCHCONTENT_SOURCE_DIR_RNNOISE=<checkout>` builds offline, and `-DRAGTIME_RNNOISE=OFF` gives a stand-in that makes none |
| `audio/` | `ragtime_audio`: `audio::VoiceAudio`, voice's microphone and speaker (PipeWire or WASAPI, never the radio's card); `audio::EarAudio`, the Ear's listener, PipeWire loaded with `dlopen` (never linked) on its own thread, or on Windows WASAPI in shared mode (`WasapiEar.cpp`) on a thread of its own; `processListeningOn` (the host's process by its port: /proc, or Windows' TCP table); on macOS Core Audio, from `platform/macos/`; with `-DRAGTIME_PIPEWIRE=OFF` stand-ins that say the Ear can't listen and voice can't record |
| `platform/macos/` | The Mac's own code (Tim Vetter's port): Core Audio voice and Ear (`CoreAudioVoice.mm`, `CoreAudioEar.mm`, devices, the radio's devices left out by the modem processes using them), the microphone permission, `/dev/cu.*` serial ports and where sotto's dylib is looked for. The audio and rig libraries pick these sources only on Apple (`docs/MACOS.md`) |
| `platform/android/` | The Android app's native half (Tim Vetter's port), socket-free: `AndroidClient` (the engines behind one snapshot and string actions; `AndroidFiles.cpp`, `AndroidGear.cpp` and `AndroidPictures.*` are its files, gearshift and picture coding) and `DemoAir` (demo mode's net: `tests/FakeHost.*` and made-up stations, stepped on a thread of its own). Built into `libragtime_jni.so` by `android/app/src/main/cpp/`, and into the `tests/android/` tests on the desktop |
| `image/` | `ragtime_image`: the picture codecs, `image::load` (JPEG 2000, WebP, HEIF from disk), `image::encode` and `image::decode` (JPEG 2000 only, from the air). Kept out of core (no codecs there); with `-DRAGTIME_PICTURES=OFF` a stand-in that says pictures can't be made or shown. `cmake/BuildImageLibs.cmake` builds the four libraries statically for Windows (`-DRAGTIME_BUILD_IMAGE_LIBS=ON`) |
| `rig/` | `ragtime_rig`: `rig::HamlibRig`, the rig opened with Hamlib on a thread of its own, Hamlib's rig list and the serial ports. Kept out of core (no ports there); with `-DRAGTIME_HAMLIB=OFF` a stand-in that says Hamlib is missing |
| `app/` | The wxWidgets app (`app/src/`). It draws and wires; logic goes in core. `MainFrame` is the one main window and owns the engines; `HostConnection` (data2g-host) and `RigConnection` (rigctld: questions, and the Ear's filter settings) are the only code with sockets, and `RigControl` holds the rig as Settings > Rig has it; `Prefs` holds every wxConfig key, and `appDataDir()` is where Ragtime keeps its own files (never wx's `GetUserDataDir()`: on Linux that is `~/.Ragtime`, the settings file); `app/src/globus/` is the Globus pane (`GlobusPanel`, `GlobusView`, `GlobusToggle`) |
| `android/` | The Android app, a Gradle project of its own (`android/README.md`): Kotlin and Compose in `app/src/main/java/org/ragtime/radio/` (`MainActivity` draws the four views, `RadioModel` owns the worker and the state, `HostTransport` the sockets, `RadioService` the foreground service, `NativeCore` the JNI calls), the JNI library in `app/src/main/cpp/` (`Jni.cpp`, with a CMake of its own over core, `ragtime_image` and `platform/android/`), device tests in `app/src/androidTest/` and JVM tests in `app/src/test/`. The top-level CMake build never enters it |
| `tests/` | ctest unit tests for core, plain executables on `tests/Check.h` (`CHECK`, `CHECK_EQ`). Add one with `ragtime_test(<name> <source>)` in `tests/CMakeLists.txt`. `FakeHost.*` is a fake data2g-host on loopback sockets (POSIX, so its test and the bench build only `if(UNIX)`); `FakeAirMain.cpp` is the bench `ragtime_fake_air`, and `ReplayHost.*` its `--replay`; `VoiceSynth.h` makes synthetic voices for the voice tests, and `VoiceChainMain.cpp` is `ragtime_voice_chain` (a WAV through the voice chain); `ChatListTest.cpp` and `RecentWorkPaneTest.cpp` are the wx tests, on real windows (`ragtime_widget_test` in `app/CMakeLists.txt`); `macos/` has the Mac's own tests (Core Audio, sotto in a bundle, the app quitting cleanly); `android/` has the Android adapter's tests (`ragtime_android_*_test`: the client, files, the gearshift, pictures and demo mode on fake hosts, with no phone; `Phone.h` is their pretend phone), built `if(UNIX)` |
| `tools/` | Reference scripts: `icon/` (`make_leaf_svg.py` draws the icon, the Maple Leaf Rag's leaf going digital, as `app/res/ragtime.svg`; `make_icons.sh` renders its PNG and ICO; `make_android_icon.py` makes the Android app's vector icons from the same SVG), `globe/` (sunlit globe renderer, and the generators of the map tables in `core/src/globus/`: coasts and borders from Natural Earth, the big lakes from `ne_110m_lakes.geojson`, state and province lines from Natural Earth's 1:10m admin-1 lines, fetched for the run, for the countries in `gen_admin1_lines.py`), `gearbox/` (ladders), `ear/` (`pretend_radio.sh`, a pretend radio and data2g-host recorder in PipeWire for the Ear, and `make_radio_noise.py`, its audio), `ham_table/` (text table 0 generator and corpus: `gen_table.py --check`), `gcc16/check.py` (GCC 16 check), `android-smoke.sh` (builds the Android app and runs its device tests against a `ragtime_fake_air` you started), `codecs/` (the 2026-10-29 test of Ragtime's JPEG 2000 against JPEG XL, CompressAI and Cool-chic at 1.6 and 4 kB: scripts, bitstreams and results, its README says how to run them; not built) |
| `cmake/` | CMake helpers. `ragtime_warnings(<target>)` gives every target `-Wall -Wextra` and, with `RAGTIME_WERROR` (on by default), `-Werror`; `Toolchain-mingw64.cmake` (the Windows cross build), `BuildWxWidgets.cmake` (wxWidgets 3.2.9, 3.2.11 on the Mac, cloned and built statically with `-DRAGTIME_BUILD_WX=ON`), `BuildHamlib.cmake` (Hamlib 4.7.2, as Data2G's packages carry, cloned and built statically with autotools, `-DRAGTIME_BUILD_HAMLIB=ON`, for the AppImage, Windows and the Mac; its poll thread, on by default since 4.6, is turned off when the rig opens), `BuildImageLibs.cmake` (OpenJPEG 2.5.3, libwebp 1.4.0, libde265 1.0.15 and libheif 1.18.2 cloned and built statically, `-DRAGTIME_BUILD_IMAGE_LIBS=ON`), `EmbedFile.cmake` (the window icon built in), `BuildSotto.cmake` (libsotto built when configuring, if missing), `FetchOpenJpeg.cmake` (OpenJPEG 2.5.3 alone, cloned and built in, `-DRAGTIME_FETCH_OPENJPEG=ON`: JPEG 2000 with no WebP or HEIF, as the Android app builds its pictures) |
| `packaging/` | Release recipes: `linux/make-appimage.sh` (with `ragtime.desktop`), `windows/make-zip.sh` (with the zip's `README.txt`), `macos/make-zip.sh` (on a Mac; the bundle's CMake, `Info.plist.in`, the icon, `check-bundle.py`, `entitlements.plist` and `build-sotto.sh` for the dylibs are beside it; Developer ID signing and notarization with `MACOS_SIGN_IDENTITY` and `MACOS_NOTARY_PROFILE`). All write to `dist/`, named by `release-name.sh`. `sotto.sh` pins sotto's commit and `build-sotto.sh` builds libsotto with its Codec 2 |
| `docs/` | The specs and how-tos: the protocol, Data2G, the gearshift, seals, Cool-chic, the flight recorder, the Mac and Android. Change them in the same PR as the code they describe. `docs/images/readme/` holds the README's screenshots, taken on the bench (`ragtime_fake_air`) at 1366×768 and squeezed with `pngquant`; retake one when its screen changes much |

The version is `project(ragtime VERSION ...)` in the top-level
`CMakeLists.txt`; core gets it from the generated `ragtime/Version.h`.

## Build and test

Ubuntu 24.04 needs `build-essential cmake pkg-config libwxgtk3.2-dev
libgtk-3-dev libhamlib-dev libpipewire-0.3-dev libopenjp2-7-dev
libwebp-dev libheif-dev libheif-plugin-libde265 git` (GTK for the ham phrase
highlight in the entry lines, `PhraseHighlight`; Hamlib for Settings >
Rig, or `-DRAGTIME_HAMLIB=OFF` to build without it; PipeWire's headers
for the Ear, or `-DRAGTIME_PIPEWIRE=OFF`. The Ear loads libpipewire when
it opens, so the build doesn't link it. OpenJPEG, libwebp and libheif
for inline pictures, or `-DRAGTIME_PICTURES=OFF`; `-DRAGTIME_HEIF=OFF`
leaves out only HEIF. RNNoise, for the voice chain, needs nothing
installed: configuring clones it from GitHub, or `-DRAGTIME_RNNOISE=OFF`.
libsotto, for voice, is built when configuring if none is found
(`cmake/BuildSotto.cmake` runs `packaging/build-sotto.sh` into
`build/build-sotto/out`, once per pinned commit; git, a warning
if it can't, `-DRAGTIME_BUILD_SOTTO=OFF` to skip; Linux only).

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j$(nproc)
    ctest --test-dir build --output-on-failure

Tests are on by default (`-DRAGTIME_TESTS=OFF` turns them off), and
every test runs headless, the app included (`ragtime --version`), but
two: `ragtime_chat_list_test` and `ragtime_recent_work_pane_test`
(`tests/ChatListTest.cpp`, `tests/RecentWorkPaneTest.cpp`, built in
`app/CMakeLists.txt`) use real windows, so they need a display
(`DISPLAY=:91` with Xvfb) and are skipped without one.
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
unit tests run under Wine, the AppImage, and the macOS app on GitHub's
Apple Silicon and Intel runners. The last three keep the zips and the
AppImage as the run's artifacts. Start a full run on a PR that
changes CMake, packaging or platform code, and always on a release PR.
Before pushing C++, run `tools/gcc16/check.py` (needs Docker): it
compiles every source in `gcc:16` at both levels without apt. No CI
job builds the Android app; the Ubuntu jobs do build and run its
adapter's tests (`tests/android/`).

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
take a while. The bots react to about one line in two. `--replay FOLDER` plays a flight recording (a report's
`flight/`) on host A's ports instead of the air, with `--from TIME` and
`--speed X` (docs/FLIGHT_RECORDER.md). The bots seal what they broadcast, answer repair requests
and take broadcast files without asking; `--loss PCT` has hosts A and B
lose that share of the group frames they hear, so repairs happen, and
`--files` has the first bot broadcast a bulletin file a minute in and
every 6 minutes. `--voice` has the last bot broadcast a made-up voice
clip 40 s in and every 4 minutes, and bots answer a session voice
message with one (`RAGTIME_SOTTO` names the `libsotto.so` to play them);
`--voice-file FILE.sot` has them offer that clip in a ragchew instead
of their bulletin, so it plays while it arrives (slowly with `--rate
150`). A call
from host A or B is answered on its second try, and each host says
`BUSY ON` and `BUSY OFF` around the others' group bursts. The hosts report SNRs,
each bot at its own strength; `--no-snr` plays a host from before Data2G
PR #98; `--lose-headers PCT` loses that share of the burst headers
hosts A and B hear, so the HEARD line names no call and the SNR comes
on the frames' Hand reference. `--rig PORT` plays a rigctld for the dial (14.105 MHz, or
`--dial HZ`; `--dial-hop` moves it outside the data segments one minute
in three; `--rig-vfo` plays a rigctld started with `--vfo`;
`--rig-yaesu` plays a Yaesu, with no PBT and the notch in Hz (`+l
NOTCHF`, `L NOTCHF`) instead of NOTCHF_RAW, IF SHIFT (`+l IF`, `L IF`)
and an FT-891's widths, and an FTX-1's notch width (`+p DSP_IF_NOTCH_W`,
`P DSP_IF_NOTCH_W 0` or `1`, 1 at first); it answers
the meters too, its SWR 1.3 or `--rig-swr X`, an ALC that follows host
A's `TXPOWER` for Auto ALC, and the S-meter, -40 dB over S9 or
`--rig-noise DB`) and prints anything it is
sent but `\chk_vfo`, `f`, the levels, `+m`, `+u MN` and the Ear's
settings (it starts at USB, a 2400 Hz filter, PBT centred and the notch
off at 0.5, whatever the pretend radio's filter is, takes `M`,
`L PBT_IN`, `L PBT_OUT`, `L NOTCHF_RAW` and `U MN`, prints each
as `fake rigctld set: ...`, and answers with them after; a tune, `F`,
the same, and the dial reads it after). For Hamlib with no
radio, pick maker Hamlib, model Dummy in Settings > Rig: a pretend rig
at 145 MHz, which "Key the rig" keys at the fake hosts' PTT. Headless:
`Xvfb :91 &`, `DISPLAY=:91 ragtime ...`, `xdotool` to click and type,
`import -window root` for a screenshot. Never point the bench at the
ports of a real data2g-host that is running.

For the Ear, `tools/ear/pretend_radio.sh up` (needs PipeWire with
`pipewire-pulse`, `pactl`, `pw-play`, `pw-cat` and python3-numpy) makes
a pretend radio source with Data2G-like bursts and a stand-in
`data2g-host` recording from it; `... down` removes them.
`build/tests/ragtime_ear_probe --seconds N` prints what the Ear finds
and hears, with no window (`--span HZ` up to 12000). Headless, PipeWire needs
an `XDG_RUNTIME_DIR` of its own, a session D-Bus there, then `pipewire`,
`wireplumber` and `pipewire-pulse` started in the background; and
`openbox` on the Xvfb display, or floating windows stack wrong. Never
point either at the radio's USB codec.

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

**Android.** The app builds with Gradle, apart from the CMake build
above: JDK 17, Android SDK 36 and NDK 28.2 (`JAVA_HOME`, `ANDROID_HOME`;
the pins are in `android/README.md`), then `cd android && ./gradlew
:app:assembleDebug :app:lintDebug`. The first configure clones OpenJPEG
from GitHub. Three kinds of test: the adapter's, under ctest with the
rest (`ctest -R android`, no phone needed); the JVM's,
`./gradlew :app:testDebugUnitTest`; and the device's, which need a phone
or emulator on adb and a `ragtime_fake_air` running:
`tools/android-smoke.sh SERIAL [PORT_OFFSET]` builds, installs, forwards
the bench's ports and runs them (an offset of 10000 for a phone whose
own Data2G TNC holds 8100 and 8300), then puts the app's saved settings
back. Never point them at a modem that is on a radio.

## Releases

1. A PR sets the version, `project(ragtime VERSION ...)`, and gets a full
   CI run, started by hand on its branch (no `[skip ci]`).
2. After Jeff merges it, build from the merge commit:
   `packaging/linux/make-appimage.sh` and `packaging/windows/make-zip.sh`
   (and `packaging/macos/make-zip.sh`, which needs a Mac).
   Or take the AppImage and zips from a full CI run on that commit; those
   jobs print each file's sha256sum in the log and the run summary, so check the
   download against it. Files are named
   `Ragtime-<version>-beta-<hash>-...` (`release-name.sh`).
3. Write `SHA256SUMS-<version>-beta` beside them (`sha256sum`), and
   smoke-test headless: the AppImage under Xvfb and `ragtime.exe` under
   Wine, each against `ragtime_fake_air` (the Mac zips are tested by
   their CI jobs' ctest).
4. Draft release notes: say whether it talks to the last release (a
   protocol change breaks that), that data2g-host is the operator's to run
   (never bundled or started), and how the gearshift finds Data2G's
   `mode_thresholds.json`. From the release with voice on: credit sotto,
   Codec 2 (LGPL-2.1, why libsotto is a shared library) and the CC BY 4.0
   datasets its codebook was trained on, as the README's "Credits" does.
5. Jeff tags `v<version>-beta` on the merge commit and publishes.

## Code

- C++17, wxWidgets, CMake, the same toolchain as Glissando. The AppImage
  and the Windows zip follow Glissando's recipes (its `docs/APP.md`),
  simpler for having no audio or codec2. The Mac build (Apple Silicon
  and Intel, a zip each) is in `docs/MACOS.md`; there is no Linux ARM
  build yet. The Android app is Kotlin and Compose over the same core
  (`docs/ANDROID.md`).
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
