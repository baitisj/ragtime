# Ragtime: plan

Started 2026-10-20 by Jeff (AG7EW) as "Data2G client: plan", a Claude Doc he edited inline. This is the snapshot of that doc taken on 2026-10-22, when the client was named **Ragtime** and Jeff said go for Stage 1. From here this file is the record: change it in the same PR as the code it describes. "The client" below means Ragtime, and "you" means Jeff.

## Summary

The proposal is a new, plain desktop client for Data2G, built from what Glissando's Data2G mode learned on the air. Everything in it is seen through two ideas:

- **Welcome to the net.** D2G is a net that never closes. You check in, net control asks for traffic, and the traffic gets passed. Every burst teaches the net who hears whom.
- **Sometimes we ragchew.** When operators want to talk, they enter a ragchew on purpose and leave it on purpose. While they're in it, they're off traffic duty.

Broadcasts, files, signed messages, pages, boards, and store and forward all hang off those two. Glissando then goes back to being the chirp mode only.

**Stage 1** (agreed with Andrew, 2026-10-21) is parity with Glissando's Data2G mode, in three tabs: Broadcast, Session and Files. The net and everything else build on it.

What the client is:

- A front end for a separately running `data2g-host`, reached over TCP on the KISS, command and session data ports, as Glissando does today.
- The D2G net: check-ins, net control and traffic, plus broadcast mode (a ragchew with everyone listening, on the D2G broadcast group). Ragchews with one station at a time, and file transfer in both. Later, signed (never encrypted) messages, Gemini-style pages and bulletin boards.
- Plain but fun: standard widgets, system theme, no Chaotica art, no easter eggs, no visi-scope. But we can still keep it lighthearted.

What it is not:

- Not a modem. It never starts `data2g-host` and never touches the sound card or PTT for data.
- Not a Glissando fork. The chirp modem, tempos, chords and the console stay in Glissando.

Jeff said go for Stage 1 on 2026-10-22. The decisions only he can make are collected under **Questions for Jeff** near the end, with his answers where he has given them.

## Welcome to the D2G net!

Your model (2026-10-21): D2G is a net that never closes, and check-ins are always welcome. It takes the shape of a traffic net: check in, net control asks for traffic, and stations pass it in turn.

**A check-in, step by step.** SA is the station checking in, and SB is the station that answers and becomes net control.

1. **SA calls CQ** on D2G: `CQ CQ D2G de SA`, or with a service, `CQ CQ BOARD de SA`.
2. **SB answers.** It gives SA a signal report, and says whether it holds traffic for SA. SA is now checked in, and every station that hears either burst updates its path table.
3. **SA acknowledges,** with SB's signal report in return. (My addition: it gives the path in both directions, and it names net control when more than one station answered.)
4. **SB asks the net:** `ANY TRAFFIC FOR SA?`
5. **Show of hands.** Every neighbour that hears SB answers at once, in a show of hands (below): it hears SB, whether it has traffic for SA, and what that traffic costs.
6. **SB tells SA** who holds traffic for SA, in order, and about how long the net will take: "3 stations have traffic for you, about 12 min of airtime."
7. **SB runs the traffic.** As net control it opens connected sessions in order of total cost, collects each station's traffic and passes it to SA. Between sessions it tells SA how it's going. Precedence comes before cost, so Emergency traffic goes first whatever it costs.

**Refinements I'd add:**

- **Direct when possible.** In the show of hands, a station also says whether it heard SA's CQ itself, and how well. If SC hears SA well, SB sends SC and SA to work each other directly, saving a hop, as a traffic net sends two stations off to pass their traffic. SC's signature is on the traffic either way.
- **More than one SB.** Several stations may hear the CQ. They answer with a show of hands too, each with its signal report. SA's client picks net control: a station with traffic for SA first, then the best signal, then a node with a long, steady record: a curmudgeon over a lid. SA's acknowledgement names it, and the others stand by.
- **Keeping the net clear.** A long session blocks the net frequency. Net control may send a pair 3 kHz up to pass their traffic, if both have rig control. Inside the narrow automatic-control segments there may be no room, and then the session stays put and the net waits, held by Data2G's busy reports.

**Every burst updates routing** (your rule):

- A station that hears a burst learns its own path from the sender: SNR, mode, band and hour.
- A signal report it overhears tells it about a path between two other stations ("SB hears SA at -8 dB").
- Not hearing counts too. If SC hears SB's report on SA but never heard SA's CQ, SC has no direct path to SA right now.
- All of it goes into the path table with its time, and ages out over about 2 weeks.
- Stations heard in a ragchew are marked busy. They aren't routing third-party traffic while it lasts, so routes go around them until they Leave.

**The show of hands.** How it works depends on the link, because only some links let signals add up. Each link in a node's links table says which kind it is:

| Link kind | Examples | Can many answer at once? | Show of hands |
| --- | --- | --- | --- |
| Multiplex | SSB and AM on HF; SSB on VHF and up | Yes: signals add in the passband, and a receiver can pull them apart by frequency | Everyone answers at once, each in its own narrow channel |
| Capture | FM, point to point | No: the strongest signal captures the receiver and the rest are lost | Net control polls in turn, strongest path first |
| Wire | IP | No need: each reply comes on its own connection | Replies arrive as they're sent |

The client takes the link kind from the radio's mode (USB, LSB or AM is multiplex; FM is capture), and the operator can override it for each link.

**On a multiplex link: the Hand burst.** Phase 1 is data from the start (your call, 2026-10-21); there's no tone stage.

- **Waveform** (provisional: Andrew's study, 2026-10-22). One FT8-style signal per station: 8-tone continuous-phase FSK at 6.25 Hz spacing, with a constant envelope. Andrew's line: "if your rig works for FT8, it works for this." The code is Data2G's CRC-aided polar code with a list decoder, which the host already has in Python and C++: 63 payload bits plus a CRC-16 make 79 bits, sent as 162 coded bits (rate 0.49). A fixed interleaver spreads them over the symbols, so a fade doesn't take out a run of neighbouring code bits. The frame is `[Costas 6][data 27][Costas 6][data 27]`, 66 symbols in 10.6 s. Two sync blocks are enough, because net control already knows each Hand's time and frequency to within a symbol and a tone. Hand-25 is dropped. It is about 3 dB more sensitive in simulation, but 3.125 Hz is too narrow for rig drift over a 21 s transmission, and Hand-50 already has margin over the traffic it announces. One speed, then:

| Hand-50 |  |
| --- | --- |
| Tone spacing and baud | 6.25 Hz |
| Width | 50 Hz |
| Length | 10.6 s (66 symbols) |
| Channels | 40 in a 2.4 kHz net, 8 in a 500 Hz net (60 Hz apart) |
| 10% failure, S/N in 2500 Hz (Andrew's simulation) | −19.2 dB with noise only; −13.8 good fading; −16.3 poor; −15.8 disturbed; −14.1 on a measured slow, selective 4000 km path |

The weakest S/N in Data2G's traffic training data is −9.2 dB, so a Hand has margin over the traffic it announces on every channel. Andrew also tried faster tones (12.5 and 25 Hz in a time × frequency grid, for the same hands per round with more Doppler tolerance). They lose 3 to 10 dB, because a shorter Hand carries less energy, and Doppler spread doesn't cost enough to make up for it.

- **500 Hz links.** A Hand is 50 Hz wide, so each station's own Hand always meets 97.221(c)'s 500 Hz limit. What a 500 Hz link limits is how many channels fit: the channel plan always sits inside the net's own width, so a 500 Hz net gets 8 channels. It fills them the same way as a wide net (the channel map, below), so expect fewer than 8 hands per round when levels spread widely. When more stations need to answer than fit, net control runs a second round for the rest.
- **Timing** (provisional). The window opens D seconds after net control's `ANY TRAFFIC` burst ends, and `ANY TRAFFIC` carries D: 3 s by default, not the 1.0 s first planned. A station learns its channel only after its host has decoded `ANY TRAFFIC` and passed it to the client, and the client has answered. On a Raspberry Pi 4, decoding alone can take most of a second. The host stamps every frame it reports with the burst's end time, on the sound card's clock, and its frequency offset. The client then asks the host to send its Hand in channel c, at that end + D, shifted by that offset. Each host starts its audio early by its own PTT and output latency, so the RF lands on time. Net control searches a quarter second either side.
- **Frequency.** Radios disagree by tens of hertz, as much as a whole channel. So each station measures net control's frequency offset from its burst and shifts its own Hand to match, landing in its channel as net control hears it. Net control still searches a few hertz either side of each channel (±5 Hz in Andrew's simulation, with Hands placed within ±3 Hz).
- **The channel map.** `ANY TRAFFIC` carries the plan: the delay D, the round, the number of channels, and a list giving each expected station its channel (a 16-bit callsign hash each, so 2 bytes). Near-far is the main risk, so the map is built around it (provisional: Andrew's simulation, with one neighbour on each side fading on its own). A neighbour up to 5 dB stronger costs under 1 dB, 10 dB stronger costs 1 to 4 dB, and 15 dB or more blocks a weak Hand completely on fading paths at the 60 Hz pitch. One empty channel between them (120 Hz) holds even a 20 dB stronger neighbour to a 1 to 4 dB loss. So net control orders the list by the latest SNR it heard from each station, and leaves an empty channel wherever the list jumps by more than 5 dB, so a loud curmudgeon never buries the weak station next door. Strangers, often passing lids, are kept away from expected stations, because their level is unknown: they get channels at the far band edge with an empty channel before them, or a short round of their own, and pick one by a hash of their callsign and the round's nonce (Stranger hands, below). Two strangers in the same channel usually both fail; they try again next round, or check in with their own CQ. A later receiver could decode the strongest Hands first, subtract them and decode the rest, as WSJT-X does for FT8. That would relax the 5 dB rule, but the first version doesn't need it.
- **A second round for the missing** (provisional). On slow fading a single Hand misses more often than its 10% point suggests: the 1% point sits 4 to 6 dB above it (−8.5 against −13.8 dB on good fading), so one long fade can take a Hand near the threshold. Net control knows whom it expected, so a short second round for the missing ones costs one more `ANY TRAFFIC` and recovers most of them.
- **Who stays quiet.** Stations in a ragchew raise no hand, and their traffic for SA waits until they Leave.

**What a Hand says** (provisional: 63 bits of payload, then a CRC-16):

| Field | Bits | Meaning |
| --- | --- | --- |
| Callsign | 28 | Packed as FT8 packs a standard call; a hash for a call that doesn't fit |
| Has traffic | 1 | Traffic for SA, or nothing |
| Heard SA | 1 | Heard SA's CQ directly, so it could work SA without a relay |
| Automatic | 1 | Controlled or Automatic |
| 500 Hz only | 1 | Can send only 500 Hz modes on this link |
| Messages | 4 | How many items, 15 meaning 15 or more |
| Size | 5 | Total bytes, in powers of two |
| Precedence | 2 | The highest among them |
| Cost | 5 | Airtime to deliver it by its best route, on a log scale |
| SNR of net control | 6 | −30 to +33 dB, as heard here |
| SNR of SA | 6 | When heard directly |
| Version | 3 |  |

The 8-bit request tag no longer rides in the Hand. It seeds the CRC instead, so a Hand still can't answer someone else's net, and the 8 bits go to the code. The CRC is 16 bits rather than 14: in Andrew's simulation false decodes come to 2.6e-4 per attempt, with net control trying 3 sync candidates and a list of 8. That is one false hand in about 1% of 40-channel rounds; CRC-14 would make it four times as many.

Net control measures each Hand's own SNR as it decodes it, so one round gives it a fresh signal report on every station that answered, in both directions. A Hand is too short to carry a 64-byte signature. It is only a show of hands, and every claim in it is checked when the traffic actually moves in a signed session. A false hand costs one wasted poll, and the callsign is accountable.

**Stranger hands** (Andrew's point, 2026-10-22). A show of hands isn't only for the net. In the stranger form the caller's burst carries a plan with an empty list: Hand speed, round and channel count, with every channel free. Each station that answers picks a channel at random, just as unknown stations already do in the net's free channels. That fits any call that asks "who's out there?":

- **CQ POTA**, or any CQ that names what it wants: everyone who wants the contact raises a hand at once, and the activator sees the whole pile-up in one round, with an SNR for each, instead of a jumble of overlapping calls.
- **QRZ?** after a ragchew ends: the stations that were waiting to call one of the ragchewers raise hands, instead of all calling at once.
- **A first CQ D2G** from a station with no known neighbours.

A stranger Hand swaps the net's traffic fields (traffic for SA, count, size, precedence, cost) for what a stranger needs to say: callsign (28 bits), a 4-character locator (15), the caller's SNR as heard (6), 500 Hz-only (1), automatic (1) and version (3), which leaves 9 bits spare. What it answers (CQ POTA, QRZ?) seeds the CRC, like the net's request tag. The caller answers with the calls it decoded, in the order it will work them, and runs another round for the rest.

**Picking a channel: a hash with a nonce** (your idea, 2026-10-22). Each round, the caller's burst carries a fresh 16-bit nonce. A stranger's channel is a hash of its callsign and that nonce, taken over the round's stranger channels. Within a round that is as good as a random pick, but every retry reshuffles: two stations that stomp on each other in one round land together again only by chance, 1 in N, rather than every time. It brings two more things for free:

- **A check against false hands.** Net control knows the nonce, so it recomputes the hash from each decoded callsign. A Hand in the wrong channel is a false decode. That cuts false hands by a factor of N, on top of the CRC.
- **No stale hands.** The nonce also seeds the CRC, with the request tag, so a Hand meant for an earlier round can't pass in this one.

Collisions within a round are still the cost. The chance that a hand has its channel to itself is (1 − 1/N)^(k − 1), for N channels and k stations answering. With 40 channels and 10 answering, that is 80%; on a 500 Hz link with 8 channels and 5 answering, 59%. Near-far adds to it, because strangers' levels are unknown and a loud one can block a weak one in the next channel. Using only every other channel removes that risk but raises collisions (20 channels and 10 answering: 63%; 4 and 5: 32%). The nonce makes that trade easier, since a collision now costs one retry and not a repeat (question 22). Either way a busy pile-up takes two or three rounds, and each round is still far shorter than taking callers one at a time.

A node under automatic control never raises a hand for a contact such as CQ POTA, because a contact needs an operator. It raises one only to offer what the net offers: traffic, routing or a service.

**On a capture (FM) link: polling.** Simultaneous answers would capture: the strongest station gets through and the rest vanish without trace. So net control polls instead:

- `ANY TRAFFIC` lists the expected stations strongest path first, by the radio's FM signal strength or Data2G's SNR. Each answers in its own slot: one Hand, carried as an ordinary Data2G frame, plus the link's turnaround time (squelch tail and PTT delay, measured for each link, about 1 s).
- One open slot follows for stations not on the list. If two lids key up at once, capture lets the stronger through, which is the one worth hearing. The other tries again at the next `ANY TRAFFIC`.
- Net control may leave off stations whose path is too weak to pass traffic anyway.

**On IP.** `ANY TRAFFIC` goes to every IP neighbour at once, and the Hands come back on their own connections within a second, signed, since there's no airtime to save.

**Mixed nets.** A node acting as net control on HF, FM and IP at once sends `ANY TRAFFIC` on each, and merges the answers into one list, precedence first, then cost.

**Who builds it.** The client has no audio: data2g-host owns the sound card. Andrew will build the Hand into data2g-host (2026-10-22), with no new dependencies. Net control doesn't put a round through Data2G's normal one-burst-at-a-time receiver. It knows each Hand's start and channel, so it gets its own windowed decoder: an FT8 decoder with the blind search removed, built mostly from parts Data2G already has for its M-FSK modes. His estimate, in new lines for each of Python and C++, with parity tests: about 50 to transmit, 200 to 250 for the decoder, and 150 for host commands, frame timestamps and holding transmit through a window. The primitive, provisionally:

- `HAND SEND ref channel payload`: send at the end of burst `ref` + D, shifted by that burst's offset.
- `HAND LISTEN D channels`: decode the window after the next burst this host sends, reporting `HAND channel payload snr offset` for each Hand decoded, then `HAND END`.
- While a window is open, the host holds its own CQ and broadcast transmissions.

Until it ships, the fallback on every link is polling, as on FM.

**Rules to check.** A Hand is an ordinary data emission, like FT8, so it belongs in the data segments. Whether its packed callsign counts as identification under 97.119 isn't checked, so each station still identifies with its Data2G frames and CW ID.

**Control and the rules.** SA's CQ comes from an operator, so SB may answer it even as an unattended node (97.221(c)). But `ANY TRAFFIC` is SB calling, and unattended SC and SD answering it are answering an automatic station. Outside the automatic-control segments that is allowed only when SB is controlled. So on HF, each band's D2G net frequency should sit inside the 97.221(b) automatic-control segments. A 2.3 kHz channel fits in every one of them (the narrowest is 4.5 kHz, on 20 m), but they are busy with Winlink gateways, so the channel needs choosing with care. On 6 m and up, the net may sit anywhere data is allowed.

**Control state** (your idea, 2026-10-21). The rules depend on who is at the controls, so the client tracks it all the time, not just at setup.

- **Two states on the air:** Controlled (an operator at the controls, there or over a control link) and Automatic (nobody at the controls). 97.221(c) treats local and remote control alike, so the protocol doesn't tell them apart (your point, 2026-10-21). The status bar shows which state applies. The station goes Automatic when the operator turns on **Unattended**, or after an idle timer if the operator switches one on.
- **Signed into the hello and the announce.** Every hello and announce says Controlled or Automatic, signed like everything else. That lets a node tell whether a caller is "a station under local or remote control" (97.221(c)). It also drives the automatic mark on the station list. The state is the operator's claim, and the callsign is accountable for it.
- **The transmit guard enforces it.** On HF, an Automatic station outside the automatic-control segments:
  - answers only callers whose hello says Controlled
  - sends in 500 Hz modes only (`BW500`)
  - never calls first
  - never sends announces or seals unprompted, except the closing seal of a session it was answering

  On HF inside those segments, and on 6 m and up, Automatic adds no limits beyond the band's own.
- **Paths are tagged by control type.** The path table keeps a link's control type next to its band and hour. A link between two Automatic nodes on HF can only use the automatic-control segments, which are narrow and crowded (5 kHz on 40 m, 17 m and 12 m). A link from a controlled station to a node can also use the wider data segments at 500 Hz. Node-to-node forwarding therefore has its own costs, and the scheduler books it into those segments, or onto 6 m and up when a VHF path exists.
- **Remote control stays inside your station.** Other stations never learn whether the operator is local or remote. When the client is run over a control link and the link fails, it stops transmitting within 3 minutes, as [97.213(b)](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-C/section-97.213) requires. It carries on as Automatic only if the operator has chosen Unattended as the fallback. Otherwise it stays silent until the link is back.

## Sometimes we ragchew

A ragchew is a room: you enter it with **Call**, you are in it until someone presses **Leave**, and while you are in it it fills the Session tab with its messages, file offers and link status. Nothing else you click opens or closes it.

**Ragchews and other sessions.** A ragchew is a connected session between operators (what this plan first called a chat session). Underneath it is a Data2G session, and so are node syncs and traffic passing, which aren't ragchews. Stations in a ragchew are keeping it open among themselves, so they aren't available to route third-party traffic while it lasts. They raise no hand on the D2G net, routes go around them, and traffic for them waits until they Leave.

&#91;embedded content: session lifecycle · 6 states\]

A quiet link is still a session: only Leave, a lost link, or the far end leaving ends it.

**Entering.**

- You press **Call W1AW** (in the station list, the station's menu, or a call box). The Session tab opens at once in the Calling state: "Calling W1AW, try 2 of 5", with **Cancel**.
- When W1AW calls you, the Session tab opens by itself: "W1AW started a session with you", with **Leave** right there if you don't want it. The tab flashes once and the Sessions list shows it.
- A toolbar switch, **Accept sessions** (`LISTEN ON`/`LISTEN OFF`), says whether others can call you at all.
- One session at a time, since data2g-host runs one ARQ link. Calling a second station asks you to leave the first.

**Being in a session.**

- The tab header is a status strip: **Connected to W1AW · 14 min**, the current speed and mode from data2g-host's reports, bytes waiting for acknowledgement, and last heard "12 s ago".
- Messages typed in the tab go into the session. Each shows Sent, then Delivered when the far end's modem acknowledges its bytes (the `BUFFER` count, as now).
- When nothing has arrived for a while, the strip says **Link quiet · 1:10 · data2g-host is still retrying**, in amber. It does not close the session. That is the KC2G case: the link was fading, not finished.
- You can still switch to the Broadcast tab and post there. data2g-host sends it while the session is idle, and the message shows **Waiting for the session to go idle** until it goes.

**Leaving.**

- **Leave** sends `DISCONNECT`, which waits for outstanding data to be acknowledged. The strip shows **Leaving, waiting for 312 bytes to be acknowledged**, and a second button, **Leave now**, sends `ABORT`.
- If files are moving, Leave asks first, once: "A file is still coming from W1AW. Leave anyway? It can carry on next time."
- When the far end leaves, or data2g-host gives up the link, the Session tab greys out, its messages still readable, with a banner: "Session ended by W1AW at 14:32" or "Link lost at 14:32 after 3 min without contact", and **Call again**.
- An optional idle limit in Preferences (off by default, or 15 minutes) leaves a session nobody has typed in, with a 60 s warning in the strip first.

**Messages to a station outside a session.** Glissando's directed broadcast message with its own chat ACK is light and works over the air. My suggestion is to keep it in the Broadcast tab as "@W1AW" addressing, so short exchanges don't need a session, but you may prefer one rule: one station means a session. That's a question below.

**Busy stations** (your question, 2026-10-20). Someone calling may hear only one side of a session, so the busy station itself has to say so on the air; its transmissions are the ones a caller can hear.

1. **A busy answer to a CONNECT.** While in a session, my data2g-host answers a CONNECT once with "busy, in a session" instead of staying silent. The caller sees "AG7EW is in a session" at once, rather than "no answer" after five tries, and my partner is spared five CONNECT bursts on top of our QSO. Needs Andrew.
2. **Presence in broadcast mode.** Entering a session, the client sends one short signed broadcast message, `AG7EW QRL with KC2G`; on Leave, `AG7EW QRV`. The QRL may lag, since broadcast bursts go only while the session is idle; the QRV is exact. If data2g-host can also report overheard session bursts, stations that missed the QRL learn it from my side of the QSO.
3. **The caller's screen:** the station list (below), and a question when you press Call on a busy station: **Tell me when free** (waits for the QRV and offers to call then), **Send a broadcast message instead**, or **Call anyway**.

**The station list shows it.** One row per station heard, with a status column that always pairs a word with its dot, so it never relies on colour alone:

| Status | Shown as | We know it from |
| --- | --- | --- |
| Available | ● QRV | Its QRV, or any broadcast traffic with no QRL since |
| In a session | ● QRL with KC2G | Its QRL message, or its busy answer to our CONNECT |
| In a session, partner unknown | ● QRL | Overheard session bursts from it (needs the host report) |
| In a session with you | ● In session with you | Our own session |
| Probably free again | ○ QRL? 25 min ago | A QRL older than 20 minutes with no session bursts heard since |
| Not taking sessions | ○ No sessions | Its announce says Accept sessions is off |
| Automatic services | Badge: board, mail, pages | Its signed announce |

- The other columns: callsign (with the key badge: Verified, New key, Key changed, Unsigned), and last heard with the locator and distance underneath it (FN31 · 3,951 km), or "no locator" until one arrives.
- Hovering a status says how we know and when: "From AG7EW's QRL at 14:02", "AG7EW answered busy at 14:05", "Session bursts heard 2 min ago".
- Sorting stays by last heard, so the list doesn't jump when someone goes QRL. A filter, **Available only**, hides the busy ones.
- A station waiting on **Tell me when free** gets a small bell in its row, and the row flashes once when its QRV arrives.

## Stage 1: parity with Glissando

Agreed with Andrew (2026-10-21): the first stage brings the client to parity with Glissando's Data2G mode, which is the checklist under "What happens to Glissando's Data2G mode". The net, signing, pages, boards, and store and forward come in later stages, on top of it.

Mock-ups of the three tabs: [Data2G client mock-ups](https://claude.ai/artifact/4zWdK2uc5du4u4ryRjERNS).

**Three tabs.**

| Tab | What it holds | When it's available |
| --- | --- | --- |
| Broadcast | Messages and file announces on D2G, beside the station list | Always |
| Session | The one session you're in: its messages, file offers, status strip, and Leave | Pops open when a session starts, whoever called; greyed out when there's no active session |
| Files | Every transfer, both directions, in sessions and broadcast | Always |

**The Session tab.** There is one tab, not one per session, because data2g-host runs one ARQ link at a time. It comes to the front by itself when a session starts, whether you called or were called. When the session ends it greys out: no typing and no sending. My suggestion is that the last session's messages stay readable there until the next session starts; they are in the log either way.

**The Files tab** has two panes, each ordered by date, newest first.

- **In progress and parts** (top): every transfer that isn't finished, including parts kept from earlier sessions. Columns: date, file, size, progress (bytes of total), From or To, state, and expiry.
  - From or To is the other station. For a file you broadcast, To says **Broadcast**. For a broadcast you're receiving, From is the sending station.
  - States: Waiting for answer, Moving, Stalled, Repairing (a broadcast share taking repair requests), and Part kept.
  - Expiry is when a kept part is deleted (7 days by default), or when a broadcast share's repair rounds end.
- **Completed** (bottom): finished transfers, with date, file, size, From or To, result (Saved, Sent, Declined, or Failed with the reason), and where it was saved.

**Right-click in Files.**

- **On a broadcast share you're sending,** the menu's first line, greyed, shows when its repair rounds end now: "Repairs open until 15:42 (1 h 10 min)". Below it: **Keep open 30 min longer**, **Keep open 2 hours longer**, **Keep open until...**, and **Close repairs now**. Keeping a share open costs no airtime unless someone asks for a repair. If you leave the station, answering repair requests falls under the automatic-control rules (97.221), so outside the automatic segments it answers only controlled stations, in 500 Hz modes.
- **On a kept part:** **Ask to send the rest** (offers to Call the station first), **Keep 7 days longer**, and **Delete part**.
- **On a transfer in progress:** **Cancel**.
- **On a completed transfer:** **Open**, **Open folder**, and **Remove from list**.

## Broadcast mode

The Broadcast tab is the first tab and never closes. It is where CQs, roundtables and broadcast files happen. It works much like Glissando's Data2G broadcast chat, minus the tempo art, but on its own broadcast group and protocol.

- **Speed** is the gearshift ("The gearshift", under Main window): the Gear button by Send shows the gear by its mode's short name, for example `Gear E2C` (`docs/GEARBOX.md`, "Mode names"), and opens the gearshift on the Submode Map.
- Each message shows Queued, Sending, Sent, or Not sent, with the same rules as now (the host's ACKMODE report, two minutes to give up). No acknowledgements in broadcast mode except for addressed messages, if we keep them.
- The station list on the left is fed by broadcast traffic: callsign, last heard, locator if sent, and whether it takes sessions. Double-clicking a station does nothing risky: it fills "@W1AW" or offers **Call**.
- Woah! becomes a plain **Pause sending** switch on the toolbar.

SNR is not reported over KISS, so the list shows last heard rather than signal strength, unless Andrew adds a report.

## File transfer

Session files keep the ideas that worked on the air (offer and answer, carrying on, a whole-file hash check), in the new protocol's records, with a clearer screen around them. Broadcast files use seal-driven repair (Signing, below) from the start. Offers appear where they happen, in the Session tab or the Broadcast tab, and every transfer, past and present, is in the **Files** tab.

**Sending.**

- In a session: drag a file onto the tab, or press **Send file...**. Several files queue in order, shown as a list you can reorder or remove before each starts. One moves at a time, as now.
- Outside a session, Send file to a station offers to **Call** first. There is no hidden session for a file.
- In broadcast mode: **Broadcast a file...**, with today's estimate box (airtime at the chosen speed, repair time, "stations will not confirm receipt") and the 300 KB cap.

**Receiving.**

- An offer is a card in the ragchew, not a red box over everything: "W1AW offers FRED.TXT, 7,000 bytes", with **Save**, **Save as...**, **Decline** and the expiry countdown. A desktop notification and the tab badge draw attention if the window is in the background.
- A broadcast file is a card in the Broadcast tab with **Receive** and **Ignore**, as now.
- The received-files folder and the auto-accept list carry over as Preferences.

**Progress.** One row per transfer, in the Files tab:

| Field | Shows |
| --- | --- |
| Bar | Sent and acknowledged, two shades, as now; broadcast files segmented by piece |
| Bytes | 4,096 of 7,000 |
| Rate | over the last 2 minutes, not since the start |
| Finish | "about 14:32 (13 min)", one item |
| State | Waiting for answer, Moving, **Stalled 1:20** (no bytes, link quiet), Leaving, Done, Failed with the reason |

**Carrying on.** Partial files are kept on disk next to the received-files folder, for 7 days, not only while the app runs. The same file from the same station resumes, as #105 does, and the card says "4,096 of 7,000 kept from before". A failed file's row in Files has **Ask W1AW to send it again** (a line in the ragchew, not a protocol change).

**History.** The Files tab's Completed pane is a persistent log (time, direction, station, name, size, result, saved path) with **Open folder**. It replaces the visi-scope's per-run history and the cat.

## Main window

One ordinary window in the system theme, laid out like a messaging app: connection bar on top, stations and sessions on the left, tabs in the middle, a status bar at the foot.

- **Connection bar:** data2g-host address and state (Connected, Reconnecting in 8 s), my callsign, broadcast group name (D2G), width (2400 Hz or 500 Hz, sending `BW2300`/`BW500`), **Accept sessions**, **Pause sending**, and a TX light from the host's `PTT` report.
- **Left pane:** Stations heard in broadcast mode, with the session partner pinned at the top while a session is up.
- **Right pane: the map** (your ask, 2026-10-22). Glissando's globe at the top, showing the picked station's path, with a caption in Glissando's form, `W1AW · FN31 · 3,951 km · 81°`, and the station's details below. The theme is new: light blue oceans (#A8D4F2), near-white land (#F7F5EE), and dark blue coasts and borders (#143068). The globe is lit from the sun's real position: the light source sits over the subsolar point, so the night side falls in shadow and the greyline shows as the soft edge between them. The twilight is blended from 3° above the horizon to 12° below it.
- **Globus on or off** (your ask, 2026-10-22). A toggle button labelled **Globus** sits where the map's heading was. It turns blue when the globe is showing, and when the globe is hidden it moves to the right end of the tab strip, so the Broadcast and Session messages can use the full width. View > Globus does the same. Beside the button, the time the globe is lit for, in UTC and local time: `22:36 UTC / 15:36 PDT`. The dividers between the station list, the tabs and the globe drag, and the client remembers where you left them. The globe also draws US state and Canadian province lines, thin and light, from Natural Earth simplified to about a quarter of a degree, so the whole set stays small.
- **Station list columns.** Call, Status, Heard, Locator, Distance, Bearing, Key (the seal badge) and Services, chosen from a Columns menu and sortable by any of them. SNR waits on a host report (Asks for Andrew). With the map shown, the narrow list keeps locator and distance under the last-heard time.
- **The gearshift** (your ask, 2026-10-22, built on the Data2G Submode Map you sent). One popup, opened from the width button on the connection bar or the Gear button by Send. The one-press 500 Hz button stays. Details under "The gearshift", below.
- **Auto ALC** beside TX Off: a small checkbox with a level meter that keeps transmit drive just under the rig's ALC. The client never touches audio, so this needs a host command to set the TX level. The client can read ALC through Hamlib on many rigs (Asks for Andrew).
- **Tabs:** Broadcast, Session and Files, always in that order (Stage 1). Session is greyed out when no session is active. A tab badge counts unread messages and waiting offers.
- **Status bar:** channel busy (`BUSY`), current mode, and the newest transfer in one line; clicking it opens Files.
- **Menus:** File (Send file, Files, Open received folder, Quit), Station (Call, Leave, Send file), View, Settings, Help. Every action has a keyboard shortcut.

**The gearshift.** The popup *is* the Submode Map: speed across (bits per second, log scale), the S/N each mode needs down the side, one dot per mode, coloured by modulation. The `ack-` modes are left out, because they carry the host's own ARQ control. Three choices sit above the map:

- **Width**, narrowest to widest: 200 Hz, 500 Hz, 1.2 kHz, 2.4 kHz. These are the map's four groups. A width is a cap, so 2.4 kHz still uses the narrow modes. Modes wider than the cap show as hollow circles.
- **Path**: Calm (no fading), Good, Poor or Disturbed (the ITU fading channels on the map). The path picks the gearbox, because modes rank very differently once the path fades: the polar modes jump up the order on a good fading path, and the QAM modes drop down it.
- **Auto**: shift to fit the path. The map draws the path's S/N, from the station's last few bursts, as a dashed line, and shades everything that needs more than the path gives, less a 3 dB margin. Auto takes the fastest gear above the shading.

**How the gears are chosen.** For each width and path, the client takes the best modes: those no other mode beats on both speed and S/N. That is 2 to 26 modes, too many to shift through. So gear 1 is the most robust mode, and each next gear is the most robust mode at least 1.8 times faster than the one below. Every shift roughly doubles the speed, like a real gearbox, and 2.4 kHz gets 8 gears on every path. The gears join into a heavy line on the map, so you can see what each step up costs in S/N.

| Gear | 2.4 kHz, Good path | 2.4 kHz, Calm path | 500 Hz, Good path |
| --- | --- | --- | --- |
| 1 | fsk16r25-r1/3 · 32 bit/s · −3.0 dB | fsk16r25-r1/3 · 32 · −12.3 | fsk16r25-r1/3 · 32 · −3.0 |
| 2 | polar-k96-f8 · 69 · −1.9 | fsk8r50-r1/2 · 72 · −10.1 | n4-qpsk-r1/3 · 88 · +0.1 |
| 3 | polar-k96-f4 · 139 · −1.2 | fsk32r62-r1/2 · 151 · −7.9 | n4-16qam-r1/3 · 176 · +2.1 |
| 4 | qpsk-r1/5 · 319 · +1.1 | qpsk-r1/5 · 319 · −4.9 | n10-qpsk-r1/2 · 333 · +5.7 |
| 5 | w48-qpsk-r1/5 · 639 · +4.1 | w48-qpsk-r1/5 · 639 · −1.6 | n10-16qam-r1/2 · 672 · +10.9 |
| 6 | w48-qpsk-r1/2 · 1611 · +8.4 | w48-qpsk-r1/2 · 1611 · +2.4 | n10-64l-r3/4 · 1544 · +18.9 |
| 7 | w48-16qam-r1/2 · 3278 · +13.8 | w48-16qam-r1/2 · 3278 · +7.6 | n10-256l-r3/4 · 2067 · +25.8 |
| 8 | w48-256l-r5/8 · 8222 · +25.8 | w48-256l-r5/8 · 8222 · +18.5 |  |

The gear list beside the map gives each gear's short name, Data2G mode, speed, its S/N on this path and how long 1 KB takes; gears the path can't carry (less the 3 dB margin) are red. The figures are the map's: 10% failure of the smallest ARQ burst, with S/N measured in 2500 Hz. Longer bursts need a little more. Glissando's five speeds sit near the Calm ladder, and two of them (fsk16r25-r1/3 and w48-16qam-r1/2) are gears on it. The new ladder replaces them.

The window opens at its last size and place, works at 1280×720, and uses the system font, so it reads well on a small laptop at the operating position. Dark mode follows the system.

## Pictures and voice, *later*

The SSTVAE pictures and RADE voice clips you decided on 2026-10-19 were already Data2G only, so they move to this client unchanged. The spec stays in its own doc, [Images and voice in chat](https://claude.ai/code/artifact/1ba7e70a-4807-49c0-b37d-9db535c6ea76), and this plan only leaves room for it:

- Session and broadcast timelines take **object cards** (a picture or a clip with its caption, sender and how many copies were stacked), beside text and file cards.
- The client will need its own microphone and speaker devices for voice clips. Today it needs no audio at all, because Data2G owns the radio sound card.
- The Part 97 notes in that doc apply: text in the data segment, pictures and voice on the phone side, and CW or spoken ID every 10 minutes.
- It waits on Andrew's latents spec and the "latents over KISS" extension in data2g-host. No work on it in the first versions.

## Signed, never secret: messages, pages and boards

The idea (yours, 2026-10-20): build the useful parts of Reticulum, but with every byte readable on the air and a signature added for authenticity, so Part 97's plain-text rule becomes a strength instead of a limit. I think it fits the client well, because sessions already give a reliable byte stream to carry it.

| Reticulum has | Our version | Encrypted? |
| --- | --- | --- |
| Identities (key pairs) | A signing key per station, bound to its callsign | No, signing only |
| Announces | A signed beacon: callsign, public key, locator, what the station offers | No |
| LXMF messages | Signed ragchew messages, delivered live or stored and forwarded | No |
| Nomad Network pages | Gemini-style text pages served from a station, signed | No |
| Propagation nodes | BBS stations that store and forward signed posts and mail | No |
| Encrypted links | Plain Data2G sessions | No |

**Why it should be legal.** Part 97.113(a)(4) bars messages encoded to obscure their meaning. A signature hides nothing: the text stays readable as sent, and the signature is a published algorithm anyone can check with a published key. That is the common reading, not an FCC ruling, and two rules keep it honest: the signature never carries anything but a signature, and the format and keys are public. The CW or voice ID rules still apply; a key is not a legal ID.

**Signing.**

- Ed25519: a 32-byte public key and a 64-byte signature, well tested, small.
- What is signed: callsign, time, sequence number, recipient (or broadcast), and the text. Time and sequence stop someone replaying an old message.
- ~~In a session, every message is signed; 64 bytes is cheap there.~~
- The originating station sends a **seal** at the **s****e****al inter****val****,**** whi****ch is less ****than ****e****v****er****y ****t****en mi****nutes an****d is config****urable****,**** **along with its CW ID. This seal is a table of contents of its messages since the last seal, a digest of each (details below), and is signed to ensure authenticity.
- On screen: a small badge per message, Sealed (key known), New Identity (first time seen), Identity Changed (warning), or Awaiting Seal.
- **Seal icons** (mock-up board "Seal state icons"). Each is a wax-seal rosette with a glyph inside, so the shape carries the meaning without colour. A dashed edge means still waiting, and a solid fill means look at this. Sealed is a green check, New Identity a blue star, Identity Changed a solid orange "!", Awaiting Seal a grey dashed edge with a clock, and Unsigned (your pick, 2026-10-22) a caution-yellow seal with a dark slash. Still proposed, for you to keep or drop: Receipted (a double check on your own messages), Repair Asked (amber, dashed, with a turning arrow, for a gap a seal revealed) and Mismatch (a solid red X: the digest doesn't match, so the text is struck through).

### Seals, receipts and repairs

The answer to your question (2026-10-21): a station seals what it **sent**, never what it thinks was received, and the other station answers with a signed **receipt**. Each statement stands on its own and names the other by digest, so nothing has to stay in step, and a lost ack costs nothing but a repeat.

Why split them. Sealing only the acknowledged messages ties authenticity to delivery, and delivery is the one thing a fade makes uncertain. That is your synchronisation problem, and it can't be fully solved: the last ack before a fade may or may not have arrived (the two generals problem). Authenticity doesn't need it solved. "AG7EW sent #41 to #44" is true whatever B heard.

**Each message** **carries a sequence number** (3 bytes) in the session. Nothing else is added to it.

**A seal** is a signed table of contents:

| Part | Size |
| --- | --- |
| Callsign, session id, seal number | about 16 bytes |
| Digest of the previous seal (chains the seals into one log) | 12 bytes |
| First sequence number, then a digest per message sent since the last seal | 3 + 12 bytes each |
| The latest receipt for the other station's seals (below) | about 16 bytes |
| Ed25519 signature over all of it | 64 bytes |

Twenty messages make a seal of about 350 bytes, a few seconds even at slow session speeds. A receiver checks the signature once, then checks each message it holds against its digest. A message it lacks shows up as a gap ("#43 sealed, not received"), so the seal doubles as loss detection, and this is subsequently mutually requested at the end of a sealing interval. A message resent later is still covered by the original seal; nothing is signed twice.

**A receipt** is your acknowledge bits, made exact: "of seal 7 (its digest), I hold these", as one bit per message in that seal's table. A count alone can match while the sets differ (one message missed, another counted twice); a bitmap tied to the seal's digest can't. The receipt rides in the receiving station's own next seal, so it is signed too.

**Lost acks heal by repetition, not by handshake.** Every seal carries the sender's latest receipt state, cumulatively, as TCP carries its cumulative ack. If A misses B's receipt, B's next seal carries it again. If B missed seal 7 itself, its receipt says so and A resends the seal. Once one seal gets through each way, both stations hold the same signed picture: what A sent, what B holds, and that each saw the other's statement. No acks of acks.

&#91;embedded content: seal and receipts through a fade · two stations\]

A loses nothing by missing B's receipt: B repeats its receipt state in every seal until A answers it.

**The seal interval.** At most every 10 minutes, sent with the CW ID as you suggested, plus one at **Leave** so a session never ends unsealed. Two optional extras: a seal at each over (when a station stops sending and waits for the reply), which keeps "waiting for seal" short in a lively ragchew; and one when a file finishes, sealing the file's SHA-256.

**Inside a Data2G session** the modem's ARQ already resends lost bytes, in order, so gaps are rare. They come at the edges: bytes in flight when the link is lost, and the last receipt before a fade. Those are exactly the cases the receipt state settles on the next session. The modem's own `BUFFER` count tells A which bytes B's modem took; the receipt is B's signed word that its client holds them.

**At the end of a session** both stations hold a mutually signed transcript: what each sent, and what each confirmed holding.

**In broadcast mode the seal drives repair** (your idea, 2026-10-21). After a seal, the broadcasting station stays sensitive to requests for anything that seal covers, ragchew messages and file pieces alike. Order, integrity and completeness then come from one mechanism, and the election rounds of today's broadcast file protocol go.

- **One numbered stream.** Everything a station broadcasts carries its sequence number: ragchew messages, file announces and file pieces (pieces also keep their number within the file).
- **What the seal lists.** A 12-byte digest per ragchew message, and each file once: its id, size, piece count and whole-file SHA-256 (12 bytes). Not a digest per piece: a 64 KiB file in 220-byte pieces would add 3.6 KB to the seal. Data2G's own coding catches damaged pieces, and the sealed whole-file hash proves the result.
- **When a seal goes.** At the seal interval with the CW ID, and also whenever the station stops sending for a while: the end of an over, or the end of a file's stream, so repair starts at once instead of up to 10 minutes later.
- **Asking.** A listener missing anything sends one **Repair request**: the seal's digest, a bitmap of the messages it lacks, and a piece list per file. It is the session receipt, sent only when something is missing.
- **No slots, no one first in line.** After its seal the station leaves a listening window. A listener waits a random time within it, and drops its own request if it hears another that already asks for what it lacks. data2g-host's wait for a clear channel does the rest.
- **Answering.** The station resends the union of everything asked, in sequence order. All of it is still covered by the original seal, so nothing is signed again. It opens another listening window, and stops after one with no requests, or when the seal leaves its sensitivity window.
- **Late joiners.** Each seal names the one before, so a listener that missed a seal sees the gap and asks for it by number. Requests are answered within the sensitivity window (below).

Precedent: NACK-Oriented Reliable Multicast (NORM, RFC 5740) repairs multicast the same way, with random backoff and request suppression. That is from memory, not rechecked.

**Seal** and **Repair request** are record types of the client's own broadcast protocol.

Open choices for you:

**Windows in Data2G terms.** Both windows follow the Data2G mode the station broadcasts in, using the airtimes data2g-host reports in its `MODES` list, so they fit any mode the host offers, the 500 Hz ones included:

- **Listening window** = 3 × the airtime of a one-codeword burst in that mode, + 4 s. Room for a few requests and the channel turning round.
- **Sensitivity window** = the last 3 seals, and never less than 4 × the airtime the sealed items took in their mode, up to 2 hours. A slow mode is listened to for longer, in proportion.

| Data2G mode | One-codeword burst | Listening window |
| --- | --- | --- |
| `fsk16r25-r1/3` | 16 s | 52 s |
| `fsk8r50-r1/3` | 11 s | 37 s |
| `fsk32r62-r1/3` | 6 s | 22 s |
| `qpsk-r1/3` | 3 s | 13 s |
| `w48-16qam-r1/2` | 2 s | 10 s |

Burst airtimes are data2g-host's at commit 8ffe15c, as measured for Glissando's Data2G mode; the client reads the live values. Open for you: the multipliers (3 and 4) and the 2-hour cap.

### Return receipts

Any item can ask for a return receipt (your idea, 2026-10-21): a ragchew message, a file, a page, a board post or mail, a radiogram. The addressee answers with the item's digest, signed, and that alone proves it arrived intact. It takes the place of chain of custody.

- **Asking.** One flag in the item's record, "receipt requested". It is covered by the sender's seal like the rest of the item.
- **The receipt.** Sent once the addressee holds the whole item and its digest checks: the item's 12-byte digest, the requester's callsign, the time it was received, and the addressee's signature. About 90 bytes.
- **Any route.** In a session it goes straight back. In broadcast mode it goes out as a broadcast record addressed to the requester. Through a board it is stored and forwarded like any post, so a radiogram's receipt can come back days later.
- **Who answers.** Only the station the item is addressed to. A broadcast item names who should answer, so a CQ with a receipt request doesn't bring back a flood. An automatic endpoint answers as itself: "received by AG7EW:mail" says the mailbox has it, not that the operator has read it.
- **On screen.** The sender's copy shows **Receipt from W1AW, 14:32, verified**, or **Receipt requested, none yet**. The addressee chooses in Settings whether receipts go automatically (the default), after asking, or never.

**Cheaper by riding in seals.** A station that owes several receipts and has a seal coming soon puts them in that seal as a list of (requester, digest, time), about 20 bytes each instead of 90, under the seal's one signature. A standalone signed receipt goes only when no seal is due soon, or when the receipt travels through a board. Either way the requester holds the addressee's signature over the exact digest it sent, which it can show to anyone later.

Not to be confused with a session's receipt (above): that one is automatic, covers a whole seal, and only tells the other station what to resend. A return receipt is per item, asked for on purpose, and travels any route.

Digests are SHA-256 truncated to 12 bytes.

**Trusting a key.** The same choice Gemini made for its certificates: trust on first use. The first key heard from W1AW is remembered; a change later is a loud warning. On top of that, you can mark a key Checked after comparing its fingerprint (shown as a few words) over the air, on QRZ, or on a QSL card. A stronger option to look into: ARRL's Logbook of the World already issues callsign certificates to verified licensees, and one could vouch for a station's key. I haven't checked whether ARRL allows that use.

**Pages (Gemini-style).** Each station can publish a few text pages: who it is, station details, net schedules, notes. The format is Gemini's gemtext, plain lines with headings and link lines like `=> KC2G/nets`. You open a page by calling the station; a session request fetches it, and the page arrives signed. Because the signature travels with the page, a copy cached and passed on by another station is still provably KC2G's. Limits per page (say 8 KB) keep airtime sane.

How the page systems compare (from memory; their current specs not checked):

|  | Gopher (1991) | Gemini (2019) | Nomad Network | Our pages |
| --- | --- | --- | --- | --- |
| Page format | Typed menus, plus plain text files | gemtext: headings, lists, quotes, preformatted blocks, links on their own lines | Micron: colours, alignment, inline links, forms | gemtext, plus simple forms for boards |
| Input | Search items only | One-line prompt | Form fields sent back to the node | Prompt and small forms (callsign, subject, post) |
| Generated pages | Server side, rare | Server side (CGI-like) | Scripts on the node | Generated by the station's software (board lists, what's new) |
| Addressing | Host, port, selector | URL with a hostname | Reticulum destination hash | Callsign and path, `KC2G/nets` |
| Transport | Plain TCP | TLS required | Encrypted Reticulum links over LoRa, packet radio or IP | Plain Data2G session |
| Authenticity | None | Server certificate, trusted on first use; client certificates for identity | Signed announces; the link proves who served it | Each page signed by its station's key |
| A copy relayed by someone else | Can't be checked | Can't be checked | Can't be checked | Still provably the author's |
| Legal on HF under Part 97 | Yes, but no authenticity | No, encrypted | No, encrypted | Yes, plain text with signatures |

The last two rows are the point: signing the page itself, not the connection, is what makes cached and relayed copies trustworthy.

**Links.** A gemtext link line is `=> <url> label`, and the URL may use any scheme. `AG7EW:board` can't be a URL as it stands, because in a URL the part after `:` is read as a port number. So the service goes first in the path, and `AG7EW:board` stays the label shown on screen:

```
=> d2g://AG7EW/board/topics/antennas  Antennas
=> topics/antennas  (relative, from a board page)
=> d2g://KC2G/pages/nets  KC2G's net schedule
```

`d2g` is a placeholder scheme name; the real one is yours to pick. Posting is the weak spot: Gemini's input is a one-line prompt, enough for a subject or a short reply, so longer posts need an upload record of our own (Gemini's add-on upload protocol, Titan, is a precedent).

**Bulletin boards.** A station can run as a board: topics, posts, "what's new since my last visit", and mail for a callsign. Mail is readable by anyone, as all amateur traffic is. Boards forward to each other overnight, like the old packet BBS networks, and signatures keep each post provably its author's however many hops it took. An unattended board falls under the automatic-control rule, 97.221 (checked 2026-10-21). On HF it may run freely in the automatic-control segments listed under Frequency plans. Elsewhere in the HF data segments it may only answer a station under local or remote control, at 500 Hz or less, which the 500 Hz mode setting already supports. On 6 m and up it may run anywhere data is allowed.

**Automatic endpoints** (your idea, 2026-10-20). A callsign is the identity, and a named service under it is an endpoint run by the station's software, the way Reticulum puts an aspect (`lxmf.delivery`, `nomadnetwork.node`) under an identity. Shown as `AG7EW:board`.

- **Not `/`.** A slash already means portable or a foreign prefix (AG7EW/P, VE7/AG7EW, /MM), and Data2G takes only A–Z, 0–9, `/` and `-` in a call, up to 10 characters.
- **On the air,** the session connects to plain AG7EW, and the first record in it names the service. data2g-host needs no change, and one radio serves both the operator and the station's machines. No service named means the operator.
- **Precedent:** AX.25 SSIDs, such as Winlink's -10 for gateways. Names read better than numbers.

| Endpoint | Does |
| --- | --- |
| `AG7EW:mail` | Takes signed messages while the operator is away; the operator reads them later |
| `AG7EW:board` | The station's bulletin board |
| `AG7EW:pages` | The station's signed pages |
| `AG7EW:echo` | Sends back what was sent, with the speed and mode the session reached: a station and path check |
| `AG7EW:report` | Replies with how it heard the caller: a two-way reverse beacon on request |
| `W7XYZ:net` | Collects and lists check-ins during a net |

- **Delegated keys.** The operator's own key signs a short delegation: "this key may speak for AG7EW:board, until this date". A compromised always-on board computer can then sign as the board, but never as the operator.
- **Marked automatic.** Messages from an endpoint carry its service name in what is signed, so the screen can say a machine replied, which fits the 97.221 automatic-control rule. A board passing on other people's mail is sending third-party traffic under 97.115 (checked 2026-10-21). Third-party traffic under automatic control is allowed for data emissions (97.115(c)). It may go to any US station, but abroad only to countries with a third-party agreement or for emergencies (97.115(a)), so a board has to check the destination country before forwarding.

**Radiograms.** A radiogram is a fixed form (preamble, address, text, signature), so it gets its own record type with a gemtext rendering for reading, not a free-form page. Signing adds two things to traffic handling, and a return receipt (below) confirms delivery. There is no chain of custody: the originator's signature already proves the text, however many hands it passed through.

- **End-to-end integrity.** The station of origin signs the message, so any relay or the delivering station can prove the text arrived unchanged. That goes well beyond the word-count check, which stays so a message can still leave by voice or CW.
- **Traffic drops.** `d2g://W7XYZ/nts` lists traffic waiting for an area as gemtext links, and a station picks up its traffic in a session.

Two caveats. A signature proves which amateur originated or handled the message, not the member of the public who wrote it, and that traffic is third-party traffic under 97.115. And it would need the NTS digital folks' buy-in to fit how they work.

### Store and forward between nodes

A node is a station running the board and mail endpoints unattended. Nodes pass signed items to each other in scheduled sessions, so mail reaches a station that is off the air and bulletins reach every node that wants them. No node can alter an item, because each one carries its author's own signature.

**Stored items stand alone.** A live ragchew is covered by seals, but an item that travels through nodes leaves its stream behind, so it carries its own signature (64 bytes). Every stored item has:

| Field | Purpose |
| --- | --- |
| Origin callsign and origin sequence number | Its identity everywhere; nodes drop duplicates by it |
| Kind | Mail, board post, radiogram, page update, return receipt, key notice |
| Addressee or topic | A callsign (mail, radiogram) or a board topic (posts) |
| Precedence | Emergency, Priority, Welfare, Routine (the radiogram scale), sent in that order |
| Expiry | Set by the author: mail 14 days, posts 30 days by default |
| Hop limit | Starts at 6; a node passing it on counts it down (not signed, so it can change) |
| Body and the author's signature | Huffman-coded text or a file; the signature covers the UTF-8 or the file bytes |

**How two nodes sync.**

1. Node A calls node B for a session, on a schedule each operator sets (for example 0300 and 1500 UTC on 30 m).
2. Both send a **summary**: for each origin with traffic in the last 30 days, the highest sequence number held without gaps, plus any gaps. A few bytes per active station, so the summary stays small even with hundreds of stations.
3. Each asks for what it lacks and is willing to carry: mail and radiograms it can deliver or pass toward their addressee, posts in topics it carries, nothing expired.
4. Items go both ways in precedence order. The session's seals and receipts confirm the handover.
5. Both update their summaries. The next sync starts from there, so a lost session costs only a repeat.

This is the Usenet idea ("I have, send me") made compact by sequence numbers, so nodes never send long lists of digests.

**Where mail goes.** A station's signed announce names its home nodes, up to three, the nodes it checks for mail. Mail flows toward them:

- A node that is itself a home node for the addressee keeps the mail until the addressee collects it.
- Otherwise, version 1 floods: mail goes to every neighbour node within its hop limit, and duplicates are dropped. Ham networks are small, so flooding is simple and robust. Routes that send mail only toward the home node can come later.
- The addressee calls any node that holds its mail (`N7XYZ:mail`), and its return receipts flow back the same way. Once a node sees the receipt, it deletes its copy.

&#91;embedded content: mail through two nodes · receipt back\]

The arrow from W1AW to its home node is W1AW calling in to collect; the mail never pushes itself onto W1AW's radio.

**Posts** flood to every node carrying their topic, Usenet style.

**Abuse and the rules.** Nodes accept only items signed by a known key, and rate-limit each callsign. No proof of work is needed, because a callsign is accountable. The message-forwarding rule, 97.219 (checked 2026-10-21), fits this well. The originating station is primarily accountable for its message (b). Later forwarding stations are not accountable for violations they pass on inadvertently, but must stop once they become aware of them (c). The first forwarding station must either authenticate the station it accepts traffic from, or accept accountability for what it passes on (d). A signature from a known key is that authentication, and a node's "stop forwarding" control covers (c). Unattended node-to-node sessions fall under 97.221: freely in the HF automatic-control segments, elsewhere on HF only when answering a controlled station at 500 Hz or less, and anywhere data is allowed on 6 m and up.

### Cost

On HF the scarce thing is airtime, so cost is measured in **seconds of airtime per kilobyte**, not hops. One measure then does four jobs: routing, budgets, spam control, and telling the operator what a send will take.

**Measuring a link.** Each node keeps a cost for each neighbour from its own sessions with it: airtime used divided by bytes acknowledged, read from data2g-host's mode and `BUFFER` reports, then divided by the share of scheduled syncs that actually connected. A link that runs `qpsk-r1/3` reliably costs little; one that only connects at night on `fsk16r25-r1/3` costs a lot. It's a moving average over the last 2 weeks, so it follows the seasons.

**Routing by cost.** Nodes put their neighbour costs in their signed announces ("N7XYZ: 12 s/KB on 30 m"). Each node then works out the cheapest path to every home node, the way distance-vector routers do. Version 1 floods; version 2 sends mail only along the cheapest path, and falls back to flooding when no path is known. Reticulum counts hops; on HF one easy hop at 2 s/KB can beat one hard hop at 60 s/KB. In practice the curmudgeons, nodes with strong, reliable paths to each other, end up carrying the long-haul traffic, mostly among themselves.

**Budgets.** Each node operator sets an airtime budget, for example 30 minutes a day per band. Items go in precedence order within it: Emergency always, then Priority, Welfare and Routine while the budget lasts. Routine traffic over budget waits for the next day.

**Spam control.** Reticulum's LXMF asks senders for a proof-of-work "stamp" whose cost the recipient sets. With callsigns there's no need to burn CPU. Instead each node gives each callsign an airtime quota (say 5 minutes a day of Routine traffic through it), and the signature proves whose quota a message counts against. Emergency traffic is never held by a quota.

**On screen.** Before a send, the client shows what it will take: "About 40 s of airtime here; about 3 min across 2 nodes to reach W1AW's home node". Board posts show how far they'll spread.

### Send now, or wait for a better path

Your point (2026-10-21): HF paths are variable but predictable. AG7EW reaches Andrew on 17 m most afternoons and almost never on 17 m after 6 PM at this time of year. So a link's cost depends on the band and the hour, and sometimes the cheapest choice is to hold traffic for a better window tonight than to fight a fading path now.

**A path table, per neighbour.** Cost in seconds of airtime per kilobyte, for each band and each hour of the day, with the chance that a session connects at all. Learned from three kinds of evidence, best first:

1. **Our own sessions** with that neighbour: what they actually cost, by band and hour. A moving average over about 2 weeks, so it follows the season.
2. **Hearing the neighbour passively:** its broadcasts and announces on `D2G`, decoded by our own data2g-host, with the mode and band they came in on.
3. **Clues about the path:** FT8 signals our station decodes from the neighbour's grid area (read from WSJT-X's UDP reports, if it runs), WWV and WWVH on 5, 10 and 15 MHz, and the NCDXF/IARU beacons (14.100, 18.110, 21.150, 24.930 and 28.200 MHz, a known 3-minute cycle). These say whether a band is open toward that area tonight, even with no traffic of our own.

For a new neighbour with no history yet, a propagation prediction from the solar flux and K index can stand in until real data replaces it.

**Impatience.** Each item gets an impatience λ, the airtime it's worth spending to save an hour of waiting:

| Precedence | Impatience | In practice |
| --- | --- | --- |
| Emergency | Unlimited | Goes now, on the best band open now, however hard the path |
| Priority | High | Waits only if a much better path opens within the hour |
| Welfare | Medium | Waits a few hours for a clearly better window |
| Routine | Low | "By tomorrow is fine": waits for the best window in the next 24 hours |

A sender can also set a deadline ("deliver by 0800"), which caps the wait whatever the precedence.

**The choice.** For each item and each next hop, the node picks the band b and start time t that cost least overall:

```latex
\min_{b,\,t}\;\left[\frac{\text{airtime}_b(t)}{p_b(t)} + \lambda\,(t - t_{\text{now}})\right]
```

Here airtime\_b(t) is the expected airtime for the item on band b at hour t, p\_b(t) the chance the session connects, and λ the item's impatience. Dividing by p\_b(t) counts the failed attempts. With λ unlimited only t = now is allowed; with λ small, the best window of the day wins.

**What it changes.**

- **Node schedules plan themselves:** each pair of nodes syncs in the windows their path table rates best, and Emergency traffic triggers a sync at once.
- **The sender sees the plan:** "Routine: will go at 2140 on 80 m, about 1 min of airtime (now would take about 6 min on 17 m). Send now anyway?"
- **Band changes:** an unattended node moving between bands needs rig control. On HF it has to stay inside the automatic-control segments, or outside them work only under 97.221(c): answering a controlled station, at 500 Hz or less.

### Links: radio, packet and IP

A link is one way to reach one neighbour node. A node can have several links to the same neighbour, for example an IP link and a 30 m link to N7XYZ, and routing treats each as its own path with its own cost. Every link carries the same signed records. Only the framing and the timing differ.

| Link | Reaches | Timing | Cost |
| --- | --- | --- | --- |
| Data2G on HF | Distant nodes | Scheduled syncs in the path table's best windows | Airtime s/KB from the path table |
| AX.25 KISS on VHF or UHF | Local nodes | Scheduled, or on demand | Airtime s/KB, usually low |
| TCP over the internet | Any node with an address | Always connected; items offered as they arrive | Near zero, but see the RF rule below |
| TCP over a ham mesh (AREDN) | Mesh nodes | Always connected | Near zero; it is radio, so Part 97 applies in full |

**Defining an IP link.** In Settings, one entry per neighbour:

- **Neighbour:** its callsign. Its key comes from its announce, or the operator pins it here.
- **Address:** host name or IP address, and port. One side listens and the other dials. Both may dial, and the first connection to come up wins.
- **Network:** Internet or Ham mesh. On a mesh link the client refuses TLS, because the bits travel over amateur radio and 97.113(a)(4) bans obscuring them.
- **Carries:** everything, everything but files, or Emergency only.
- **To radio:** whether items that arrive over this link may go out on the air (the RF rule, below).

**Running an IP link.**

- **Handshake:** each side sends its hello with a random nonce, then signs the other side's nonce, both callsigns and the time. That proves each end holds the key for its callsign, and it is all the trust a link needs.
- **Sync:** the same summaries and fetches as an HF session. Then the link stays open and each new item is offered at once ("I have"), so internet-connected nodes stay in step within seconds.
- **Keeping it up:** a keep-alive every 60 s. On a drop, retry after 10 s, doubling up to 15 minutes.
- **Framing:** length-prefixed records with the same coding as on the air, so a captured TCP stream reads exactly like a captured radio session.
- **No TLS by default.** Signatures already stop forgery, and privacy isn't a goal. Operators of an internet-only link may turn TLS on, since that link is outside Part 97. A mesh link never can.
- **Status:** the node's status page lists each link: up or down and since when, items each way today, and the queue waiting for it.

**The RF rule.** Internet links add a new risk. Anyone on the internet can make a key and claim a callsign, while on the air, trust on first use is grounded in actually hearing the station. So:

- A key first learned over the internet counts as **not heard on air** until the station is heard on the air, Checked by the operator, or vouched for (Logbook of the World, later). Items signed by such a key travel over IP only, and no node transmits them.
- Each node operator chooses whether internet-borne items go out on radio at all: Yes, Emergency and Priority only, or No. The node is the first station to put them on the air, which makes it the "first forwarding station" of 97.219(d), so it is that operator's call.
- An author can mark an item **Radio only**, and then no node passes it over the internet. Winlink's radio-only addressing does the same (from memory). It's useful for drills, and for proving an RF path works.

### Other store-and-forward gaps

- **Where to find a node.** By checking in to the D2G net (above). Check-ins, signal reports and the rare announces carry each node's links: its IP address if public, and for HF the band, frequency and listening schedule or scan list. The directory endpoint covers nodes out of earshot.
- **Large items.** Items over 4 KB move in pieces with resume, reusing the session file engine, so a fade doesn't restart them. Nothing over 300 KB goes on HF at all (your limit, 2026-10-21). By rough estimates from Data2G's MODES airtimes, 300 KB takes about 35 minutes on w48-16qam-r1/2 and about a day on fsk16r25-r1/3, so before any send over 10 minutes of airtime the client shows the estimate and asks. Within the cap, each node sets the largest item it will relay unattended on HF, 32 KB by default. A relayed item costs airtime at every hop, and in version 1 at every node it floods to, and unattended answering outside the automatic segments is limited to the slower 500 Hz modes. Bigger items go directly in a session, or travel over IP or VHF.
- **Storage.** Each node sets a disk limit. When full, it drops Routine before Welfare before Priority, soonest expiry first, and never drops Emergency. A copy is kept until it expires or the addressee's return receipt passes through, whichever comes first.
- **Gaps that aren't gaps.** A node that doesn't carry a topic still sees that topic's sequence numbers go by. It records them as "seen, not wanted", so its summaries don't list them as missing forever.
- **Cancel.** An author can send a signed cancel for an item it sent. Nodes drop the item and pass the cancel on. A node operator's own "stop forwarding" (97.219(c)) is local, and drops the item at that node only.
- **Clocks.** Expiry, quotas and schedules need each node's clock right to within a minute or so, from GPS or NTP. The status page warns when it drifts.
- **Running a node.** The status page shows the queue, each link, today's airtime against the budget, and blocked callsigns. In version 1, a node is managed by its operator in person, over IP, or by the operator's own remote control, not with commands sent over the air.

### Frequencies and band plans

Your call (2026-10-21): the client starts from Part 97's automatic-control sub-bands, and the operator picks frequencies inside them; the same approach extends to VHF, UHF and microwave with the recommended band-plan segments.

**A band plan file, not code.** The client reads a plain data file per region: for each band, its segments, and for each segment what the rules allow and what the band plan recommends.

| Field | Example |
| --- | --- |
| Range | 14.0950–14.0995 MHz |
| Automatic control | Allowed / only when answering a manually controlled station / no |
| Bandwidth limit | 500 Hz for answering-only automatic operation on HF |
| Emissions and licence classes | Data; General and up |
| Band-plan use | Digital, weak signal, packet, mesh, beacons (recommended, not law) |
| Source | The rule or band plan it came from, with its date |

The US file ships first, checked line by line before release. Its order of authority (your call, 2026-10-21): Part 97 first; where Part 97 says nothing, the ARRL band plan; then, for VHF and up where the ARRL defers to them, the regional frequency coordinators. Each segment records which of these it came from. Other regions are files contributed later. Nothing in the code knows a frequency.

**How the operator uses it.** In Settings, each node operator sets a frequency, or a range to pick from, for each band the station uses. The client allows only frequencies whose whole signal (dial plus the mode's audio passband) sits inside one permitted segment, and shows why when it refuses: "Automatic control is allowed only in 14.0950–14.0995 and 14.1005–14.1120 MHz." Attended operation keeps the full data segments; unattended operation narrows to the automatic segments. The radio stays the source of truth: the client tunes only for a scheduled unattended sync, and only to a frequency the operator chose.

**HF automatic-control segments**, from 97.221(b), checked against the current eCFR text on 2026-10-21 (every line matched):

| Band | Automatic data allowed |
| --- | --- |
| 80 m | 3.585–3.600 MHz |
| 40 m | 7.100–7.105 MHz |
| 30 m | 10.140–10.150 MHz |
| 20 m | 14.0950–14.0995 and 14.1005–14.1120 MHz |
| 17 m | 18.105–18.110 MHz |
| 15 m | 21.090–21.100 MHz |
| 12 m | 24.925–24.930 MHz |
| 10 m | 28.120–28.189 MHz |

97.221(c) allows unattended data on any other HF frequency where data is authorized, on two conditions. First, the station is responding to interrogation by a station under local or remote control. Second, no transmission is wider than 500 Hz. The 60 m channels (97.303(h)) are excluded. That fits the 500 Hz setting: a node answering callers can use the wider data segments only in 500 Hz modes, and never on 60 m. Every HF data signal is also capped at 2.8 kHz (97.307(f)(3)), which all Data2G modes meet.

**VHF and up.** 97.221(b) allows automatic control of data on 6 m and every shorter band, so there the band plan, not the law, picks the frequency. Part 97 still sets the width (97.307(f)), and where Part 97 is silent the ARRL band plan is the next reference, as you said. Both checked 2026-10-21:

| Band | Part 97 data limit | ARRL data segment |
| --- | --- | --- |
| 6 m | 19.6 kbaud, 20 kHz ((f)(5)) | 50.6–50.8 MHz digital; 50.62 packet calling |
| 2 m | 19.6 kbaud, 20 kHz ((f)(5)) | 145.01–145.09 MHz packet |
| 1.25 m, 219–220 MHz | Data only, 100 kHz ((f)(13)) | None listed |
| 1.25 m, 222–225 MHz | 56 kbaud, 100 kHz ((f)(6)) | 223.52–223.64 MHz digital and packet |
| 70 cm | 56 kbaud, 100 kHz ((f)(6)) | None nationally; left to local coordinators |
| 33 cm | Any digital code ((f)(7)) | 909–927 MHz broadband |
| 23 cm | Any digital code ((f)(7)) | 1248–1252 and 1297–1300 MHz digital |
| 13 cm | Any digital code ((f)(7)) | 2410–2450 MHz broadband and 802.11 (AREDN) |
| 9 cm | Any digital code ((f)(7)) | 3360–3400 MHz OFDM and broadband (3.45–3.5 GHz was withdrawn on 2022-04-14) |
| 5 cm | Any digital code ((f)(7)) | Analog and digital segments within 5650–5925 MHz |
| 3 cm | Any digital code ((f)(7)) | Analog and digital segments |

The client recommends these segments first. Wider channels open up there, and the transport layer (Still missing, below) is how they get used: Data2G through an SSB or FM radio, AX.25 packet, or IP over a microwave mesh like AREDN. On 70 cm the US file needs each regional coordinator's plan, since the ARRL leaves it to them.

Sources, checked 2026-10-21: Part 97 on eCFR, sections [97.115](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-B/section-97.115), [97.219](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-C/section-97.219), [97.221](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-C/section-97.221), [97.305](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.305) and [97.307](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.307), and the [ARRL band plan](https://www.arrl.org/band-plan). Not yet checked: whether 97.307 caps the width above 902 MHz, and the regional coordinators' plans.

### Still missing, compared with Reticulum

With store and forward and cost in place, these are the gaps left. My recommendation for each is in the last column.

| Reticulum has | Our spec today | Recommendation |
| --- | --- | --- |
| Many interfaces: LoRa, packet radio, TCP, serial | Data2G on HF only | A transport layer under the protocol: Data2G for HF, plain TCP between nodes over the internet, AX.25 KISS for VHF packet later. Same signed items on all of them |
| Live multi-hop links through transport nodes | One hop: sessions are station to station | Keep it that way on HF. Multi-hop is store and forward, which suits HF's delays |
| Key rotation and recall | Trust on first use and a Key changed warning | A **key notice**: the new key signed by the old one; or, if the old key is lost, vouched for by two stations you've marked Checked. Nodes carry notices like any item |
| Announces propagated through the network | A signed beacon, heard directly | Nodes store and pass on announces (callsign, key, home nodes, endpoints, link costs), so a directory builds up: `N7XYZ:directory` answers "who is W1AW, and where does it get mail?" |
| One identity on several devices | One station, one key | The same key on several machines, with a device number in the sequence numbering, so two machines never reuse a number |
| Clock-free replay protection | Time in every record | Sequence numbers do the real work; times are only checked within a day, so a station with a drifting clock still gets through |
| Groups | Board topics and the broadcast group | Enough for now: topics flood between nodes, and nets have their endpoint |

Two things a ham network can do that Reticulum can't, worth having early:

- **Logging.** Every session and its messages can go to an ADIF log, with the signed transcript as proof of the contact.
- **Emergency use.** Precedence goes all the way through, from the send button to node budgets, and radiogram and ICS-213 forms are record types of their own.

**What it needs from Data2G.** Nothing new for signing, pages or boards: it all rides inside sessions and broadcast mode. A board serving several callers in turn would want to know how long data2g-host's sessions take to set up and tear down.

## On the air

The client speaks a new protocol of its own, designed from scratch (your call, 2026-10-21). It does not try to talk to Glissando.

- **Broadcast group:** `D2G` (your choice, 2026-10-21), not GLISS, so the two programs never read each other's frames.
- **Messages:** UTF-8 text records, with the sender's callsign, a sequence number and the time in every one, so a listener who fades in mid-stream knows who it is. The text is coded with Glissando's ham-table Huffman code (your choice, 2026-10-21): about 5 bits a character instead of 8, its 42 common phrases as one symbol each, and anything outside printable ASCII as an escape and the raw byte. The table is fixed, so every record still decodes on its own. Records carry a table version, so the table can grow later. Seals and receipts hash the UTF-8 text, not the coded bits, so a new table never breaks a signature. The coding is lossless and the table is published in the spec, so it hides nothing (like Varicode or Baudot), and the exact UTF-8 bytes typed are what gets signed, with no Unicode normalisation or line-ending changes, so both ends hash the same bytes.
- **Sessions:** a hello record first, naming the protocol version and the records each end takes, then message, file, seal and receipt records.
- **Files:** session files keep the ideas that worked on the air (offer and answer, carrying on from a kept part, a whole-file hash check), written in the new records. Broadcast files use seal-driven repair from the start.
- **Commands:** the same command-port lines Glissando uses, because those are Data2G's own protocol (`MYCALL`, `CHAT ON`, `LISTEN`, `BCAST OPEN`, `BCAST MODE`, `BW500`/`BW2300`, `MODES`, `CONNECT`, `DISCONNECT`, `ABORT`).

**Reusing code.** What carries over is the logic and the tests of the socket-free engines: KISS and command-port handling (`Data2GLink`), the session file engine, SHA-256, parts of the broadcast engine, and the Huffman coder (`HamText`, with its phrase table and generator), all adapted to the new records. `HamText`'s block size changes from Glissando's 9-byte segments to the new records. Glissando's packed frame headers (`FrameCodec`) are not needed, and they were the part carrying FreeDV contributors' work. Everything that carries over was written in this project, apart from one commit by Andrew (#100), so the client can be MIT if you relicense your own code and Andrew agrees for his commit (or that part is rewritten).

## What the air taught us

Most of the trouble with KC2G came from the app guessing when a session should exist. The operator picks a station, and the app opens, holds and closes a session from that picture. Under fading the guesses went wrong, and the screen didn't show why.

| What happened (date) | Why | What the client does instead |
| --- | --- | --- |
| "Gliss keeps asking for session disconnect" (2026-10-19) | Deselecting a station to broadcast ended its session, and asked each time | A session ends only when you press **Leave**. Selecting things never ends one |
| KC2G disconnected mid-message in deep QSB while a group file was served (2026-10-19, log 13:04:48 and 13:09:48) | A session closed as soon as other work wanted the group, and the group file counted as wanting it the whole time | No silent auto-close while you are in a session. A quiet link shows as quiet, with a timer, not as closed |
| Directed messages failed after re-selecting him (parked 2026-10-19) | Most likely the same closing rule (fixed in #105); never confirmed | Messages to a station go into the session you are in, and the tab shows whether it is up |
| Session caller missing from the station list (2026-10-19) | A station was listed only once a chat frame decoded | An incoming session opens its own tab at once, listed under Sessions |
| "min left" and "ETA" seemed to disagree under QSB (2026-10-19) | One estimate shown twice; the average rate falls during fades | One estimate, from recent throughput, and a plain **Stalled** state when nothing moves |
| Cancelled session files started again from byte 0 (2026-10-19) | Session files kept no `.part`; fixed in #105, but only while the app runs | Partial files kept on disk across restarts |
| Andrew's feedback on #101 (2026-10-09) | Sessions dropped too easily from a single click | Entering and leaving are explicit buttons, never side effects of a click |
| BW500 missed at first (2026-10-20) | Bandwidth was treated as our mode filter, not the host's setting | Bandwidth is a host setting shown on the connection bar, sent as `BW500`/`BW2300` |

Two more facts shape the design. data2g-host serves one command client at a time, so the client owns the host while it runs. And since Data2G PR #59, the host sends broadcast bursts while a session is idle, so being in a session no longer has to stop broadcast mode.

## What happens to Glissando's Data2G mode

Decided (2026-10-21): Glissando drops Data2G entirely as soon as the client is judged functionally equivalent to Glissando's Data2G mode. One Glissando release then removes the Data2G transport, its Preferences, the FILE and CONNECTED lamps, the transfer rows in COMMS, and the Data2G TV scene. Until then, Glissando's Data2G mode gets fixes only. The chirp modem and the codec2 DATAC13/DATAC4 fallback are untouched throughout.

Functionally equivalent means the client does all of this (tick as each lands):

- [ ] Broadcast ragchew at every speed, with Sending, Sent and Not sent
- [x] Messages to one station in a session, delivered by the modem's acknowledgement (package F)
- [x] Waiting for the session to go idle, shown on broadcast messages held by a session (packages E and F)
- [x] Incoming sessions, with the caller on the station list at once (package F)
- [ ] Session files: offer, save or decline, auto-accept list, carrying on from a kept part, whole-file hash check
- [x] Broadcast files with repair, received automatically or on request (package H)
- [ ] Progress for each transfer, both directions at once, with a finish estimate
- [x] The 500 Hz setting, sending `BW500`/`BW2300` (package E)
- [x] Pause sending (Glissando's Woah!) (package E)
- [ ] No sending outside the US data segments when receive only is set

The Data2G TV scene and the BONKing cat are Glissando art, so they go with the mode rather than to the client. You may want to keep the cat somewhere; that's your call.

## Questions for Jeff

Each has my recommendation. Type your answer in the last column, or comment on the row.

| # | Question | My recommendation | Your answer |
| --- | --- | --- | --- |
| 1 | Does Glissando keep a Data2G backend once the client exists? | Decided: drop it as soon as the client is functionally equivalent (checklist above) |  |
| 2 | Where does the client live? | A new repository of yours, for example `baitisj/<name>`, so Glissando stays the chirp project. Only `baitisj/glissando` is attached to this project, so you'd create the repo and add it | Decided: `baitisj/ragtime` (2026-10-22) |
| 3 | Language and toolkit? | C++17 and wxWidgets: it reuses the tested engines as they are, and the AppImage and MinGW Windows builds are already worked out. Qt 6 is the cleaner-looking alternative; Python and Qt would fit Data2G's own stack but means rewriting the engines | Decided: C++17 and wxWidgets (2026-10-22) |
| 4 | Should Andrew be involved, or should the client live with Data2G? | Ask him early. He owns the protocol and plans the latents extension; he may want the client under arodland or listed in Data2G's docs. Either way it stays a separate program from data2g-host |  |
| 5 | Do the old rules still hold: never pull Data2G code in, never start `data2g-host`? | Keep both for now. If Andrew adopts the client, the first may change, and a **Start data2g-host** button could make sense in a client that is his |  |
| 6 | Should a message to one station outside a session still exist ("@W1AW" in broadcast mode with a chat ACK)? | Keep it: short replies shouldn't need a session. Sessions for QSOs and files |  |
| 7 | Should a session ever close by itself? | Never while you are in it, except an idle limit you switch on (off by default) |  |
| 8 | Name of the client's broadcast group? | Decided: D2G |  |
| 9 | Licence? | MIT: the chat frame code with FreeDV contributors' work isn't needed any more; it takes your relicensing of the Data2G code and Andrew's OK for his commit | Decided: MIT (the repository's LICENSE) |
| 10 | Name? | Something plain. Using "Data2G" in the name needs Andrew's OK. Working name in this doc: "the client" | Decided: Ragtime (2026-10-22). A sibling of Glissando's music, and from "ragchew" |
| 11 | Platforms for version 1? | Linux AppImage and Windows zip, as Glissando. Mac when there is a build route |  |
| 12 | Sign broadcast messages one by one, or with a periodic seal? | Seals in both modes, at most every 10 minutes with the CW ID; receipts in sessions, repair requests in broadcast mode |  |
| 13 | How are keys trusted? | Trust on first use plus a Checked mark after comparing fingerprints; look into Logbook of the World vouching later |  |
| 14 | Pages and boards: which first? | Signed messages first, then pages (small, read only), then boards with forwarding |  |
| 15 | Should signing ever reach Glissando's chirp mode? | Not planned: a seal could fit, but the chirp mode stays as it is unless you want it |  |
| 16 | Should the station go Automatic by itself after an idle timer? | Offer the timer but leave it off by default; the Unattended switch is the normal way. If a remote control link fails, the client stops sending within 3 minutes, unless Unattended is set as the fallback |  |
| 17 | Allow TLS on internet-only node links? | Not in version 1: signatures stop forgery, and one plain protocol is easier to debug. Revisit if node operators ask. Never on mesh links |  |
| 18 | Default for putting internet-borne items on the air? | Emergency and Priority only, and only from keys heard on air |  |
| 19 | Largest item a node carries on HF? | 300 KB for anything on HF (your call, 2026-10-21). Nodes relay up to 32 KB unattended by default, and an operator can raise that to 300 KB |  |
| 20 | Where does the D2G net sit on each HF band? | Inside the 97.221(b) automatic-control segments, one 2.3 kHz channel per band, picked to keep clear of busy Winlink gateways |  |
| 21 | Show of hands: tones or data? | Decided: data from phase 1, no tones (your call, 2026-10-21). The Hand burst needs a data2g-host primitive (ask Andrew); polling until then |  |
| 22 | Stranger rounds (CQ POTA, QRZ?): use every channel, or every other one? | Every other channel, picked by a hash of callsign and the round's nonce. In Andrew's study a stranger 15 dB louder next door blocks a weak Hand outright, while with the nonce a collision costs only one retry, and the same two stations rarely meet again |  |

**Asks for Andrew** (Data2G's author, not you):

- What does an idle session cost on the air (keep-alives, how often), and how long does data2g-host retry before it reports `DISCONNECTED`? That decides how long "Link quiet" can honestly say the host is still trying.
- Can data2g-host answer a CONNECT with "busy, in a session" while it is in one, and report that to the caller's client?
- Can it report overheard session bursts ("heard AG7EW in a session", and the partner's call if the bursts carry it)?
- Can it report an SNR for broadcast bursts over KISS, for the station list?
- Can it take 200 Hz and 1.2 kHz width caps, beside `BW500` and `BW2300`, for the width gearshift?
- Can it take a TX level command, so the client's Auto ALC can hold drive just under ALC?
- Could data2g-host offer a show-of-hands primitive for the D2G net: send a Hand burst (spec under The D2G net) in a given channel, starting a set delay after the last burst heard and shifted to match that burst's frequency offset, and decode every Hand in a list of channels in that window, reporting each with its SNR and offset?
- **Answered (2026-10-22):** Andrew will build the Hand into data2g-host, in both the net and stranger forms. His provisional response is folded into the show-of-hands spec above: 3 s lead time, Data2G's CRC-aided polar code with 63 payload bits and a CRC-16, no Hand-25, a near-far-aware channel map, and a second round for missed stations. The `HAND SEND` / `HAND LISTEN` primitive is under "Who builds it".
- For the gearshift: can the polar modes (`polar-k96-f8`, `polar-k96-f4`, `polar-k192-f8`) carry broadcast and session data, and be picked with `BCAST MODE`? They are gears 2 and 3 on a good fading path.
- Could `MODES` report each mode's thresholds (AWGN and the three fading channels), so the gearbox follows the host instead of a table in the client?

## Build order

Each step is a usable release on its own, and the first real-radio check comes early, because no Data2G code has yet run against two real `data2g-host`s.

Steps 1 to 7 are Stage 1, parity with Glissando.

1. **Core library.** Bring the socket-free engines (command port and KISS, session files, SHA-256) into the new repo as one library with their unit tests, adapted to the new records. CI on Linux, with the GCC 16 check.
2. **Connection and broadcast mode.** Main window, connection bar, Broadcast tab, station list, speeds. Usable on the air between client stations.
3. **Two-host bench.** Two real `data2g-host`s on PulseAudio null sinks, the client on each, run by you on Hamlet (Data2G needs PyTorch). Checks the timers against real bursts.
4. **Deliberate sessions.** Call, the session tab and its status strip, Link quiet, Leave and Leave now, incoming sessions, Accept sessions.
5. **Session files.** Offer cards, the send queue, the new progress row with Stalled, partial files on disk across restarts, Transfers history.
6. **Broadcast files.** Seal-driven repair behind the new cards and rows (seals list digests from the start; signing them comes with step 9).
7. **First release** of the client once it is functionally equivalent (the checklist above), then the Glissando release that drops Data2G.
8. **Protocol version 1 written down as a spec, so others can build clients that talk to it.**
9. **Signing:** station keys, session seals and receipts, broadcast seals with repair requests, badges, trust on first use.
10. **Pages:** a station's own gemtext pages, fetched in a session, cached signed copies.
11. **Boards:** topics, posts, mail, then forwarding between boards, after a Part 97 automatic-control check.
12. **Pictures and voice**, after Andrew's latents spec.
