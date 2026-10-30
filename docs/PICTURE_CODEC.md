# Cool-chic pictures (draft)

**Status:** a draft for review. Nothing here goes on the air yet. Today
Ragtime sends only JPEG 2000 pictures (docs/PROTOCOL.md, "Pictures"). This
page specifies a second kind, built on Orange's Cool-chic 5.0.1, for the
C++ decoder and encoder now being written. The plan is the Claude Doc
"Cool-chic in Ragtime: plan"
(https://claude.ai/artifact/BtTekxxQmqdf9S1iV1eCo5, private to Jeff), and
`tools/codecs/README.md` holds the tests that led to it.

## Why

At the sizes a picture has on HF, Cool-chic beats our JPEG 2000 by 1.6 dB
at 1.6 kB and 1.9 dB at 4 kB. It has no pretrained model: the sender fits
a small network and its latent grids to the one picture, and sends the
network's weights in the stream. The receiver's decoder is a few thousand
multiplies per pixel. The latent grids go coarsest first, so a stream cut
off part way still makes a picture: the preview grows as the bursts come,
as it does with JPEG 2000.

## The stream

A Ragtime Cool-chic picture is a Cool-chic 5.0.1 bitstream (Orange's
`coolchic/bitstream/`, commit `a6fe38a`) for a single intra frame, with
**one change**: the latent part. In order:

1. Cool-chic's video header, frame header and Cool-chic header, bit for
   bit as `coolchic/bitstream/header/` writes them for one RGB 8-bit intra
   picture.
2. The network weights, as `coolchic/bitstream/neuralnet/` writes them:
   quantised integers, exp-Golomb coded, `nn_n_bytes` long.
3. The latent part, `n_bytes_latent` long (the Cool-chic header's field,
   which now counts this part as below):
   - **the grid table:** for each latent grid, coarsest first (Cool-chic's
     decode order, `idx_latent` from `n_latent_grids - 1` down to 0), the
     length in bytes of its run, as an unsigned LEB128 varint (7 bits a
     byte, low first, high bit set on every byte but the last);
   - **the runs**, in the same order: each grid's latents, range coded on
     their own (below) and flushed.

The header and weights come first, so a receiver can show nothing until
they have arrived: about 0.4 kB with the light decoder (`lop`). After
them, each grid can be decoded as soon as its run is in, and the grid
being received can be decoded as far as its bytes go.

What a stream costs over Cool-chic's own: the grid table (about one byte
a grid) and the flushes (a byte or two a grid). The Wonder Wheel at
1,573 bytes became 1,586, and 3,768 became 3,778.

## Latent coding

Everything here is integer arithmetic, so every machine decodes the same
values. Cool-chic 5.0.1 used the constriction range coder, whose
probabilities are built in floating point. A station that rounded one
differently would decode garbage from that point on.

### The model for each value

The ARM (auto-regressive module), as Cool-chic 5.0.1 runs it in fixed
point (`coolchic/bitstream/component/armint.py`, `fixed_point_arm`, int64
throughout), gives each latent a pair of indices from its already decoded
neighbours and, through the IFCE, from the coarser grids:

- **mu**, the mean: index `m`, mu = -64 + m/256. It is clamped to
  0..32767.
- **scale**, the Laplace width b: index `s`, ln b = -5 + s/256. It is
  clamped to 0..2560.

These are `mu_scale - MU_LOG_SCALE_MIN_FIXED_POINT`, exactly as in
Cool-chic's `latent.py`. The values coded are in [-64, 63] (Cool-chic's
`AC_MAX_VAL` = 64), so there are 128 symbols, index `i` = value + 64.

### Frequencies

The frequencies of the 128 symbols sum to 2^24. Each symbol gets at least
1, so any value can be coded, and the rest follow the Laplace
distribution. Two tables, `K` (2,561 entries) and `P` (1,024 entries),
are in `tools/codecs/rcc/tables.txt`. They are written by `tables.py`
with exact decimal arithmetic and taken as they are:

    K[s] = round(2^32 / (256 · e^(-5 + s/256) · ln 2))      (fits in 32 bits)
    P[f] = round(2^31 · 2^(-f/1024))                         (P[0] = 2^31)

The half tail, for a distance d from mu in 1/256ths:

    e  = |d| · K[s]               (64 bits: |d| < 2^16, K < 2^32)
    ip = e >> 32
    T(d) = 0                          if ip ≥ 24
         = P[(e >> 22) & 1023] >> (8 + ip)   otherwise

T is half of e^(-|d|/b), in 1/2^24ths. The CDF at d is
`G(d) = T(d)` for d < 0, and `2^24 - T(d)` for d ≥ 0.

The cumulative frequency below symbol index i, for i = 0..128:

    C(0)   = 0
    C(128) = 2^24
    C(i)   = i + (((2^24 - 128) · G(d_i)) >> 24),   with
    d_i    = (i - 64) · 256 - 128 - (m - 16384)     (the edge below value i - 64, from mu)

Symbol i has frequency C(i+1) - C(i), which is at least 1.

### The range coder

This is LZMA's range coder widened to a 64-bit range.

**Encoder state:** `low` (64 bits and a carry), `range` (64 bits, starts
at 2^64 - 1), `cache` (a byte, starts 0), `cache_size` (starts 1), and the
output.

To code symbol i:

    r      = range >> 24
    low   += r · C(i)
    range  = r · (C(i+1) - C(i))
    while range < 2^56: range = (range << 8) mod 2^64; shift_low()

`shift_low()`:

    if low < 0xFF·2^56 or low ≥ 2^64:
        carry = low >> 64 (0 or 1)
        out (cache + carry) & 0xFF, then cache_size - 1 bytes of (0xFF + carry) & 0xFF
        cache = (low >> 56) & 0xFF
        cache_size = 0
    cache_size += 1
    low = (low mod 2^56) << 8

**Finishing a run:**
1. Pick the value v in [low, low + range) with the most zero bits at the
   bottom: for b from 64 down, v = (low + 2^b - 1) with its low b bits
   cleared, and take the first v < low + range.
2. Set low = v and call `shift_low()` nine times.
3. Drop the first byte written, which is always 0.
4. Strip trailing zero bytes; the decoder reads zeros past the end.

A run of values that are certain costs a byte or nothing.

**Decoder:** `range` = 2^64 - 1, and `code` = the run's first 8 bytes,
big-endian. Bytes past the end of the run read as 0. To decode:

    r = range >> 24
    v = min(code / r, 2^24 - 1)               (integer division)
    i = the symbol with C(i) ≤ v < C(i+1)     (binary search over 0..127)
    code  -= r · C(i)
    range  = r · (C(i+1) - C(i))
    while range < 2^56: range = (range << 8) mod 2^64; code = ((code << 8) | next byte) mod 2^64

The value is i - 64. A corrupt stream decodes to wrong values but never
leaves these bounds.

### The order of values in a grid

The order is Cool-chic 5.0.1's wavefront (`latent.py`,
`generate_coding_order` and the index arithmetic after it). With its
9×9 context mask, each row runs 10 steps behind the row above. Within a
step, the values go from the top row down. A run cut short therefore
fills its grid from the top-left corner, in a slanting front.

## Partial streams

Bytes arrive in stream order. A receiver with the first N bytes:

1. If N doesn't reach the end of the weights, it shows nothing.
2. Reads the grid table, if it has arrived whole. If it hasn't, all the
   latents are zero.
3. Decodes each grid in order. A grid whose run has fully arrived decodes
   exactly. In the grid being received, a value is good if the decoder
   had read no byte past N when it decoded it (counting the 8 bytes read
   at the start). The first value that isn't good, and everything after it
   in that grid and in every finer grid, is taken as 0. Bytes a range
   encoder has written never change, so a good value is the true one.
   The decoder reads 8 bytes ahead, so the grid being received shows
   its values about 8 bytes after they were sent.
4. Runs the upsampling and synthesis on the grids as they stand.

The C++ decoder should keep the decoded latents and its decoder state
between bursts, so that each new burst decodes only the new values. The
synthesis runs again for each new preview.

## The picture

Upsampling and synthesis are Cool-chic 5.0.1's: the same layers, the same
weights after the inverse quantisation (`q_step`), and the same final
steps in `decode.py` and `io/format/png.py`. The output is RGB, clamped
to [0, 1], times 255, rounded half to even (NumPy's `round`). These can
run in floating point. A pixel off by one between machines does no harm,
since nothing more is decoded from the picture.

## Limits

Reader and writer both refuse a stream outside these limits:

- **Picture:** width and height 1 to 2048 pixels and at most 2048×1536
  pixels in all (the float synthesis takes a few hundred MB there), RGB,
  8 bits, one intra frame, no common randomness.
- **Network:** at most 12 latent grids, 7 synthesis layers (the header has 3 bits) of at most 64
  channels, and 4 ARM hidden layers. Spatial contexts are at most 40
  (Cool-chic's mask).
- **Lengths:** `nn_n_bytes` and every run must fit inside the stream; the
  whole picture is under Ragtime's 300 KB cap.
- A stream that ends early, or a table that claims more than the stream
  holds, is a partial stream, never a crash.

## Reference code and test vectors

- `tools/codecs/rcc/rcc.py` is the latent coding above, in plain Python
  integers, with `test_rcc.py`.
- `rcc_cool.py` plugs it into Cool-chic 5.0.1 from outside.
- `rcc_write.py` writes a fitted picture (a Cool-chic work folder) as a
  stream and dumps its latents.
- `rcc_read.py` decodes a stream, or its first N bytes.
- `tests/data/coolchic/` holds a small picture of our own, with its
  streams, latents and decoded pictures (see its README), for the C++
  tests.

## Licence

Cool-chic is Orange's, under BSD-3. C++ ported from it keeps Orange's
notice and sits in `image/src/coolchic/` with the licence beside it. The
range coder and the tables are new and MIT. (Jeff to confirm.)
