# Ragtime

A plain, friendly chat and file-transfer client for
[Data2G](https://github.com/arodland/Data2G), the HF data modem by Andrew
Rodland, KC2G. Ragtime is a sibling of
[Glissando](https://github.com/baitisj/glissando), Jeff Baitis's (AG7EW)
chirp-mode chat app, and is named for the same musical family and for the
ragchew.

Status: Stage 1 under way. Ragtime connects to data2g-host and chats on
the D2G broadcast group, with the station list and Globus; sessions and
files come next. It hasn't been on the air with a real data2g-host yet.

## Building

On Ubuntu 24.04:

    sudo apt-get install build-essential cmake libwxgtk3.2-dev
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j$(nproc)
    ctest --test-dir build --output-on-failure
    ./build/app/ragtime

### Trying it without Data2G

The build includes a bench of fake data2g-hosts with a few chatty
stations of its own. Nothing goes on the radio.

    ./build/tests/ragtime_fake_air &
    ./build/app/ragtime --config /tmp/ragtime-a.conf

Set any callsign and locator in Settings > Station; the address
127.0.0.1 and ports 8100 and 8300 are the defaults. A second Ragtime
with its own `--config` file and ports 8110 and 8310 talks to the first.

## The idea

- **Welcome to the D2G net.** D2G is a net that never closes. You check
  in, net control asks for traffic, and the traffic gets passed. Every
  burst teaches the net who hears whom.
- **Sometimes we ragchew.** When operators want to talk, they enter a
  ragchew on purpose and leave it on purpose.

Ragtime is a front end for a separately running `data2g-host`, reached
over TCP. It never starts the modem and never touches the sound card.
Messages are signed, never encrypted, as Part 97 requires.

## Plan

| Document | What |
| --- | --- |
| [docs/PLAN.md](docs/PLAN.md) | The full design: the net, ragchews, broadcast mode, files, signing, store and forward, frequencies, decisions and open questions |
| [docs/STAGE1.md](docs/STAGE1.md) | Stage 1, parity with Glissando's Data2G mode, split into work packages |
| [docs/GEARBOX.md](docs/GEARBOX.md) | The gearshift: how a Data2G mode is picked from the Submode Map |
| [docs/MOCKUPS.md](docs/MOCKUPS.md) | The screens, colours, Globus and seal icons |
| [CLAUDE.md](CLAUDE.md) | Working notes and ground rules for contributors |

## Licence

MIT. See [LICENSE](LICENSE).
