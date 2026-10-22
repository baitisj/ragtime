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

data2g-host has three TCP ports (Glissando's `docs/DATA2G.md` describes
the command lines):

| Port | Default | Carries |
| --- | --- | --- |
| KISS | 8100 | Broadcast frames on the `D2G` group's KISS port, sent as ACKMODE frames so the host reports each one sent |
| Commands | 8300 | `MYCALL`, `LISTEN`, `BW500`/`BW2300`, `BCAST OPEN D2G FROM <call>`, `MODES`, `BCAST MODE`, `CONNECT`, `DISCONNECT`, `ABORT`, and the host's reports |
| Session data | 8301 (commands + 1) | A session's byte stream, both ways, once `CONNECTED` |

**A broadcast frame** is the byte `0xA0` followed by whole records, as
many as fit. In version 0 a frame is at most 255 bytes, the size
Glissando's Data2G frames were on the air; larger frames wait on a word
from Andrew about the host's limits. Frames aren't padded: Data2G carries
each frame's length, so the frame ends at its last byte. A record never
spans two frames.

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
| `0x20` | FileOffer | size, digest, name | **yes** |
| `0x21` | FileAnswer | offer, answer, [from] | no |
| `0x22` | FileData | offset, bytes | part of its offer |
| `0x23` | FileSaved | offer | no |
| `0x24` | FileCancel | offer, reason | no |
| `0x30` | FileAnnounce | size, piece size, digest, name | **yes** |
| `0x31` | FilePiece | index, bytes | part of its announce |
| `0x40` | Seal | number, previous, first item, count, digests | no |
| `0x41` | RepairRequest | to, seal, seal digest, flags, items, files | no |

Square brackets mark optional trailing fields: a record may end before
them.

**Hello** (`0x01`) opens every session stream, and may go in a broadcast
frame as a check-in.

| Field | Bytes | Meaning |
| --- | --- | --- |
| version | 1 | The protocol version the sender speaks: 0 |
| flags | 1 | Bit 0: accepts sessions. Bit 1: automatic (nobody at the controls; Stage 2 gives it its rules). Other bits 0 |
| takes | 1 | The records the sender takes. Bit 0: Text and Ack. Bit 1: Status. Bit 2: session files (FileOffer to FileCancel). Bit 3: broadcast files (FileAnnounce, FilePiece). Bit 4: Seal and RepairRequest. Version 0 sends `0x1F` |
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
piece's start; the last may be short.

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

**RepairRequest** (`0x41`): a listener's word that it lacks something a
seal covers.

| Field | Bytes | Meaning |
| --- | --- | --- |
| to | 2 to 8 | Whose seal |
| seal | 2 | Its number |
| seal digest | 12 | Its record digest; zeros when asking for a seal not heard |
| flags | 1 | Bit 0: send the seal itself again |
| count | 1 | Items in the seal (0 when it wasn't heard) |
| items | count / 8, rounded up | One bit per item, the first item in the top bit of the first byte: 1 = missing |
| files | the rest | For each file with pieces missing: its seq (3), then a piece list |

A **piece list** opens with its encoding byte. Encoding 0 is a bitmap:
first piece (2), span (2), then span bits, the first piece in the top
bit, 1 = missing. Encoding 1 is ranges: count (1), then each range's
first piece (2) and length minus one (1). The encoder picks the shorter.

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

Data2G delivers the session's bytes in order and acknowledges them: the
host's `BUFFER <n>` report is the bytes written that the far end has not
yet acknowledged. A station takes its own writes up to the total written
minus n as delivered.

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
   the sender's cancel still counts as delivered.
7. A session ending under a transfer fails it. The receiver keeps the
   part file, for 7 days and at most 8 of them, so the same file from the
   same station carries on from it next time.

The receiver saves under a safe form of the offered name: its last path
part only, without control or direction characters, path separators,
Windows' forbidden characters, leading or trailing dots and spaces, or a
Windows device name, cut to 200 bytes at a character boundary. Auto-accept
never overwrites a file; it numbers the name instead.

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
  anyway.
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

### Seals and repair

Version 0 defines the records; package H builds the behaviour, and
`docs/PLAN.md` ("Seals, receipts and repairs") has the reasoning.

- A station sends a **Seal** of the items it broadcast since its last
  one: at most every 10 minutes, with its CW ID, and whenever it stops
  sending for a while (the end of an over, or of a file's pieces).
- After a seal it leaves a **listening window**: 3 × the airtime of a
  one-codeword burst in its mode, plus 4 s.
- A listener that lacks something the seal lists, or the seal itself
  (each seal names the previous one), waits a random time within the
  window and sends one **RepairRequest**, unless it hears another that
  already asks for everything it lacks.
- The station resends the union of what was asked, in sequence order,
  then opens another window. It stops after a window with no requests, or
  once the seal leaves its **sensitivity window**: the last 3 seals, and
  never less than 4 × the airtime of the items sealed, up to 2 hours.

The multipliers and the cap are open for Jeff (PLAN.md).

## Growing the protocol

- A receiver skips a record of a type it doesn't know, by its length, and
  keeps it as unknown. New record types need no new version.
- A receiver ignores bytes after the fields it knows in records with a
  fixed layout (Hello, Status, Ack, FileOffer, FileAnswer, FileSaved,
  FileCancel, FileAnnounce, Seal), so new fields go on the end.
  Text, FileData, FilePiece and RepairRequest run to the end of the
  record and can't grow that way.
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
