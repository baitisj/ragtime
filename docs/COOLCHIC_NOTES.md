# Cool-chic: what the port taught us

Notes from porting Cool-chic 5.0.1 to C++ (ragtime PRs #107, #108 and
#109, 2026-10-29) and wiring it into the picture dialog. Each section
starts with what we learned, then what could be done about it. The
bitstream is `docs/PICTURE_CODEC.md`, and the plan was the Claude Doc
"Cool-chic in Ragtime: plan".

## The big ideas

### There is no model, only a fit

JPEG 2000 and the learned codecs we tested (CompressAI) carry their
knowledge in the decoder: a fixed transform, or 57 MB of weights trained
on thousands of photographs. Cool-chic carries none. The sender trains a
tiny network on the one picture it is sending, and sends the network
along with the picture's latent grids.

That turns the usual costs around:

- **Encoding is training.** It takes minutes, not milliseconds: about
  three minutes for 640×480 on four cores in our C++, against 18 minutes
  in Python on two.
- **Decoding is cheap.** At 640×480 the decoder takes about 150 ms for
  the latents and 100 ms for the picture.
- **"Retraining" needs no GPU and no picture collection.** What we called
  retraining for previews is a change to how each picture is fitted
  (below), run on the sending station.

### The weights are a toll paid up front

The network's quantised weights come before any latent, and nothing can
be drawn without them. With the light decoder (`lop`, 505 parameters) the
headers and weights take about 0.4 kB. With `hop` (1,881 parameters)
they take about 1.5 kB. At a 1.6 kB budget, `hop` would spend the whole
budget on the decoder before the first pixel. So the app uses `lop`.

A rough rule from both presets: **about 0.8 bytes a parameter, plus 40
bytes of headers** (`FitEncode.cpp`).

### Coarsest first makes it progressive, almost for free

Cool-chic already wrote its latent grids coarsest first. Two changes
made a cut-off stream useful:

1. **A flush per grid**, behind a small table of run lengths. A receiver
   then knows where each grid ends and decodes each one as soon as it is
   in. This costs 10 to 13 bytes a picture.
2. **The partial rule.** Within the grid being received, a value counts
   only if the decoder read no byte past what has arrived, its 8-byte
   read-ahead included. Bytes a range encoder has written never change,
   so a value decoded that way is the true one, and the grid fills in
   from the top left in a slanting front (Cool-chic's wavefront order).

### Progressive fitting is the "retrain"

A network fitted only to the whole picture has never seen its finest
grids missing. Zero them, and the preview is worse than soft: at 4 kB a
plain fit shows 16 dB from its first 2,000 bytes. Progressive fitting
takes the finest 1 to 3 grids out on half the gradient steps. The
network then learns to make a fair picture from the coarse grids alone,
and the fit moves bytes into them.

| At 4 kB (λ 0.03) | Plain fit | Progressive fit |
|---|---|---|
| First 1,500 bytes | 14.9 dB | 16.0 dB |
| First 2,000 bytes | 16.0 dB | 21.3 dB |
| First 3,000 bytes | 18.8 dB | 22.9 dB |
| Whole | 24.2 dB at 3,778 B | 23.5 dB at 3,307 B |

At matched rates the whole picture gives up about 0.3 dB at 4 kB, and
nothing at 1.6 kB, where nearly all the bytes go into one 60×80 grid
anyway. It is on by default.

### Only the entropy coding has to be exact

A range decoder that rounds one probability differently from the
encoder decodes garbage from that symbol on. Cool-chic 5.0.1 builds its
probabilities in floating point (the constriction coder), so two
machines could disagree. We replaced that part with integers only: a
fixed-point Laplace model from two tables made with exact decimal
arithmetic, frequencies summing to 2^24, and LZMA's range coder widened
to 64 bits. The ARM, which predicts each latent's distribution, was
already fixed point in Cool-chic 5.

Everything after the latents (upsampling and synthesis) runs in float.
Our decoder's pictures differ from Python's by one level in 14 of
921,600 samples, which does no harm, because nothing more is decoded
from pixels.

16-bit frequency totals were tried first. With every symbol needing at
least 1 of 65,536, a grid of nearly all zeros leaked over 100 bytes.
2^24 totals made that leak negligible.

### Rate is not a knob Cool-chic has

Cool-chic takes λ, the weight of the rate in its loss, and the size
falls out of the fit. Ragtime's dialog takes an airtime, which gives a
byte budget. On the Wonder Wheel the latents' bits went as λ^-0.72
(0.071 bits a pixel at λ 0.03, 0.025 at λ 0.13, progressive fit). The
encoder now:

1. guesses λ from that curve and the picture's size, aiming at 94% of
   the budget less the headers and weights,
2. steers λ gently at the validations late in the main phase (from 60%
   to 85% of it), toward the rate that fits, and
3. if the written stream still comes out too big, fits on a short while
   at a λ worked out from the real weights' cost, at most twice.

**The rate mid-fit is a poor guide.** The first try steered from the
start of the main phase, and it went badly wrong. The latents' rate
climbs while they grow from zero under heavy noise, then falls several
times over as the fit settles. At a 1.6 kB budget it went from 2,200
bytes up to 4,000 and then down to 350, while the steering chased it.
λ ended at 1.5, twenty times too high, and the stream came to 739 bytes
of the 1,600 allowed. Steering only once the rate has settled fixed it:

| Budget | Stream | PSNR | λ ended at | Fit |
|---|---|---|---|---|
| 4,000 B | 3,735 B | 23.82 dB | 0.0237 | 189 s |
| 1,600 B | 1,475 B | 21.29 dB | 0.120 | 186 s |

Both are the Wonder Wheel at 640×480 on 4 cores, progressive fitting.
The guess from the curve was within 5% of where the steering ended,
which says more about this picture's being the one the curve came from
than about the curve. A picture with much more or much less detail will
lean on the steering and the retry.

### What the C++ fit taught about speed

- One fitting step at 640×480 went from 350 ms to 160 ms on one thread
  by running the ARM on blocks of 64 latents at a time and the
  upsampling on padded rows. On four threads it takes 69 ms.
- The 2,000 steps break down as a warm-up race of 7 runs of 50 steps
  (5 random starts, then the best 2), 1,150 steps of soft rounding under
  a cosine learning rate, and 500 steps with hard rounding. The warm-up
  is about a sixth of the time.
- Quantising the weights and RDOQ add about 22 s, a tenth of the whole.
  They try every quantisation step for each network on the whole
  picture. They now check for Cancel as they go.
- The C++ fit landed slightly ahead of Python's: 3,752 B at 24.36 dB
  against 3,768 B at 24.17 dB. That is within what a different random
  start gives, so it is not a real gain.
- The hand-written gradients match PyTorch's autograd to 1e-4, and our
  SOAP optimiser matches Cool-chic's to 1e-5 over 25 steps. Those checks
  (`ragtime_coolchic_fit_test`) are what make the fit trustworthy
  without PyTorch.

### A second decoder of bytes from the air

Until now only JPEG 2000 was decoded from the air. The Cool-chic decoder
checks every count and length against hard caps (2048 a side, 12 grids,
7 synthesis layers of at most 64 channels, 300 KB) before it allocates
anything. 30,000 fuzz rounds ran clean under ASan and UBSan, and 1,000
run in ctest each time.

## Potential improvements

Roughly in order of what they would buy.

1. **Smaller weights.** The weights are 0.4 kB of every picture. A
   starting network shipped inside Ragtime, with each picture sending
   only its differences, could halve that. So could coarser weight
   quantisation for the synthesis, which RDOQ already shows tolerates
   it. This is research, but at 1.6 kB it is a quarter of the budget.
2. **Something to see before the weights arrive.** Nothing shows for the
   first 0.4 kB, where JPEG 2000 already shows a soft picture. A few
   dozen bytes of thumbnail (an 8×6 grid of colours) in front of the
   weights would fill that gap. That would be a bitstream change.
3. **A better rate guess.** One λ curve, from one picture, starts every
   fit. A short probe fit (100 steps at two λs) would measure this
   picture's slope before the main fit, and steering could aim for the
   final rate rather than the rate mid-fit.
4. **Faster fits.**
   - SIMD (AVX2, NEON) in the synthesis and the ARM: Cool-chic 4.2.1's C++
     decoder had AVX2 paths for exactly these layers.
   - Fewer warm-up starts (3 then 1) for small budgets, where the start
     matters less.
   - Stop early when the validation loss has stopped improving.
   - Fewer steps for small pictures: the schedule never runs fewer than
     2,000, so a 160×120 picture fits about as long per pixel as a big
     one.
5. **Grey pictures as grey.** A grey picture is fitted as colour, three
   synthesis outputs where one would do. One output channel would save
   synthesis weights and some rate. That needs a header flag the decoder
   honours.
6. **A gentler front within a grid.** The wavefront order fills the grid
   being received from the top left, so a picture cut mid-grid is sharp
   in one corner. Coding the grid in interleaved rows (every 4th row,
   then the ones between) would spread the sharpening over the whole
   picture, at some cost in context.
7. **A perceptual loss.** Cool-chic fits MSE, which favours blur. A loss
   with some SSIM or a gradient term might suit "pretty matters" better
   at these low rates.
8. **The receiver's decode.** Each burst re-runs the whole synthesis
   (about 100 ms at 640×480). Caching the upsampled coarse grids, which
   don't change once complete, would cut that.
9. **Use the spare cores when idle.** Fits run on every core. A low
   thread priority would keep the rest of the desktop smooth while one
   runs.

## In the app (2026-10-29)

- The picture dialog has a Codec choice: JPEG 2000 (made at once) or
  Cool-chic (sharper, takes minutes), remembered between pictures.
- Cool-chic fits on its own thread, aimed at the bytes the airtime
  allows. The preview shows the fit's picture about once a second, so
  the picture is seen forming. The bar under it fills from the right
  over a percent scale (0% on the right, 100% on the left), with grey
  lines walking across the part done. Cancel stops the fit after asking,
  as does closing the dialog. The settings are held while it fits.
- When the fit ends, the preview plays the picture arriving, as for
  JPEG 2000: weights first (a blank), then the grids coarsest first.
- On the air a Cool-chic picture is kind `0x02`. A receiver tells the
  codecs apart by the stream's first bytes (a JPEG 2000 codestream
  starts with SOC and SIZ), and Ragtime 0.2 shows a `0x02` picture as a
  plain file.
- Orange's BSD-3 notice must go in the release notes, or the About box,
  once pictures ship.
