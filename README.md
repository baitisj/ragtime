# Ragtime

A plain, friendly chat and file-transfer client for
[Data2G](https://github.com/arodland/Data2G), the HF data modem by Andrew
Rodland, KC2G. Ragtime is a sibling of
[Glissando](https://github.com/baitisj/glissando), Jeff Baitis' (AG7EW)
chirp-mode chat app, and is named for the same musical family and for the
ragchew.

Ragtime helps you keep in touch. Every burst it hears teaches it when,
and on which band, each of your friends gets through, so it can tell you
who is likely around now, and the best time of day to check in to the
D2G net to find them.

Status: 0.1.1-beta. Ragtime connects to data2g-host,
chats on the D2G broadcast group. Current features include:
- "Globus," which plots your friend's maidenhead coordinates
   on a globe,
- "Gearshift," a graphical representation of different Data2G modes,
  which aids in the selection of the optimal rate for chatting with
  the group. NOTE: THIS REQUIRES DATA2G's mode_thresholds.json
  -- see below for more details.
- Peer-to-peer sessions, which feature automatic rate negotiation and
  file transfer.
- Group file transfer which includes repair rounds.
- "Recent work," under the station list: the stations you hear and
  work, the ones likely around at this hour first, from two weeks of
  the path log (who was heard when, and on which band).

## Downloads

Releases carry a Linux AppImage (x86_64, glibc 2.39 or newer, as on
Ubuntu 24.04) and a portable Windows zip (64-bit). Ragtime needs a
[data2g-host](https://github.com/arodland/Data2G) running on your
computer or your network; it never bundles or starts one. Give its
address and ports in Settings > Station.

The gearshift reads Data2G's `data2g/codes_data/mode_thresholds.json`
from your Data2G clone: from `$DATA2G_HOME`, or a `Data2G` or `data2g`
folder in your home folder or its `src`, `git`, `code`, `projects`,
`Projects`, `repos` or `dev` folder. Otherwise load the file with **Load
thresholds...** in the gearshift. See [docs/GEARBOX.md](docs/GEARBOX.md).

## Building

On Ubuntu 24.04:

    sudo apt-get install build-essential cmake pkg-config libwxgtk3.2-dev libgtk-3-dev libhamlib-dev libpipewire-0.3-dev
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j$(nproc)
    ctest --test-dir build --output-on-failure
    ./build/app/ragtime

### Packages

    packaging/linux/make-appimage.sh      # dist/Ragtime-<version>-...-x86_64.AppImage
    packaging/windows/make-zip.sh         # dist/Ragtime-<version>-...-windows-x86_64.zip

The Windows build cross-compiles on Ubuntu 24.04 with
`g++-mingw-w64-x86-64-posix` and builds its own static wxWidgets and
Hamlib (which needs `autoconf automake libtool`); with
Wine installed, `ctest --test-dir build-win` runs the unit tests as
Windows programs.

### Trying it without Data2G

The build includes a bench of fake data2g-hosts with a few chatty
stations of its own. Nothing goes on the radio.

    ./build/tests/ragtime_fake_air &
    ./build/app/ragtime --config /tmp/ragtime-a.conf

Set any callsign and locator in Settings > Station; the address
127.0.0.1 and ports 8100 and 8300 are the defaults. A second Ragtime
with its own `--config` file and ports 8110 and 8310 talks to the first.

A tester's report (Help > Save a report) plays back the same way:
`ragtime_fake_air --replay flight/` gives a Ragtime the host's side of
the recording, at the spacing the tester saw (docs/FLIGHT_RECORDER.md).

## The idea

- **Welcome to the D2G net.** D2G is a net that never closes. You check
  in, net control asks for traffic, and the traffic gets passed. Every
  burst teaches the net who hears whom.
- **Sometimes we ragchew.** When operators want to talk, they enter a
  point to point ragchew on purpose and leave it on purpose.

Ragtime is a front end for a separately running `data2g-host`, reached
over TCP. It never starts the modem and never plays to the sound card;
its Ear only listens in, through PipeWire, to what data2g-host records.
It reads the radio's dial from rigctld, or opens the rig itself with
Hamlib (Settings > Rig), and can key it when data2g-host does, for rigs
whose drivers can't be opened twice (off by default). Through Hamlib or
rigctld, the Ear can also set the rig's receive filter: drag the edges
on the waterfall, and the change goes back after 15 s unless you keep
it (off by default).
Messages are signed, never encrypted, as Part 97 requires.

## Plan

| Document | What |
| --- | --- |
| [docs/PLAN.md](docs/PLAN.md) | The full design: the net, ragchews, broadcast mode, files, signing, store and forward, frequencies, decisions and open questions |
| [docs/STAGE1.md](docs/STAGE1.md) | Stage 1, parity with Glissando's Data2G mode, split into work packages |
| [docs/HOW_SEALS_WORK.md](docs/HOW_SEALS_WORK.md) | Seals in plain words, with pictures: what is inside one, how repairs work, and what a late joiner asks for |
| [docs/GEARBOX.md](docs/GEARBOX.md) | The gearshift: how a Data2G mode is picked from the Submode Map |
| [docs/MOCKUPS.md](docs/MOCKUPS.md) | The screens, colours, Globus and seal icons |
| [CLAUDE.md](CLAUDE.md) | Working notes and ground rules for contributors |

## Licence

MIT. See [LICENSE](LICENSE).
