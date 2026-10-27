# Pictures and voice in chat

A draft UI spec for still pictures (SSTVAE) and short voice clips (RADE)
carried inside Ragtime chat. Nothing here is built. It waits on Andrew's
spec for analogue latents on Data2G and on a "latents over KISS"
extension in data2g-host, and no code starts until Jeff says go.

Where this came from: Jeff asked for it on 2026-10-19, when Data2G was
still a modem option inside Glissando. It was written as a Claude Doc,
"Analogue objects in chat: UI spec (draft)"
(https://claude.ai/code/artifact/1ba7e70a-4807-49c0-b37d-9db535c6ea76,
private to Jeff), and the Part 97 notes were checked against the rule
text the same day. Pictures and voice were Data2G only from the start, so
they moved to Ragtime unchanged when Data2G left Glissando (2026-10-22).
This file is that doc as of 2026-10-26, with Glissando's console names
swapped for Ragtime's:

| The doc said (Glissando) | In Ragtime |
| --- | --- |
| COMMS chat window | The Broadcast and Session tabs |
| GLISS group | The D2G broadcast group |
| FILE lamp, COMMS status rows | The Files tab and the status bar |
| Snooper | The flight recorder (`docs/FLIGHT_RECORDER.md`) |
| Preferences, Audio Config | Settings |
| `UsDataSegments.h` | `core/src/radio/Bands.cpp` |

Where this file and `docs/PLAN.md` ("Pictures and voice, *later*")
disagree, the plan wins.

Digital pictures are another thing, and are built: a picture sent as a
progressive JPEG 2000 file, on D2G or in a ragchew (docs/STAGE1.md,
"Inline pictures", and docs/PROTOCOL.md, "Pictures"). They are data,
so they need none of the analogue rules below.

## Summary

A chat message can carry one analogue object: a still picture made with
SSTVAE, or a short voice clip made with RADE. Its body goes on the air as
analogue latents, not bits, inside Data2G's own framing on its OFDM
modem. A small digital descriptor Ragtime puts in each burst says who
sent it, what it is and which piece is which. That lets a listener who
fades in know the sender, stack a later copy on an earlier one, and ask
for just the weak pieces again.

Andrew plans a spec for sending analogue latents, to a contract like
SSTVAE's or RADE's, on Data2G's OFDM modem within Data2G framing. His
draft so far is [analog slots](https://github.com/arodland/Data2G/pull/83)
(`docs/analog-slots.md`, 2026-10-18). So objects are a Data2G feature:
Data2G frames, sends and measures them, and Ragtime supplies the latents,
the descriptor and the UI. Ragtime reaches the latents through a
"latents over KISS" extension Data2G would add ("What Ragtime needs from
Data2G", below).

What stays the same:

- A message with no object sends exactly what it sends today.
- Every burst's descriptor carries the sender's callsign.
- Long objects split into bursts under the 180 s transmit time-out.

What SSTVAE is, from [its README](https://github.com/arodland/SSTVAE)
(Andrew's too): an autoencoder sends a 640×480 picture as continuous
latents on OFDM carriers, about 1,200 Hz wide, in modes A, B and C of 32,
64 and 95 s. Quality falls gently down to about 0 dB S/N; on fading,
below about −2 dB, acquisition fails and there is no picture at all. Both
ends need the same ~40 MB model checkpoint, and the on-air format is not
frozen. Its interleaver spreads every frame across the whole picture, so
a fade softens all of it a little rather than wiping a band.

## What it builds on

Most of the machinery already exists for files and text; objects reuse
it rather than invent new habits.

| Today | Where | What objects take from it |
| --- | --- | --- |
| Session files: FileOffer, accept, decline, pieces, cancel | `docs/PROTOCOL.md`, "Files in a session"; the Files tab | The offer, the Save or Decline choice, the record layout (new record types for objects) |
| Broadcast files: FileAnnounce, pieces kept a day, seals and repair | `docs/PROTOCOL.md`, "Broadcast files", "Seals and repair" | Refinement follows the same pattern, except a repeated piece is added to what was heard instead of replacing it |
| Repeat averaging in Glissando's chirp receiver: failed copies' soft values kept and a later copy added | Glissando PR #67, `docs/HOW_IT_HEARS.md` Lesson 9 there | The same idea for analogue symbols: add copies, weight each by its S/N |
| Timelines of text and file cards, the Files tab's bars and ETA, the status bar | `BroadcastPage`, `SessionPage`, `FilesPage` | Progress, offers and history for objects, with no new main window |

Two things are missing. The codecs: neither SSTVAE nor RADE is in
Ragtime (SSTVAE's native C++ app and model live in arodland/SSTVAE,
Artistic License 2.0). And Data2G owns the sound card and the PTT, so the
body has to go through Data2G itself: that is what analog slots are for.

## The object

An object is one picture or one voice clip, cut into numbered segments.
The segment is the unit everything else works in: it is what a receiver
places, stacks, rates and asks for again.

| Field | Size | Meaning |
| --- | --- | --- |
| Object id | 24 bits | First 3 bytes of SHA-256 over the sender's call and the encoded symbols. The same object sent again has the same id, which is how copies find each other. |
| Kind | 2 bits | Picture or voice (two spare values) |
| Codec and version | 6 bits | SSTVAE vN or RADE vN. Copies made with different codecs never stack. |
| Segments | 8 bits | How many, up to 255 |
| Segment length | 8 bits | In codec frames. Voice: about 1 s of RADE latents, so a segment is a stretch of time. Picture: a group of SSTVAE latents; because SSTVAE interleaves across the whole picture, a segment is a share of the detail everywhere, not a region. |
| Caption | up to 32 chars | Ragtime's text coding, e.g. `QSL card` or `Field Day setup`. Sent digitally, so it reads even when the body doesn't. |
| Sender | every burst | The callsign, by Jeff's rule |

A receiver keeps, per object and per segment, the running weighted sum of
what it heard, the total weight, and an S/N estimate. That is all it
needs to redraw the best picture so far, or play the best clip so far, at
any moment. Objects are kept a day, up to 8 objects, then dropped.

Names for the two kinds, if Jeff wants 1950s ones: **Radiophoto** for
pictures and **Voicegram** for voice clips. The rest of this file uses
them.

## Framing

An object travels as Data2G bursts in Data2G's own framing, as Andrew
plans: preamble, header and a digital control codeword (group or
session, sequence, refinement layer), then pilots and the latents on its
OFDM modem. Ragtime adds one thing inside that framing, a short digital
descriptor in the same burst, so every burst says who sent it and what it
is even when its latents are too weak to use.

Data2G owns the framing and the waveform; Ragtime owns the descriptor and
what the values mean.

| Part of a burst | Who | Notes |
| --- | --- | --- |
| Preamble, header, control codeword (group or session, sequence, layer) | Data2G | Andrew's spec |
| Pilots, equalisation, clipping, placement, interleaving within the burst | Data2G |  |
| Precision per value on receive | Data2G |  |
| Descriptor: object id, kind, codec and version, the segments in this burst, caption, sender's call | Ragtime, as digital bytes in the burst | Needs Data2G to take a client's digital bytes beside its analog values. Fallback: an ordinary D2G broadcast record just before the burst. |
| Latents to the contract (bounded, stated statistics) and an importance per value | Ragtime, from SSTVAE's or RADE's encoder | Data2G never holds the model |
| What a resend carries: a repeat or a refinement layer | Ragtime |  |

An object takes one or more bursts; each names its segments in its
descriptor, so a listener who fades in mid-object can still place what it
hears. Every burst keeps under the 180 s transmit time-out.

In a session on a strong path the analogue body may not be worth it: the
quantised latents can go as an ordinary session file, which ARQ delivers
exactly. Ragtime could pick that when the session's mode beats the
analogue airtime (question 4).

## Refinement

Two copies of a segment heard at the same S/N, added together, give a
copy 3 dB better; four give 6 dB. Weighting each copy by its own S/N,
segment by segment, means a copy that faded halfway through helps where
it was strong and costs nothing where it was weak.

Why that works, in one step. Each copy k of a symbol arrives as the true
value x plus its own noise, y_k = x + n_k, with noise power σ_k². The
noises of different copies are independent, so the best combination is
the weighted average

    x̂ = (Σ_k y_k / σ_k²) / (Σ_k 1 / σ_k²),    S/N of x̂ = Σ_k S/N_k

The signal adds coherently and the noise only in power, so the S/Ns
simply add.

What the receiver must do for this:

- Demodulate each copy on its own, with its own timing, frequency offset
  and phase, down to the codec's symbols. Combine there, before the
  picture or voice decoder runs, and only for copies with the same id and
  codec.
- Estimate S/N per segment, from the codec's pilots, and keep it as the
  weight.
- Redraw or replay from the combined symbols each time a copy is added.

Any copy with the same id stacks, asked for or not. A station that sends
its QSL picture to every contact sends the same id each time, so a
listener who heard three of those ends up with a three-copy picture.

Who combines: Andrew's draft leaves open whether Data2G or the client
combines resends, and leans to the client as simpler and able to send
refinements that aren't repeats. Recommend the client: it allows
refinement layers, and it lets the viewer show any one copy against the
combined result. All the UI needs is each segment's precision after
combining, whoever does it.

### Asking for a clearer copy

1. A segment under a threshold (say 0 dB after combining) shows as weak
   on the object's quality strip: a row of small cells, one per segment,
   under the thumbnail or the voice clip. On a voice clip the cells line
   up with time. On a picture they don't line up with any region, since
   SSTVAE spreads each segment over the whole picture, so the strip is
   the only place weakness shows.
2. The operator right-clicks the object and picks **Ask for a clearer
   copy**. The weak segments are listed; they can untick some. No asking
   happens on its own.
3. In a session the request is Data2G's own HARQ acknowledgement, which
   carries per-segment S/N. On the D2G group, which has no
   acknowledgements, it goes in the sender's repair windows, as
   RepairRequests for broadcast files do.
4. The sender sees a chip on the card, **W1AW asks for 4 of 12 segments
   again**, with **Send** and **Not now**. A setting, off by default,
   sends them without asking; it is airtime, so the operator decides.
5. The resend is one or more bursts whose descriptors name those
   segments. On the group, several askers' lists are joined, as broadcast
   repairs are.
6. The receiver stacks them. The card's badge goes from **1 copy** to
   **2 copies, +3 dB** on the weak segments.

## Sending a Voicegram

A microphone button beside Send records a clip, cleans it up, lets the
operator hear what the far end will hear, and queues it like a message.
Nothing goes on the air until Send.

Ragtime has no microphone or speaker today: Data2G owns the radio's sound
card, and the Ear only listens to it. So this adds a **Microphone** and a
**Speaker** device to Settings, each with a Test button.

1. **Record.** Click the **Voicegram** button (a little ribbon
   microphone) to start, click again to stop; or hold it, push-to-talk
   style, a setting. A level meter and a count, `0:07 / 0:20`, show while
   recording. The default limit is 20 s.
2. **Clean up.** Run on the recording, each one a switch under a
   **Processing** heading in Settings, all on by default:
   - a high-pass at 100 Hz, for hum and handling noise;
   - noise reduction (RNNoise);
   - a gentle compressor and level to a fixed loudness, so every clip
     goes out at the level RADE expects;
   - trim of silence at both ends.
3. **Preview.** The clip appears in the entry area as a small strip with
   its waveform, length and airtime: `Voicegram, 0:12, about 14 s on the
   air`. **Play** plays it through RADE's encoder and decoder locally, so
   the operator hears the RADE sound and not their raw voice. A second
   button, **Play at −2 dB**, adds noise first to show how it degrades.
4. **Send.** Send queues it to the session's station or the group, as a
   message would be. It gets the same delivery marks and Cancel, and the
   timeline shows the clip with its strip.

Airtime: RADE's latents take about as long as the speech, plus Data2G's
framing. A 20 s clip fits one burst well inside the 180 s limit. The
airtime shown comes from data2g-host.

## Sending a Radiophoto

A **Radiophoto** window composes a picture the way SSTVAE's Transmit
pane does and queues it to the Broadcast or Session tab. It reads
SSTVAE's own overlay and template files, so a send set up in SSTVAE loads
here unchanged.

What SSTVAE already has, from its README and `docs/overlay-templates.md`:
a picture with an overlay document on top (text and image items,
normalised coordinates, JSON); templates, which are overlay documents
with placeholders like `{theircall}`; and an image item whose source is
`last_rx`, the picture being replied to. The preview is the render, so
what is arranged is what goes out.

The window, left to right:

- **Pictures.** Thumbnails from a pictures folder (a setting), plus the
  Radiophotos received this run, newest first. **Load...** takes any
  file.
- **Template.** A list of SSTVAE template files from a templates folder
  (default: SSTVAE's own, if found). Ragtime fills the placeholders from
  what it already knows: `{mycall}`; `{grid}` from the locator;
  `{theircall}` from the session or the selected station; `{snr}` from
  the station list's last report; `{utc}`, `{date}`; `{mode}` as the
  SSTVAE mode. A template's custom fields show as a small form under it.
- **Preview.** The 640×480 render. Text items can be dragged, as on
  SSTVAE's desktop. The `last_rx` inset is the last Radiophoto from the
  selected station.
- **Send panel.** Mode A, B or C (32, 64, 95 s of latents) with the
  airtime from data2g-host; a caption line (the digital caption, 32
  characters); SSTVAE's **Optimize latents before transmitting** switch,
  which runs while the operator edits and is worth 1.4 to 1.8 dB at the
  far end; **Send**, which queues it and closes the window.

The queued Radiophoto is a card in the timeline with its thumbnail, mode,
caption, delivery marks and Cancel. Right-click a station for **Send
Radiophoto...**, which opens the window with that station chosen.

Mode C's 95 s of latents still fits one burst under 180 s; if Data2G caps
bursts shorter, Ragtime splits by segments and the descriptor of each
says which.

## Receiving

A received object is a card in the timeline that fills in as it arrives
and sharpens as copies stack. There is no offer to answer: an analogue
body costs little to keep, so every object heard is kept for a day, as
broadcast file pieces are.

As soon as a burst's descriptor decodes, a card appears:
`W1AW  Radiophoto, mode B: "QSL card"` or `W1AW  Voicegram, 0:12`. On it:

- a Radiophoto's thumbnail, redrawn as segments arrive; a Voicegram's
  waveform strip with **Play**;
- the quality strip, a cell per segment, lit by its S/N after combining
  (bright above about 6 dB, grey down to 0 dB, dark red below, dark where
  nothing came yet);
- a badge, `1 copy` or `3 copies, +4.8 dB`, and the caption.

A burst whose descriptor decoded but whose latents are too weak to use
still shows the card, with the caption and a dark strip, so the sender's
call and caption always read.

Right-click the card: **View...**, **Play**, **Save As...** (PNG for
pictures, WAV for voice), **Ask for a clearer copy**, **Forget**. A
setting, off by default, plays each Voicegram when it completes; another
saves every Radiophoto to the received files folder with SSTVAE's
filename template, `2026-10-19_205414Z_14.078MHz_W1AW.png`.

**The viewer.** **View...** opens a Radiophoto viewer at full size, with
the strip, the copies, and a switch to show any one copy alone against
the combined result, so the operator can see what stacking bought.

**The Files tab** lists objects going out and coming in beside files:
who, what, a bar, the ETA, and Cancel. A refinement request arriving
shows on our object's card as a chip, **W1AW asks for 4 of 12 segments
again**, with **Send** and **Not now**. The flight recorder logs each
burst's descriptor, as it logs chat records.

## What Ragtime needs from Data2G: latents over KISS

Ragtime needs a **latents over KISS** extension in data2g-host: KISS
frames that carry one burst's latents out and back, on the ports chat
already uses (the D2G group's KISS port, and the session). Everything
else in this spec is Ragtime's own. The ground rules stay: Ragtime talks
to data2g-host over TCP only, never starts it, never holds Data2G code;
Data2G never holds the codec's model.

| Frame or report | Carries | What the UI uses it for |
| --- | --- | --- |
| Out: latent burst | Group or session; client protocol id (SSTVAE or RADE, with version); Ragtime's burst tag; the descriptor as digital bytes; value count and format; the values; importance per value or per class; the audio centre (see "A wide passband across the line") | Sending an object |
| Out: withdraw | The burst tag | Cancel on a queued object, before it keys |
| Command: airtime | Protocol id, value count, group or session; back: seconds | The airtime in the Send panels and the 180 s split |
| In: sent, held, dropped | The burst tag, as the host's reports do for chat | Delivery marks and the Files tab's bar |
| In: latent burst heard | Group or session; sender's call; protocol id; the descriptor bytes; values in the sender's units and a precision for each, each burst unmixed; the burst's S/N and frequency offset | Drawing, stacking and the quality strip |
| In: heard, values unusable | Group or session; sender's call; the descriptor if it decoded | The "heard, too weak" card with caption |
| In: measurement acknowledgement | In a session, the far end's per-segment S/N for our burst | The "asks for 4 of 12 segments" chip |

Notes for Andrew:

- A picture is a few hundred thousand values. As 16-bit values with a
  scale that is a few hundred kB per burst over localhost, which KISS
  framing handles; the format is his to fix.
- The descriptor needs a burst that carries a client's digital bytes
  beside its analog values. If that doesn't fit his burst format,
  Ragtime can send the descriptor as a broadcast record just before, at
  some cost to the fade-in rule.
- Importance as a fixed profile per protocol id suits SSTVAE and RADE. If
  Ragtime uses the same protocol ids as SSTVAE's own app over Data2G,
  each can show the other's pictures (question 3).

## Compatibility and open questions

Text chat is untouched. Objects need both ends on a version that has
them and a data2g-host with latents over KISS.

- An older Ragtime never asks for latents, so it never sees an object;
  whether its host shows the burst in monitor mode is Data2G's business.
- SSTVAE's on-air format isn't frozen, and both ends need the same model
  checkpoint. The descriptor carries the codec version; a mismatch shows
  the card with its caption and `needs SSTVAE model vN`, and never
  stacks.

Questions for Jeff, with the recommendation first:

1. **Band segments.** Ragtime is receive only outside the US data
   segments (`core/src/radio/Bands.cpp`). Voice is a phone emission and a
   picture an image emission, which US rules put in the phone and image
   segments (47 CFR 97.305(c)). Recommend: objects transmit only where
   phone or image is authorised, through a second segment table, while
   text stays as it is. This is a reading of the rules, not legal advice.
2. **Codecs in the app.** Ragtime makes and reads the latents, so it
   needs SSTVAE's encoder and decoder (a ~40 MB model) and RADE with
   FARGAN. In-process (the AppImage grows by the model; SSTVAE is
   Artistic License 2.0) or a separate helper process? Does the no-code
   rule for Data2G cover SSTVAE too? Recommend: ask Andrew, and lean to a
   helper process for SSTVAE.
3. **Hearing SSTVAE's own app.** Use the same client protocol ids as
   SSTVAE over Data2G, so the two show each other's pictures? Recommend
   yes.
4. **Session: analogue or a plain file.** Recommend: send a plain file in
   a session when its mode beats the analogue airtime.
5. **Limits.** Voicegrams 20 s, Radiophoto modes A to C, objects kept a
   day, 8 at most. Recommend as written.
6. **Names.** Radiophoto and Voicegram, or plain Picture and Voice
   message?

## Band segments: where data meets phone and image

On 17, 12 and 10 m a General can work both sides of the data and phone/image boundary. On 80, 40, 20 and 15 m the phone side just above the boundary is Extra only. Segment edges and privileges are 47 CFR 97.305(c) and 97.301, checked against the current text on 2026-10-19 (see "Checked against Part 97" below).

| Band | RTTY and data | Phone and image from | General phone from |
| --- | --- | --- | --- |
| 80 m | 3.500 to 3.600 | 3.600 | 3.800 |
| 40 m | 7.000 to 7.125 | 7.125 | 7.175 |
| 20 m | 14.000 to 14.150 | 14.150 | 14.225 |
| 17 m | 18.068 to 18.110 | 18.110 | 18.110 |
| 15 m | 21.000 to 21.200 | 21.200 | 21.275 |
| 12 m | 24.890 to 24.930 | 24.930 | 24.930 |
| 10 m | 28.000 to 28.300 | 28.300 | 28.300 (Technicians: SSB voice only, so no objects) |

No boundary: 160 m allows everything everywhere, 30 m has no phone or image, and 6 m allows everything above 50.1 MHz. On 60 m, data and phone share the same channels and the 5.3515 to 5.3665 MHz segment, but image is not allowed there, so voice could go on 60 m and pictures could not.

- **Settled.** One emission can't straddle the boundary: emissions must be confined to the segment the control operator may use (97.307(b)). A burst sits wholly on one side, with its width plus margin; Ragtime keeps 3 kHz today (`SEGMENT_MARGIN_HZ`).
- **A reading, not a ruling.** The descriptor rides inside a picture or voice burst, so it is part of that image or phone emission. SSTV's VIS code and FreeDV's callsign data are treated the same way in the phone band. So object bursts go on the phone and image side. The FCC hasn't ruled on mixed latent bursts.
- **What follows.** Text (data) and objects (image or phone) can't share one frequency on HF. Data2G listens on one dial, and Ragtime reads the dial but never retunes it. So objects would need their own frequency just above the boundary, or a retune per object.

## A wide passband across the line

With a 6 to 8 kHz USB passband, set the dial 3.2 kHz below each band's data/phone line. Text bursts then sit wholly in the data segment at the bottom of the audio, and picture and voice bursts sit wholly in the phone/image segment above it. The passband straddles the line; no single burst does. One dial serves both, so the radio never retunes.

The arithmetic. In USB a tone at audio frequency f goes out at dial + f, so the line falls at audio b = line − dial. Take each burst as up to 2.4 kHz wide (Data2G's widest modes; SSTVAE needs only about 1.2 kHz), a guard g = 250 Hz on each side of the line, and a passband from 0.2 kHz up. Then:

    0.2 + 2.4 + g  ≤  b  ≤  f_top − 2.4 − g

    8 kHz rig:  2.85 ≤ b ≤ 5.15 kHz
    6 kHz rig:  2.85 ≤ b ≤ 3.55 kHz

b = 3.2 kHz fits both, which puts text bursts at audio 0.55 to 2.95 kHz and picture and voice bursts at 3.45 to 5.85 kHz.

    audio (kHz)  0   0.55         2.95  3.2  3.45         5.85     8
                 |    [ text, data ]   |    [ picture, voice ]    |
                                     line

| Band | Line (MHz) | Dial, USB (MHz) | Text bursts (MHz) | Picture and voice (MHz) | Phone side open to |
| --- | --- | --- | --- | --- | --- |
| 80 m | 3.600 | 3.5968 | 3.59735 to 3.59975 | 3.60025 to 3.60265 | Extra only |
| 40 m | 7.125 | 7.1218 | 7.12235 to 7.12475 | 7.12525 to 7.12765 | Extra only |
| 20 m | 14.150 | 14.1468 | 14.14735 to 14.14975 | 14.15025 to 14.15265 | Extra only |
| 17 m | 18.110 | 18.1068 | 18.10735 to 18.10975 | 18.11025 to 18.11265 | General and Extra |
| 15 m | 21.200 | 21.1968 | 21.19735 to 21.19975 | 21.20025 to 21.20265 | Extra only |
| 12 m | 24.930 | 24.9268 | 24.92735 to 24.92975 | 24.93025 to 24.93265 | General and Extra |
| 10 m | 28.300 | 28.2968 | 28.29735 to 28.29975 | 28.30025 to 28.30265 | General and Extra |

160 m, 30 m and 6 m need none of this (no line, or no phone), and 60 m is channelised.

What this asks for:

- **The rig.** A transmit passband this wide is SDR territory. Most SSB rigs (the IC-7100 among them, as far as we know) stop near 3 kHz on transmit; on those the dial still serves text, but not objects. Check each rig.
- **Data2G.** It must place a burst at a given audio centre: about 1.75 kHz for text, about 4.65 kHz for pictures and voice. That is one more item for the latents over KISS ask (a centre frequency per burst), or two data2g-host instances on one sound card if Andrew prefers.
- **One burst at a time.** Text and an object keyed together would be two emissions at once, raising PEP and making intermodulation that could land on the wrong side of the line. Data2G already sends one burst at a time.
- **The guard.** 250 Hz covers dial error of tens of Hz, filter skirts and drift. It doesn't cover splatter from an overdriven transmit chain, which 97.307(b) also forbids, so drive matters more here than usual.
- **Licence.** On 80, 40, 20 and 15 m the phone side right at the line is Extra only, so a General station gets text there but not pictures or voice. 17, 12 and 10 m work for Generals.

These "straddle" dials are for future experiments. Text would work at them on any SSB rig, since the text bursts sit wholly below the line, but the README's Frequencies section lists the everyday ranges. The table's figures are arithmetic on the segment lines from the table above; the lines themselves are checked against 97.305(c).

## Checked against Part 97

The wide-passband plan holds up against the rule text: the segment lines and class privileges in the tables above are right, and the rule that forbids straddling is confirmed word for word. Two corrections came out of the check, and one question the rules don't answer.

| Claim | Verdict | Where |
| --- | --- | --- |
| Data/phone lines at 3.600, 7.125, 14.150, 18.110, 21.200, 24.930, 28.300 MHz | Confirmed (40 m also has phone at 7.075 to 7.100, but only for stations outside the continental US region) | 97.305(c), 97.307(f)(11) |
| General phone starts at 3.800, 7.175, 14.225, 21.275; 17, 12 and 10 m have no class split above General | Confirmed | 97.301(d) |
| A burst must sit wholly inside its segment: "Emissions resulting from modulation must be confined to the band or segment available to the control operator" | Confirmed | 97.307(b) |
| Data bursts at most 2.8 kHz wide on HF | Confirmed; Data2G's 2.4 kHz modes fit | 97.307(f)(3) |
| A picture burst is limited to the width of a communications-quality voice signal of the same modulation type | Confirmed; SSTVAE's ~1.2 kHz fits | 97.307(f)(2) |
| Technicians may send objects on 10 m | **Wrong.** Technicians may only send SSB voice (J3E, R3E) there, so neither pictures nor RADE voice. Fixed in the band table | 97.307(f)(10) |
| 60 m is like the other bands | **Partly wrong.** Phone, RTTY and data only, no image, 2.8 kHz: voice yes, pictures no. Fixed in the band table | 97.307(f)(14) |
| The descriptor's callsign counts as station identification | **No.** An image ID must meet broadcast TV standards (73.682(a)), and a data ID needs a data emission. Picture and voice bursts still need a CW or spoken ID every 10 minutes and at the end; a CW ID is allowed anywhere | 97.119(a), (b); 97.305(a) |

What the rules don't settle: whether a burst that mixes digital bits (Data2G's control codeword and our descriptor) with analogue latents is a phone or image emission, or partly a data one. 97.3(c) defines the types by emission designator, and a mixed analogue and digital multicarrier burst doesn't fit any listed designator cleanly. Nothing in 97.307 allows or forbids data multiplexed inside a phone or image emission. The nearest precedent is FreeDV: its specification calls FreeDV 1600 "1K20J2E", a phone emission, while it carries callsign text and sync bits, and it runs in the phone segments. That is the project's own claim, not an FCC ruling. If the FCC saw the descriptor and control codeword as data, object bursts would belong in the data segments instead, and the wide-passband trick would be pointless. Keeping the digital part small, as FreeDV's 25 bit/s text is, makes the phone and image reading stronger. For certainty, the ARRL's regulatory staff are the people to ask.

Sources (opened 2026-10-19): [97.3](https://www.law.cornell.edu/cfr/text/47/97.3), [97.119](https://www.law.cornell.edu/cfr/text/47/97.119), [97.301](https://www.law.cornell.edu/cfr/text/47/97.301), [97.305](https://www.law.cornell.edu/cfr/text/47/97.305), [97.307](https://www.law.cornell.edu/cfr/text/47/97.307), [97.309](https://www.law.cornell.edu/cfr/text/47/97.309), [FreeDV specification](https://freedv.org/freedv-specification/). This is a reading of the rules, not legal advice.
