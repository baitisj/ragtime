# Ragtime protocol, version 0

This is what Ragtime puts on the air through data2g-host: the records,
how they are packed into broadcast frames and into a session's byte
stream, how they are numbered and named, and how text is coded. The code
that implements it is `core/src/protocol/` (records, frames, streams),
`core/src/text/` (the text table), `core/src/link/` (the host's ports)
and `core/src/files/SessionFiles.cpp` (files in a session). Where this
document and the code disagree, fix whichever is wrong in the same PR.

Version 0 is new. It shares nothing with Glissando's frames, and the two
programs don't read each other: Ragtime opens the broadcast group `D2G`,
never `GLISS`.

## The rules it keeps

- **Every record names its sender.** Each one carries the sender's
  callsign, a sequence number and the time, so a listener who fades in
  mid-stream knows who it is hearing. There are no session tags in place
  of callsigns.
- **No frame is ever all zeros.** A frame of zeros passes Data2G's CRC,
  so every frame and every session stream opens with the byte `0xA0`.
- **Signed, never secret.** Nothing is encrypted (Part 97,
  97.113(a)(4)). The text table is published here and is lossless, so it
  hides nothing. Signatures come in Stage 2; the digests they will sign
  are here from the start.
- **What is hashed is what was typed.** Digests cover the exact UTF-8
  bytes typed, with no Unicode normalisation or line-ending changes, and
  never the coded bits, so a new text table never changes a digest.
- **300 KB at most** for any file over HF.

## Where records travel

data2g-host has three TCP ports ([`DATA2G.md`](DATA2G.md) sums up its
commands and reports, read from Data2G's source):

| Port | Default | Carries |
| --- | --- | --- |
| KISS | 8100 | Broadcast frames on the `D2G` group's KISS port, sent as ACKMODE frames so the host reports each one sent |
| Commands | 8300 | `MYCALL`, `LISTEN`, `BW500`/`BW2300`, `BCAST OPEN D2G FROM <call>`, `MODES`, `BCAST MODE`, `CONNECT`, `DISCONNECT`, `ABORT`, and the host's reports |
| Session data | 8301 (commands + 1) | A session's byte stream, both ways, once `CONNECTED` |

**A broadcast frame** is the byte `0xA0` followed by whole records, as
many as fit. In version 0 a frame is at most 255 bytes. Data2G sizes
every data mode's receiver for one 256-byte KISS frame; a longer one
fits only some modes (`docs/DATA2G.md`, "A broadcast burst"). Frames
aren't padded: Data2G marks where each frame ends (a 2-byte length, or
from broadcast v3 COBS and a zero byte), so the frame ends at its last
byte. A record never spans two frames.

**A session stream** is the byte `0xA0`, then a Hello, then any records,
back to back, for as long as the session lasts. Each session starts a
fresh stream in each direction.

The top nibble of `0xA0` says Ragtime, and the bottom one the protocol
version, so a version 1 frame opens with `0xA1`. A receiver drops a frame
whose first byte it doesn't know.

## Records

Every record has the same outline:

| Field | Bytes | Meaning |
| --- | --- | --- |
| type | 1 | Below. 0 is never a type |
| length | 1 or 2 | The bytes after this field, as an unsigned LEB128: 7 bits a byte, least significant group first, the top bit set on every byte but the last. At most 2 bytes in v0, so a record holds at most 16,383 bytes after its length |
| callsign | 2 to 8 | The sender, packed (Fields, below) |
| seq | 3 | The sequence number (Sequence numbers, below) |
| time | 4 | When it was made, in Unix seconds, UTC |
| body | the rest | By type |

The header (callsign, seq and time) is at least 9 bytes; for a
five-character call it is 11, and the outline with its type and length
is 13.

All numbers are big-endian (most significant byte first), except the
length.

### Types

| Type | Name | Body | Is an item? |
| --- | --- | --- | --- |
| `0x01` | Hello | version, flags, takes, [locator, [software]] | no |
| `0x02` | Status | state, [partner] | no |
| `0x10` | Text | flags, [to], text | **yes** |
| `0x11` | Ack | to, item | no |
| `0x12` | Ping | to, number, mode | no |
| `0x13` | Pong | to, number, S/N | no |
| `0x20` | FileOffer | size, digest, name | **yes** |
| `0x21` | FileAnswer | offer, answer, [from] | no |
| `0x22` | FileData | offset, bytes | part of its offer |
| `0x23` | FileSaved | offer | no |
| `0x24` | FileCancel | offer, reason | no |
| `0x30` | FileAnnounce | size, piece size, digest, name | **yes** |
| `0x31` | FilePiece | index, bytes | part of its announce |
| `0x40` | Seal | number, previous, first item, count, digests | no |
| `0x42` | RepairRefused | seal, reason, count, files | no |
| `0x43` | RepairRequest | to, seal, seal digest, flags, items, items wanted, files | no |
| `0x44` | ItemParity | seal, count, items, index, bytes | no |

`0x41` was Ragtime 0.1's RepairRequest, which listed the pieces missing.
It is retired: a newer station skips it as a type it doesn't know, and
0.1 skips `0x43` and `0x44` the same way. So 0.1 and newer stations
still chat, but can't repair each other.

Square brackets mark optional trailing fields: a record may end before
them.

**Hello** (`0x01`) opens every session stream, and may go in a broadcast
frame as a check-in.

| Field | Bytes | Meaning |
| --- | --- | --- |
| version | 1 | The protocol version the sender speaks: 0 |
| flags | 1 | Bit 0: accepts sessions. Bit 1: automatic (nobody at the controls; Stage 2 gives it its rules). Other bits 0 |
| takes | 1 | The records the sender takes. Bit 0: Text and Ack. Bit 1: Status. Bit 2: session files (FileOffer to FileCancel). Bit 3: broadcast files (FileAnnounce, FilePiece). Bit 4: Seal, RepairRequest, RepairRefused and ItemParity. Bit 5: Ping and Pong (0.1.2 on). Version 0 sends `0x3F`, and sent `0x1F` before 0.1.2 |
| locator | 4, optional | Maidenhead, packed (Fields); 0 for none |
| software | 1 + n, optional | A short string, at most 64 bytes, such as `Ragtime 0.1.0` |

A station never offers a file to one whose Hello lacks bit 2 of takes;
the file ends Not taken.

**Status** (`0x02`): state, 1 byte (0 QRV, available; 1 QRL, in a
session; 2 QRT, closing down), then optionally the partner's callsign
when QRL.

**Text** (`0x10`): one message, whole, or one part of a long one
(Broadcast, below).

| Field | Bytes | Meaning |
| --- | --- | --- |
| flags | 1 | Bits 0 to 3: the coding (0: text table 0; 15: raw UTF-8; 1 to 14 for later tables). Bit 4: addressed. Bit 5: ack wanted (only when addressed) |
| to | 2 to 8, when addressed | The station it is for |
| text | the rest | Coded as the flags say |

The sender codes the text both ways and sends the shorter, table 0 on a
tie. A receiver that doesn't have the table named skips the record as
malformed. There is no length inside: the text runs to the end of the
record.

**Ack** (`0x11`): to (the Text's sender, packed), then item (3 bytes,
the Text's sequence number). It answers an addressed Text that asked for
one.

**Ping** (`0x12`), on the group: to (the station asked, packed), number
(2 bytes, the sender's own count, so a Pong names its Ping), mode (a
short string: the Data2G mode the Ping goes in, as `MODES` names it;
empty for the group's). `BCAST HEARD` doesn't say a burst's mode, so the
Ping carries it.

**Pong** (`0x13`): to (the Ping's sender), number (2, the Ping's), S/N
(1 byte: the S/N the answering host gave for the Ping's burst, in
RepairRequest's S/N code, 0 for none; 128 to 255 read as none). With
five-character calls each is 20 bytes, one or two codewords in most
modes. Pings, below.

**FileOffer** (`0x20`), in a session: size (4 bytes, at most 307,200),
digest (12 bytes, the whole file's: SHA-256, first 12 bytes), name (a
short string, UTF-8, at most 200 bytes). The offer is an item; its
sequence number names the transfer from then on.

**FileAnswer** (`0x21`): offer (3), answer (1 byte: 0 accept, 1
decline), and when accepted from (4 bytes): how much of the file the
receiver already holds from an earlier try, so only the rest is sent.
Missing, from is 0.

**FileData** (`0x22`): offset (4 bytes, where in the file these bytes
start), then the bytes, at most 4,096 a record. Its header's seq is the
offer's.

**FileSaved** (`0x23`): offer (3). The receiver has every byte, the
digest matched, and the file is saved under its name.

**FileCancel** (`0x24`): offer (3), reason (1). The top bit of the
reason says who is speaking, which also says whose transfer it ends:

| Reason | Sent by | Means |
| --- | --- | --- |
| `0x01` | sender | The operator stopped it |
| `0x02` | sender | The file could not be read, or the receiver asked to carry on past its end |
| `0x03` | sender | The offer went unanswered |
| `0x81` | receiver | The operator stopped it, or declined after accepting |
| `0x82` | receiver | It could not be written, the bytes came out of order or more than offered, the digest didn't match, it is over 300 KB, or this station may not transmit now |
| `0x83` | receiver | The offer went unanswered |

**FileAnnounce** (`0x30`), in broadcast mode: size (4), piece size (2,
never 0), digest (12, the whole file's), name (short string). An item.

**FilePiece** (`0x31`): index (2), then the piece's bytes. Its header's
seq is the announce's. Piece i holds bytes i × piece size up to the next
piece's start; the last may be short. An index at or past the piece
count names a **parity piece** (Parity, below), as long as the piece
size rounded up to even; Ragtime makes the piece size even, so a parity
piece is no longer than a piece.

**Seal** (`0x40`): the table of contents of what a station has sent
since its last seal.

| Field | Bytes | Meaning |
| --- | --- | --- |
| number | 2 | This seal's number, counting up from 1 |
| previous | 12 | The previous seal's record digest; zeros before the first |
| first item | 3 | The first item's sequence number |
| count | 1 | Items listed, at most 255 |
| digests | 12 each | The record digest of first item, first item + 1, and so on |

A file is listed once, by its FileAnnounce's digest, which covers its
size, piece size and whole-file digest. There is no digest per piece:
Data2G's own coding catches a damaged piece, and the whole-file digest
proves the result. Stage 2 signs seals; version 0's seals are unsigned.

**RepairRequest** (`0x43`): a listener's word that it lacks something a
seal covers, and how many repair symbols it wants for it.

| Field | Bytes | Meaning |
| --- | --- | --- |
| to | 2 to 8 | Whose seal |
| seal | 2 | Its number; 0 to ask for the seals over the files listed (below) |
| seal digest | 12 | Its record digest; zeros when asking for a seal not heard |
| flags | 1 | Bit 0: send the seal itself again. Bits 1 to 7: the asker's S/N code (below) |
| count | 1 | Items in the seal (0 when it wasn't heard) |
| items | count / 8, rounded up | One bit per item, the first item in the top bit of the first byte: 1 = missing |
| items wanted | 1 | Repair symbols wanted for the items missing: as many as are missing, or more for what the asker expects to lose (Parity, below) |
| files | 5 each, to the end | For each file it lacks pieces of: its seq (3), then the parity pieces it wants (2) |

**Seal 0.** Seals count from 1, so a request under seal 0 asks for
whichever seals list the files it names: flag bit 0 set, count 0, and
each file with a want of 0. A listener sends one when it heard a file
(its announce or a piece) but no seal over it, every copy lost.

**The S/N code** in bits 1 to 7 of flags says how well the asker hears
the seal's sender, so the repairs can go in a gear it can copy. It is the
mean of the SNRs the asker's data2g-host reported for that sender's
last 5 bursts in the last 15 minutes, the figure the gearshift's Auto
shifts for. Until 0.1.2 it was the lowest of the 15 minutes, which in
Jeff's AG7EW–KC2G test (2026-10-25) put every repair in the slowest
gear for one deep fade while the path ran up to 10 dB above it; the
parity covers what a fade takes. Senders read either the same way.
Code v = 0
means no figure (a host from before Data2G PR #98, or nothing heard
lately); 1 to 127 mean −24 + (v − 1) × 0.5 dB, so −24 to +39 dB in
half-dB steps, rounded to the nearest and held at both ends.

**RepairRefused** (`0x42`): the seal's sender saying it won't repair
what was asked for, once for every station that asked.

| Field | Bytes | Meaning |
| --- | --- | --- |
| seal | 2 | The seal the refusal is about |
| reason | 1 | 0 stopped sharing (the operator closed its repairs), 1 cancelled (the operator stopped the file and closed its repairs), 2 no longer held (expired, or lost in a restart), 3 closed (out of its sensitivity window, or a file under it had the repair airtime its sender allows) |
| count | 1 | Files the reason covers; 0 means everything under the seal, chat lines too |
| files | 3 each | Each file's seq |

A refusal over one file is 20 bytes with a 5-character call: one
codeword in every Data2G mode. A reason this version doesn't know is a
refusal all the same.

**ItemParity** (`0x44`): a repair symbol over some of the items a seal
lists (Parity, below).

| Field | Bytes | Meaning |
| --- | --- | --- |
| seal | 2 | The seal whose items it covers |
| count | 1 | Items in the seal |
| items | count / 8, rounded up | One bit per item, as in RepairRequest: 1 = covered |
| index | 2 | Which parity symbol: it is named 256 + index |
| bytes | the rest | The symbol, an even number of bytes |

### Fields

**Callsigns** are packed three characters to two bytes, Radix 40: `A`
to `Z` are 1 to 26, `0` to `9` are 27 to 36, `/` is 37, `-` is 38, 0 is
the end mark and 39 is reserved. The call (upper case, 1 to 10
characters, as data2g-host takes it), then an end mark, padded with end
marks to a multiple of three; each three codes c1 c2 c3 become the
16-bit number (c1 × 40 + c2) × 40 + c3. So AG7EW is `077a 22d8`, and a
record's callsign field ends at the first group holding an end mark.

**Locators** pack into 4 bytes: square × 577 + sub + 1, where square is
the four-character square as a number ((field1 × 18 + field2) × 10 +
digit1) × 10 + digit2, and sub is 0 for a four-character locator or 1 +
s1 × 24 + s2 for a six-character one. 0 means none.

**A short string** is a length byte then that many UTF-8 bytes.

**A digest** is the first 12 bytes of a SHA-256.

## Sequence numbers

A station numbers its own records, separately for its broadcasts and for
each session:

- An **item** (Text, FileOffer, FileAnnounce) takes the next number. The
  first is 1; after `0xFFFFFF` it starts again at 1. 0 means no item yet.
- A **part of an item** (FileData, FilePiece) carries its item's number.
- **Anything else** (Hello, Status, Ack, the file answers, Seal,
  RepairRequest) carries the newest item's number, so a listener can see
  what it has missed since.

The broadcast numbering is kept across restarts. A session's starts
afresh at 0 with each session, so its first item is 1.

An item is named by (callsign, seq, time). The time tells apart two items
with the same number after the numbers wrap, or after a station loses its
saved numbering.

## Record digests

An item's **record digest** names it in seals and repair requests. It is
the first 12 bytes of the SHA-256 of the record's canonical form:

    type (1) | callsign (packed) | seq (3) | time (4) | body

with no length field, and with a Text's body as it is when coded raw
(flags low nibble 15, then the UTF-8 bytes). So the text table a Text
went out in never changes its digest. Any record has one; in v0 seals
list only items.

## Text coding

Table 0 is Glissando's ham-table Huffman code, exactly: the same codes,
built by `tools/ham_table/gen_table.py` from the same corpus
(`chat_corpus.txt`, `prose.txt`, `phrases.txt`) into
`core/include/ragtime/text/HamTextTable.h`. It codes common chat in about
4.6 bits a character, and 42 common phrases ("CQ CQ", " de ", " 73",
" antenna" and so on) as one symbol each. Any byte not in the table goes
as an escape and the raw byte, so every byte string round-trips, UTF-8
or not.

A whole Text record's text is one coded run, padded with zero bits to the
byte. Zero bits after the last symbol decode to nothing, so the padding
needs no length.

**Table 0 is frozen.** Its codes are a protocol constant: two goldens in
`tests/HamTextTest.cpp` check them, and `gen_table.py --check` checks the
header against the corpus. A better table takes a new number (1 to 14)
and goes beside table 0, never in place of it.

## Sessions

A session (a ragchew) starts only when an operator presses **Call** (the
host sends `CONNECT <me> <them>`) or the host reports one coming in, and
ends only on **Leave** (`DISCONNECT`, which waits for what is still
unacknowledged) or **Leave now** (`ABORT`), the far end leaving, or the
host giving up the link. Nothing else opens or closes one.

1. On `CONNECTED`, each end writes `0xA0` and its Hello on the data port,
   numbered afresh.
2. The first record of the other end's stream must be a Hello. If the
   stream doesn't open with `0xA0` and a Hello, or a record's length or
   type is invalid, it isn't Ragtime's: the reader takes nothing more
   from it and the screen says so.
3. A known record that doesn't parse is skipped by its length, and counted.
4. The far end's Hello can arrive before `CONNECTED` is read, since they
   come on different ports. Ready the stream reader when a session ends,
   not when one starts.
5. **Leaving.** Before `DISCONNECT`, a station writes a **Status** QRV
   into the session, its farewell, and sends `DISCONNECT` once the host
   has read it (or 10 s on). The far end, finding the farewell in the
   stream, says "Session ended by AG7EW"; a session that ends without one
   is "Link lost" (or, from a host since Data2G PR #122, what its
   `DISCONNECTED` reason says). The farewell and the host's `DISCONNECTED` come on
   different ports, in either order, so a station still reads the stream
   for a second after a session ends without one. Leave now (`ABORT`)
   writes no farewell.
6. **On the group,** each end sends a Status QRL naming its partner when
   a session starts, and a Status QRV when it ends (`docs/PLAN.md`, "Busy
   stations"). The host holds the QRL until the session goes idle; a QRL
   still unsent when the session ends is taken back, with no QRV after
   it.

Data2G delivers the session's bytes in order and acknowledges them: the
host's `BUFFER <n>` report is the bytes the host has read from us that the
far end has not yet acknowledged. A station takes its own writes up to
what the host has read minus n as delivered, counting only what it can
be sure of (`session::StreamAccount`):

- A count that rose by k means the host read at least k more bytes,
  since acknowledgements only lower it.
- `BUFFER 0` means everything read is acknowledged. One that comes 2 s
  or more after a write settles that write too: the host reads its data
  port as bytes come and answers each read with a count, so a 0 that late
  is not from before the read. (A read that takes an acknowledgement
  bigger than itself in the same step lowers the count instead of raising
  it; this is what settles it.)

### Files in a session

One file goes to a station at a time; the next waits for it.

1. The sender writes a **FileOffer**. It waits 4 minutes for an answer,
   then cancels (reason `0x03`); the receiver gives it up at the same
   time (`0x83`).
2. The receiver answers with a **FileAnswer**: decline, or accept with
   from. From is the length of a part file the receiver kept from an
   earlier try of the same file (same station, size and digest), else 0.
   An offer over 300 KB is cancelled at once (`0x82`).
3. The sender writes **FileData** records, at most 4,096 bytes each,
   from offset from up to the size, in order.
4. The receiver checks each FileData's offset is where the file has got
   to, and that no more comes than was offered; otherwise it cancels
   (`0x82`) and throws the part away.
5. With every byte in, the receiver checks the whole file's digest
   against the offer's. A match is saved under its name and answered with
   **FileSaved**; a mismatch is thrown away and cancelled (`0x82`). The
   sender counts the file delivered only on FileSaved.
6. Either side can **FileCancel** at any time. A FileSaved that crosses
   the sender's cancel still counts as delivered. The session's stream
   is one queue, so a sender's cancel goes out behind the file bytes the
   host already has (unless the host can cut them, below), and the
   receiver takes those until the cancel comes. A receiver that cancels
   drops what comes after.
7. A transfer cancelled, or a session ending under it, leaves the
   receiver with a part file: every byte that arrived, including those
   of a FileData still coming in (the stream is in order, and the digest
   checks the whole file in the end). It keeps it for 7 days and at most
   8 of them, across restarts, so the same file from the same station
   carries on from it next time. The sender keeps a note of the part it
   sent for as long, so its operator can send the rest.
8. **Leaving** with a transfer under way, a station writes its
   FileCancel (`0x01` or `0x81`) before the farewell, so the far end
   hears why. A file still waiting its turn is offered in the next
   session with that station.

A sender offers nothing until the far end's Hello has come and says it
takes session files. It keeps no more file bytes waiting at the host than
two of the session mode's longest bursts (between 1 and 8 KB), so a Text
typed during a transfer goes out behind at most that much; with a host
that can cut, 8 KB, and a Text cuts in ahead of them. How much is the
sender's own business and not part of the protocol.

The receiver saves under a safe form of the offered name: its last path
part only, without control or direction characters, path separators,
Windows' forbidden characters, leading or trailing dots and spaces, or a
Windows device name, cut to 200 bytes at a character boundary. Auto-accept
never overwrites a file; it numbers the name instead.

### Cuts

Since Data2G PR #118 a host can drop session bytes it holds and hasn't
sent. Ragtime asks for `DATA2G ON` when the command port opens; a host
that answers WRONG is older, and its sessions go without cuts. With it
on, offsets are host byte offsets: the bytes the data port has taken
this session, counted from `CONNECTED` (Ragtime writes nothing before
it, and the host drops what comes earlier).

A sender cuts when a file of its stops either end with bytes still at
the host, when Leave finds file bytes there, and when a Text is typed
with more than 1 KB waiting there behind file bytes. Nothing changes for
the far end: its stream stays whole, and no record says a cut was made.

1. The sender stops writing, and once nothing has gone to the data port
   for half a second (so the host has read all of it) sends
   `CLEANTXBUFFER n`, n being every byte written.
2. `CLEANTXBUFFER OK drop_from drop_to limbo_from limbo_to`: the host
   dropped `[drop_from, drop_to)`; `[limbo_from, limbo_to)` was on air,
   not yet acknowledged. A drop_to short of n means bytes were still on
   their way to the host; those were kept, so the sender cuts again.
3. With a limbo, it waits for `CLEANTXBUFFER SETTLED from split to`:
   delivered before split, dropped from it.
4. The far end now has the sender's stream up to the first byte cut. The
   host cuts at its own record boundaries, not Ragtime's, so that byte
   may fall inside one of our records: the sender writes the rest of that
   record again, from its copy of what it wrote, then the Texts waiting,
   then every record that began after the cut except FileData, then the
   rest. A file still going carries on from the first FileData cut; one
   that stopped counts as sent only what went before it.
5. `LATE`, `BUFFEREMPTY`, `FAILED`, WRONG, or no answer in 10 s: nothing
   was cut, and writing goes on. A session that ends first (`SETTLED ...
   UNKNOWN`) takes the cut with it. Cuts that leave a hole in the middle
   of the stream (bytes after the cut went on air) can't be mended, and
   the sender leaves at once (Leave now).

## Broadcast

Broadcasts go on the `D2G` group, opened with `BCAST OPEN D2G FROM
<call>`; the host answers with the group's KISS port.

- A **burst** is one or more frames written together as ACKMODE frames,
  each with a 2-byte tag. It is **Sent** when the host has acknowledged
  every tag, and **Not sent** if the host reports `DROPPED`, the command
  port closes, or it waits 2 minutes, counted while no session is up (a
  session holds the group, so a burst waits for it to end).
- Before a burst in another mode, the station sends `BCAST MODE <port>
  <mode>`. Modes are Data2G's, named as the host's `MODES` list names
  them; timings follow that list's airtimes, never Glissando tempos.
- The host reports `BCAST <port> HEARD <call>` for a burst it decoded
  and `LOST` for one it couldn't. The records inside name their senders
  anyway. Since Data2G PR #98 HEARD ends with the burst's SNR (dB,
  average power in 2500 Hz, the gearshift's scale), always the last word;
  a callsign always has a letter, so the two never mix. `BCAST * MISSED
  <submode> <codewords> [snr]` is a burst no port could claim, maybe
  another group's. In a session, `SN` answers `SN <snr>` for the partner's
  last burst, or WRONG before one. A host from before PR #98 sends no
  SNR and answers SN with WRONG, and Ragtime works the same with it,
  less the SNR column and Auto.
- **A message too long for one frame** goes as several Texts, each an
  item of its own in a frame of its own, all in one burst. The sender
  splits between words where it can (at a space, which is dropped) and
  always between UTF-8 characters. A message is at most 2,000 bytes.
- **Check-ins.** A Hello rides along at the front of a message's first
  frame: the first message after the group opens, the first after the
  station's callsign, locator or Accept sessions changes, and then at
  most every 10 minutes. A station may also send a Hello alone.
- **Addressed Texts.** A Text addressed to a station with ack wanted is
  answered on the group by that station's Ack, at once and without the
  operator; Acks heard together go in one burst. A Text heard again (the
  same callsign, seq and time) is shown once but acknowledged again, since
  the sender is asking because the first Ack went missing.

### Pings

A Ping asks one station how well it hears us, and tries a gear both
ways (Jeff, 2026-10-25).

- The pinged station answers with a Pong, by itself, as it answers an
  Ack: in the mode the Ping names when its host offers it, else in the
  group's mode. Its S/N is the figure its host's `BCAST HEARD` gave for
  the Ping's burst; that line and the frame come on different ports in
  either order, so it waits up to 3 s for the figure (one under 2 s old
  counts), then answers without one.
- At most one Pong to a station every 15 s, however often it pings.
- The pinger reads the S/N it hears the Pong at from its own host, the
  same way. A Pong that doesn't come within twice the mode's listening
  window plus 3 s (at least 20 s) shows as no answer; a late one still
  counts. Nothing is retried by itself.
- A station never pings one whose Hello lacks takes bit 5.
- Pings and Pongs aren't items: they carry the newest item's number, no
  seal lists them, and they are never repaired.

### Seals and repair

`docs/PLAN.md` ("Seals, receipts and repairs") has the reasoning, and
`docs/HOW_SEALS_WORK.md` explains it in plain words with pictures; this
is what version 0 does. Every timing is in airtimes of Data2G modes, as
the host's `MODES` lists them.

**The sender.**

- It seals its FileAnnounces, and its Texts only when the operator
  asks for it (Ragtime's "Seal my broadcast messages", off by default,
  because a seal costs a transmission of its own). Other records are
  never sealed.
- It seals the items it has broadcast and not yet sealed: when nothing
  of its own is on the way and either the last item went 60 s ago or a
  file's first pass has just ended, and in any case once the oldest
  unsealed item is 10 minutes old.
- A seal lists a run of consecutive numbers. An item that never went (its
  burst was Not sent) is listed with a zero digest, so nobody asks for
  it; a run that starts with such items starts after them. A gap in the
  numbers (unsealed chat between files) ends a run, and the next run is
  another seal. A run too long for one frame goes as several seals, a
  frame each. The seals made at one time are packed into bursts together.
- A seal burst that is Not sent goes again, three tries in all.
- When a seal burst is Sent, a **listening window** opens: 3 × the
  airtime of a one-codeword burst in its mode, plus 4 s (52 s in
  fsk16r25-r1/3, 13 s in qpsk-r1/3).
- At the window's end it answers with parity (below), under each seal
  and in each repair gear: as many symbols as the neediest asker in that
  gear wants, not the union of everyone's gaps. For the items, that is
  the items themselves when the wants are at least as many as the items
  asked for, followed by ItemParity for the rest; with fewer wanted,
  that many ItemParity, unless the items themselves take no more
  airtime; and always the items themselves when an ItemParity record
  wouldn't fit a frame. For each file, that many parity pieces, never
  the same one twice. Then come the seals asked under, which open the
  next window when that last burst is Sent. A round is cut at 10
  minutes of airtime, and the rest is asked for again.
- **Sizing per gear.** An asker that lacks M items and wants w symbols
  for them, and lacks m of the items going in one gear, is counted as
  wanting w × m / M of that gear's symbols, rounded up.
- **The repair gear.** Each asker's gear is the fastest on the sender's
  own ladder needing no more than the asker's S/N less 3 dB (the
  gearshift's Auto sum), or gear 1 when even gear 1 needs more. An item
  or piece goes again in the slowest gear of the stations that asked for
  it, and never in a faster gear than it first went in, which failed
  someone already, nor in a slower gear than the sender's floor
  (Ragtime's "Slowest repair gear", none by default) unless it first
  went slower still. A file the operator gave its own repair gear (its
  menu's Repair gear) goes in that gear, whatever the askers say. A
  request with no S/N, or a sender with no ladder, means the gear it
  first went in. This holds whether the operator chose
  the gear or Auto did. The resend goes gear by gear, fastest first,
  each in sequence order, and the seals go last in the slowest gear
  used, so every asker can copy them; the next window is sized for that
  gear.
- **The slow cap.** Gears slower than the round's own (the newest
  seal's) may take at most 5 minutes of airtime a round (Ragtime's
  "Maximum time spent in repairs per round", 1 to 60 minutes). What doesn't fit is asked for again
  next round, so other stations get a word in between. A round's first
  burst always goes. A file's parity pieces are made to fit the cap, as
  they are numbered one after another: one made and then cut would look
  lost to its listeners (below, Wants).
- **The repair airtime cap.** Once a file's repairs have taken the
  airtime its sender allows (Ragtime's "Close a file's repairs after N
  minutes of repair airtime", 30 by default), its seal closes, and a
  request under it is refused (reason 3). The operator may re-open it
  while the seal is still held: the seal goes again, the count starts
  afresh, and a listener that heard the seal after the refusal asks
  again.
- **Closing.** When the operator closes a seal's repairs, or picks a
  file's repair gear, the round's bursts still queued that carry only
  repairs under that seal are taken back. The one with the host goes
  on; the round's seals still go, so the next requests are answered in
  the new gear, or refused.
- **Refusals.** A request under a seal the sender won't serve is
  refused at the window's end, in the gear of the weakest station that
  asked, after the seals: a seal closed by the operator (reason 1 if a
  file under it was stopped, else 0), a seal out of its sensitivity
  window (3), pieces of a file it no longer holds (1 or 2, for that
  file), or a seal of its own it no longer keeps (2): one at or up to
  32767 numbers before its newest. Requests for the same seal and reason
  share one refusal, and the same refusal goes at most once in 10
  minutes. A request with no window open opens one for it.
- A window with no requests ends the repairs, except that a seal over a
  file goes again after it, at most twice more, in case the seal or a
  request was lost (NORM's flush, repeated).
- A request that comes with no window open (a listener that pressed
  Receive late) opens one, so others can join it.
- A request under seal 0 is answered with the seal over each file it
  names, at the window's end as any seal asked for. A file not sealed
  yet is left alone, since its seal is coming. One it no longer holds
  is refused (reason 2, under seal 0, naming the file), and one whose
  seal is closed is refused under that seal, naming the file, since the
  asker doesn't hold the seal.
- It answers a seal while the seal is in its **sensitivity window**: one
  of its newest 3 seals, or younger than 4 × the airtime of the items it
  covers, and never past 2 hours, unless the operator keeps it open
  longer or closes it now.

**A listener.**

- It keeps what it heard of each station (items, seals, and which files
  it heard pieces of) for 2 hours.
- On a seal it works out what it lacks: items it never heard or heard
  differently, how many pieces it lacks of files it is receiving (less
  the parity pieces it holds), and a seal it missed (each seal names the
  previous one; the request then has the missed seal's number, a zero
  digest and flag bit 0). For each it asks a want sized for its loss
  (Parity, below).
- It asks only about overs it was there for: it holds an item of the
  seal, or a piece of a file the seal lists (the announce may be what it
  lost), or an older seal or item of that station. A station that just
  tuned in doesn't ask for what went before.
- It waits a random time within the window less its own request's
  airtime and 2 s, reckoned in its own mode. Requests it hears from
  others to the same station meanwhile speak for it, if their asker's
  gear is no faster than its own (both worked out on its own ladder from
  the two S/N figures; a figure missing counts as the gear in use): the
  seal if they ask for it; its items if one request lacks every item it
  lacks and wants at least as many symbols; a file if one request wants
  at least as many of its parity pieces. If nothing is left, it stays
  quiet. A request from a
  station that hears the sender better would bring the repair back in a
  gear it may not copy, so then it asks too.
- Its request carries its S/N code for the sender.
- A request is one frame. Files past what fits are asked for next round.
- It rebuilds items from ItemParity, and a file's missing pieces from
  parity pieces, as soon as it holds enough (below). An item rebuilt
  must decode as the item the seal lists at its place, with the digest
  the seal gives; a rebuilt file must match its whole-file digest. A
  rebuilt item is then taken as if heard.
- **No seal at all.** A file heard (its announce, or a piece) with no
  seal over it 3 minutes after the last of it came lost every copy of
  its seal. It asks for that seal by the file, under seal 0, and again
  every 3 minutes, at most 10 times, unless another station's request
  asks for it first. With the announce held, only while the operator
  receives the file; without it, the seal brings the announce too.
- **A round's seal lost.** It asks only when it hears a seal, so a lost
  seal at the end of a round would leave it waiting. A listener that
  still lacks something under a seal, with nothing from the sender and
  no request of its own (or speaking for it) for 3 minutes, asks again.
- It keeps each item while a seal over it is kept, so it never seems to
  lack what it holds.
- It stops asking under a seal after 5 requests in a row that brought
  nothing, and asks for a missed seal at most 5 times.
- A refusal heard stops it asking for what the refusal names, for 2
  hours, and a file it covers shows as not repaired, with the reason;
  Ask again is greyed. Hearing a seal again that was refused whole (the
  operator kept it open after all) lifts that refusal.

### Parity

Repairs are **parity**, worked by a Cauchy Reed-Solomon code over
GF(2^16): the 65,536 16-bit numbers, where adding is XOR and
multiplying is done by log tables on the primitive polynomial
x^16 + x^12 + x^3 + x + 1 (`0x1100B`).

- **Symbols.** The data symbols are byte strings of one even length,
  read as 16-bit numbers, most significant byte first. A shorter one
  counts as padded with zeros. Each has a name y: a file's piece by its
  index, an item by its place in the seal (0 for the first item). A
  parity symbol has a name x that is none of the ys: a file's parity
  piece by its index, which is at least the piece count; an ItemParity
  by 256 + index.
- **The sum.** Parity symbol x is, word by word, the sum over every data
  symbol of d_y × 1 / (x + y).
- **Why.** Any k of the data and parity symbols give back all k data
  symbols, whichever k they are (the code is MDS). So one parity symbol
  fills a different gap for each listener that lacks one, and a round
  sized to the neediest listener serves them all. Rebuilding uses the
  inverse of a Cauchy matrix, which has a closed form, so m missing
  symbols take m² work per word.
- **Files.** A file's pieces are the data. A parity piece is as long as
  the piece size rounded up to even. A rebuilt piece is cut back to its
  own length (the last is short).
- **Items.** The items an ItemParity covers are the data, each as its
  encoded record (type, length, header and body, as it went on the
  air), padded to the longest of them rounded up to even. Its bytes are
  that long. ItemParity over the same seal, the same items and the same
  length rebuild together.
- **Wants.** A listener that lacks n symbols and loses about a share p
  of what is sent asks for
  (n + sqrt(n × p / (1 − p))) / (1 − p), rounded up, with p held at 0 to
  0.75: n, plus enough over for the losses, plus about one standard
  deviation of them, so about nine rounds in ten fill every gap. Before
  any repair round, p is the share of the first pass it lost (for a
  file, counted from the first piece it heard, so a late joiner doesn't
  count what went before it; half when it heard none). After one, p is
  the share of the symbols asked for that didn't come, when some came;
  for a file, of the parity pieces sent since it first asked, counted
  by their index from the first after the highest it held then to the
  highest it has heard, so pieces a round cut short never sent don't
  count as lost (in Jeff's test 4 of 21 came of a round cut at 5
  minutes, and the next request asked for 41).
  A file's want is capped by the sender at four times its pieces.

On paths where some stations lose most of every burst, this matters
most: Ragtime 0.1 sent again the union of every piece anyone lacked,
round after round, and its losers were still missing pieces hours
later. With parity, a file usually reaches every station in a round or
two.

### Broadcast files

- A file is at most 300 KB. Its pieces fill a frame: three codewords in
  the FSK modes, otherwise as much as the 255-byte frame takes, with the
  sender's callsign in each piece's header. A piece's frame is k codewords
  less 3 bytes: COBS's byte, the zero ending the frame, and the zero
  broadcast v3 puts before a frame that starts a codeword
  (`link::framedBytes`, `link::burstDataBytes`). It fills whole codewords
  in v3 and never spills into one more in v2, where it wastes a byte.
- **Bursts stay long.** A burst carries as many pieces as fit 12 s
  and 8 frames (`MAX_BURST_SECONDS`, `MAX_FRAMES_PER_BURST`), whatever
  the path. From Data2G's broadcast v3 (PR
  #121), a lost codeword loses only the frames it touches, so a frame's
  chance doesn't depend on where it sits in the burst, and a longer burst
  pays its control codeword and the 4 s gap below once for more pieces.
  Under v2, a lost codeword lost the rest of its burst, which was why
  shorter bursts on fading paths were considered (ragtime #68), and then
  dropped for v3 (docs/DATA2G.md, "Loss").
- The first pass goes one burst at a time, so others can get a word
  in: the announce, then the pieces, with the announce again ahead of
  every 20 pieces. A burst Not sent goes once more.
- **Room to answer.** A file's burst (and a repair round's) yields: it
  goes no sooner than 4 s after any burst of ours was sent, a chat line
  included, while our own messages may go ahead of it. 4 s is counted
  from the host's Sent, about half a second before its PTT goes off; a
  station answering at once keys about a second in, and its header
  decodes here about a second after that, which holds our next burst.
  After a message addressed to a station, which asks for its Ack, a
  file's bursts also wait until the Ack is heard or a listening window
  in the message's mode has passed (3 one-codeword bursts and 4 s). An
  Ack goes as soon as the station decodes the message. A reply typed
  while another station's burst was on air waits for it to end, then
  takes each of data2g-host's 1 s slots with probability (P + 1) / 256.
  Ragtime sets KISS P to 191 as it connects (the host's own default is
  63): 75% a slot, so 94% of such replies go within two slots, inside
  the gap. The operator sees the estimate (pieces, airtime, when repairs end
  at the latest) before it goes; Ragtime refuses a file that would take
  over 2 hours of airtime and warns over 15 minutes.
- **Stopping a share** ends its first pass, not its repairs. What went is
  sealed, and while that seal is fresh (its sensitivity window, above)
  a request on the group for any of its pieces, sent or not, is answered
  on the group with no question to the operator; so a station part way
  through can finish it. Close repairs ends it (Cancelled in the log).
  Stopped before any burst went, it simply ends.
- A listener shows a heard announce, and receives it if the operator
  presses Receive, or without asking if they turned on Receive broadcast
  files automatically (off by default). Pieces heard before their
  announce are held, up to 2 MB, until it comes.
- A listener that stops receiving keeps what came as Incomplete and asks
  for nothing more. One that has given up asking (5 requests that
  brought nothing, above) shows the file as Incomplete too. Ask again
  starts asking afresh under the file's seal, whatever earlier requests
  brought, as long as that seal was heard in the last 2 hours; it can
  also be asked while the file is still being received. With no such
  seal there is nothing to ask under, and the screen says why; the
  pieces kept still count if the sender broadcasts the file again.
  Rejecting a file throws away what came and is logged as Declined.
- A listener asks for parity pieces, not pieces (Parity, above), and
  keeps the parity pieces it hears, those for other stations included,
  until it holds as many pieces and parity pieces as the file has
  pieces; then the missing ones are rebuilt.
- With every piece in, the whole-file digest is checked. A file that
  fails it is fetched once more from the start; a second failure ends
  it. A file announced again with the same size, piece size, digest and
  name takes over the pieces already heard.
- A file still missing pieces once its seal is over 2 hours old, or 2
  hours 10 minutes after anything new came, is shown as incomplete, and
  kept 7 days. Finished files stay on the screen for 24 hours, in memory
  only.
- A file not yet whole (heard, being received or incomplete) is kept on
  disk too, a file each in the app data folder's `broadcast-parts`, so a
  restart loses neither its pieces nor the seals heard over it: the file
  comes back on the Files tab, and a seal still under 2 hours old is
  asked under again (a stopped file still asks for nothing). Heard and
  never received, it goes 24 hours after its announce; incomplete, 7
  days after it went so; saved or rejected, at once. Nothing read back
  is trusted: a piece that doesn't fit the announce is dropped, and a
  file that doesn't parse, or lacks its closing line, is deleted.

## Growing the protocol

- A receiver skips a record of a type it doesn't know, by its length, and
  keeps it as unknown. New record types need no new version.
- A receiver ignores bytes after the fields it knows in records with a
  fixed layout (Hello, Status, Ack, Ping, Pong, FileOffer, FileAnswer,
  FileSaved, FileCancel, FileAnnounce, Seal, RepairRefused), so new
  fields go on the end.
  Text, FileData, FilePiece, RepairRequest and ItemParity run to the end
  of the record and can't grow that way.
- A new text table takes a new coding number in Text's flags.
- A change an old receiver would misread takes a new version: `0xA1`,
  and Hello's version 1.

## A worked example

AG7EW's 7th item, the text `CQ CQ de AG7EW`, made at 2026-10-22 00:00
UTC (Unix 1792627200), in a broadcast frame of its own:

    a0                      frame byte: Ragtime, version 0
    10                      type: Text
    14                      length: 20 bytes follow
    07 7a 22 d8             AG7EW: (1×40 + 7)×40 + 34 = 0x077a,
                                   (5×40 + 23)×40 + 0 = 0x22d8
    00 00 07                seq 7
    6a d9 52 00             time 1792627200
    00                      flags: table 0, not addressed
    00 c2 1d 32 45 04 27 c0 the text in table 0 (8 bytes for 14 characters)

23 bytes in all. Its record digest is the SHA-256 of `10 077a22d8 000007
6ad95200 0f` followed by the 14 ASCII bytes, cut to 12 bytes:
`8c8208b632620e86b8884f31`. `tests/ProtocolTest.cpp` checks both.
