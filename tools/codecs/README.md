# Picture codecs at 1.6 and 4 kB

The scripts and results of a test run on 2026-10-29: could anything beat
the JPEG 2000 that Ragtime sends its inline pictures in (docs/STAGE1.md,
"Inline pictures") at the sizes a picture has on HF? The write-up, with
the pictures side by side, is the Claude Doc "Picture codecs at 1.6 and
4 kB" (https://claude.ai/artifact/SPsYp1d99sL7QzrW1kVG3v, private to
Jeff). Nothing here is built by CMake or run in CI. It is a record that
anyone can run again.

## The picture and the budgets

Every codec gets the same picture: `wonder_wheel.jpg` from Andrew's
SSTVAE (https://github.com/arodland/SSTVAE, commit `cc06e87`, v0.6.3), a
640×480 photo of the Wonder Wheel's neon sign. It is under SSTVAE's
Artistic License 2.0, so it isn't copied here: clone SSTVAE to get it.

The budgets are 1,638 bytes (1.6 kB, about 0.04 bits a pixel) and 4,096
bytes (4 kB, about 0.11 bits a pixel), roughly what 60 s takes at a low
and a middling Data2G gear. Each codec takes the best picture that fits.
Where a codec does better by coding a smaller picture and scaling it back
up to 640×480, the scripts try 1/1.5, 1/2 and 1/3 too and keep the best.
The scores are PSNR and SSIM against the original at 640×480
(`metrics.py`).

## Setting up

    git clone https://github.com/arodland/SSTVAE        # the picture
    git clone https://github.com/Orange-OpenSource/Cool-Chic   # commit a6fe38a, v5.0.1
    python3 -m venv venv && . venv/bin/activate
    pip install imagecodecs numpy pillow scikit-image
    pip install torch compressai                         # several GB
    pip install einops fvcore ConfigArgParse constriction==0.4.2 scipy matplotlib  # Cool-chic
    convert SSTVAE/wonder_wheel.jpg ww.png                # ImageMagick
    convert SSTVAE/wonder_wheel.jpg ww.ppm

The run used Python 3.13, torch 2.15 (from PyPI: download.pytorch.org is
blocked in the cloud sandbox), CompressAI 1.2.8, imagecodecs 2026.10.10
(libjxl 0.12.0), OpenJPEG 2.5.0 and Cool-chic 5.0.1. Everything ran on
four CPU cores, with no GPU.

## The scripts

| Script | What it does |
| --- | --- |
| `j2k_at.cpp` | Ragtime's own `image::encode` at a budget, the codestream Ragtime sends, then `image::decode` |
| `jxl_at.py` | JPEG XL at a budget |
| `cai_at.py` | Four CompressAI learned codecs at budgets |
| `coolchic.sh` | Cool-chic fitted to the picture at one λ |
| `cc_prefix.py` | A Cool-chic bitstream decoded as if cut off, for a progressive preview |
| `cc_encode_prog.py` | Cool-chic's encoder, fitted so that a cut-off stream still gives a fair picture |
| `metrics.py` | PSNR and SSIM of decoded pictures against the original |
| `grid.sh` | Captioned pictures two to a row, as in the doc |

### `j2k_at.cpp`: JPEG 2000

    R=/path/to/ragtime   # built with pictures on: build/image/libragtime_image.a
    g++ -std=c++17 -O2 -I$R/image/include -I$R/core/include -I$R/build/core/include \
        j2k_at.cpp $R/build/image/libragtime_image.a $R/build/core/libragtime_core.a \
        $(pkg-config --libs libopenjp2 libwebp libheif) -ldl -o j2k_at
    ./j2k_at ww.ppm 4096 ww_j2k_4096.j2k ww_j2k_4096.ppm

`j2k_at IN.ppm BYTES OUT.j2k OUT.ppm` codes a binary PPM in at most BYTES,
exactly as the picture dialog does (progressive quality layers, and as
many resolutions as suit the size), writes the codestream and decodes it
again. It prints the size, the layers and the resolutions. For a smaller
picture, shrink the PPM first (`convert ww.ppm -filter Lanczos -resize
320x240! ww_s2.ppm`) and scale the decoded one back up the same way.

### `jxl_at.py`: JPEG XL

    python jxl_at.py ww.png 1638 4096

This searches libjxl's distance (its quality knob) by halving, at effort
9, for the largest picture under each budget, at full size and at 1/2,
1/3 and 1/4. It writes `out/ww_jxl_<budget>_s<scale>.jxl` and `.png`.
libjxl can't go under about 6.5 kB at 640×480 even at its coarsest
distance, 25, so at these budgets only the shrunk pictures fit.

### `cai_at.py`: CompressAI

    python cai_at.py ww.png 1638 4096 > out/cai.log

This runs four pretrained models from CompressAI's zoo:
bmshj2018-hyperprior, mbt2018-mean, mbt2018 and cheng2020-attn. It tries
every quality at each scale and keeps the best SSIM under each budget.
The byte counts are real entropy-coded strings plus 6 bytes for a header
(model, quality, size). The weights download from compressai.s3 the first
time (Cheng 2020 at quality 1 is 57 MB). It writes
`out/ww_cai_<model>_<budget>.png`, and the log has every trial.

### `coolchic.sh`: Cool-chic

    COOLCHIC=Cool-Chic ./coolchic.sh ww.png 0.03 lop cc/lop03.cool

Cool-chic is an overfitted codec. It fits a small network and its latent
grids to this one picture by gradient descent, and sends the network's
weights in the bitstream. λ trades bytes for quality: bigger is smaller.
The last argument is Cool-chic's decoder size, from `lop` (light, about
0.4 kB of weights) to `hop` (about 1.5 kB of weights at these sizes). One
fit is 2000 steps and takes about 17 minutes on two CPU cores. The script
writes OUT.cool, OUT.log (the fit, step by step), OUT.png (decoded from
the bitstream) and OUT.work (Cool-chic's working folder), then prints the
size, the weights' and latents' share in bits a pixel, and the PSNR.

### `cc_prefix.py`: Cool-chic cut off

    cd Cool-Chic && PYTHONPATH=. python ../cc_prefix.py ../cc/l02.cool ../ww.png ../cc/prefix

Cool-chic writes its latent grids coarsest first, so a bitstream cut off
part way still holds the coarse grids whole. This script decodes a
bitstream with the finest one, two, three and more grids taken as zero.
That is what a receiver could show from a cut-off stream with no change
to Cool-chic's fitting. It prints each grid's size and bytes, and then for
each cut the bytes needed and the PSNR. The pictures go to
`OUTDIR/k<cut>.png`. It patches Cool-chic's decoder from outside, so the
checkout stays untouched.

### `cc_encode_prog.py`: Cool-chic fitted for a progressive preview

    cd Cool-Chic && PYTHONPATH=. python ../cc_encode_prog.py -i ../ww.png -o ../cc/prog03.cool \
        --workdir ../cc/wp3 --lmbda 0.03 --n_itr 2000 --dec_cfg_residue cfg/dec/intra/lop.cfg

This takes `cc_encode.py`'s arguments. On half the training steps
(`PROG_SHARE`, 0.5 by default), the finest 1 to `PROG_MAX` (3) synthesis
grids are taken as zero before the picture is made. The network therefore
learns to give a fair picture from the coarse grids alone. The rate counted
is always the whole bitstream's, and validation and the bitstream are the
whole picture's. Compare the result's cut-off pictures (`cc_prefix.py`) with
a plain fit's. No GPU and no picture collection is needed: Cool-chic has no
pretrained model, and every picture is fitted from scratch.

### `metrics.py` and `grid.sh`

    python metrics.py SSTVAE/wonder_wheel.jpg out/*.png
    ./grid.sh grid.jpg "Original" ww.png "JPEG 2000, 4060 bytes" out/ww_j2k_4096.png

### `rcc/`: Ragtime's Cool-chic stream

The reference for docs/PICTURE_CODEC.md: Cool-chic 5.0.1's bitstream with
its latents coded by an integer range coder, one flushed run per grid, so
every machine decodes the same values and a cut-off stream decodes as far
as it goes. `rcc.py` is the coder (`test_rcc.py` tests it), `tables.py`
writes its tables, `rcc_cool.py` plugs it into Cool-chic, `rcc_write.py`
writes a fitted picture as a stream, `rcc_read.py` decodes a stream or its
first N bytes, and `make_vectors.sh` (with `make_tiny.py`'s picture) makes
`tests/data/coolchic/`.

    cd Cool-Chic && export PYTHONPATH=.:../rcc
    python ../rcc/rcc_write.py ../cc/w4 ../ww.png ../lop03.rcc
    python ../rcc/rcc_read.py ../lop03.rcc ../lop03-2000.png 2000

## Results

PSNR in dB, SSIM from 0 to 1, both higher-is-better. Decode times are on
one CPU core. Cool-chic's times are its Python decoder, and C would be
far quicker.

| Budget | Codec | Coded at | Bytes | PSNR | SSIM | Decode |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| 4 kB | Cool-chic, mop, λ 0.035 | 640×480 | 3,751 | 24.30 | 0.773 | 1.4 s |
| 4 kB | Cool-chic, lop, λ 0.03 | 640×480 | 3,768 | 24.17 | 0.771 | 1.1 s |
| 4 kB | CompressAI Cheng 2020 | 427×320 | 3,646 | 24.73 | 0.799 | 2.3 s |
| 4 kB | CompressAI mbt2018 | | 3,458 | 24.16 | 0.780 | |
| 4 kB | CompressAI mbt2018-mean | | 3,742 | 23.88 | 0.770 | |
| 4 kB | CompressAI bmshj2018 | | 3,794 | 23.86 | 0.769 | |
| 4 kB | JPEG 2000 (Ragtime) | 640×480 | 4,060 | 22.32 | 0.688 | 6 ms |
| 4 kB | JPEG XL | 213×160 | 4,087 | 21.84 | 0.691 | 2 ms |
| 1.6 kB | CompressAI Cheng 2020 | 213×160 | 1,554 | 21.80 | 0.674 | 0.9 s |
| 1.6 kB | Cool-chic, lop, λ 0.13 | 640×480 | 1,573 | 21.38 | 0.636 | 1.1 s |
| 1.6 kB | CompressAI mbt2018 | | 1,410 | 21.11 | 0.639 | |
| 1.6 kB | CompressAI mbt2018-mean | | 1,494 | 20.99 | 0.634 | |
| 1.6 kB | CompressAI bmshj2018 | | 1,486 | 20.95 | 0.633 | |
| 1.6 kB | JPEG 2000 (Ragtime) | 640×480 | 1,525 | 19.75 | 0.560 | 8 ms |
| 1.6 kB | JPEG XL | 213×160 | 1,625 | 19.34 | 0.565 | 4 ms |
| 5 kB | Cool-chic, hop, λ 0.02 | 640×480 | 5,155 | 25.94 | 0.819 | 2.1 s |
| 1.8 kB | Cool-chic, lop, λ 0.10 | 640×480 | 1,831 | 22.01 | 0.668 | 1.2 s |

The last two rows miss the budgets but show where Cool-chic stands: the
big `hop` decoder spends about 1.5 kB on its weights.

Cool-chic cut off (`cc_prefix.py` on the 5,155-byte `hop` bitstream).
Its first 1.5 kB are weights, so nothing shows before them:

| Kept | Bytes | PSNR |
| --- | ---: | ---: |
| Everything | 5,155 | 25.94 |
| All but the finest grid (480×640) | about 4,600 | 23.67 |
| All but the two finest (down to 120×160) | about 3,700 | 17.13 |
| All but the three finest (down to 60×80) | about 2,000 | 14.21 |

Cool-chic fitted for a progressive preview (`cc_encode_prog.py`, lop),
in Ragtime's stream (`rcc/`), cut at the same byte counts as plain fits:

| Fit | Whole | 2,000 B | 2,500 B | 3,000 B | 3,400 B |
| --- | ---: | ---: | ---: | ---: | ---: |
| Plain, λ 0.03 | 3,778 B, 24.17 dB | 16.0 | 17.1 | 18.8 | 20.9 |
| Progressive, λ 0.03 | 3,307 B, 23.53 dB | 21.3 | 22.0 | 22.9 | |
| Progressive, λ 0.022 | 4,063 B, 24.13 dB | 20.6 | 21.7 | 22.4 | 23.0 |

At 4 kB the whole progressive picture needs about 7% more bytes for the
same quality, and is 2 to 5 dB better for most of the way in. At 1.6 kB
it costs nothing (λ 0.10: 1,548 B, 21.47 dB, against the plain fit's
1,586 B, 21.38 dB). Both fits put nearly everything into one grid there.

`results/` keeps the bitstreams: Ragtime's JPEG 2000 and JPEG XL at both
budgets (`.j2k`, `.jxl`), the Cool-chic runs (`.cool`, decoded with
`cc_decode.py`), and `cai.log`. CompressAI's strings weren't kept, but
the log has every trial.

## What it showed

- The learned codecs beat JPEG 2000 by about 2 dB at both budgets.
  CompressAI's Cheng 2020 was the best of the pretrained ones, and
  Cool-chic came within half a dB of it, with no pretrained model.
- JPEG XL is no help this small. It only matched JPEG 2000, and only by
  coding a third-size picture.
- **CompressAI's catch:** every station would need the same 57 MB of
  weights, and a bit-exact entropy decoder on every machine. It isn't
  progressive, so nothing shows until the last byte.
- **Cool-chic's catch:** a fit takes a quarter of an hour of CPU, and the
  weights cost bytes up front. Its decoder is small and integer-exact,
  and the grids come coarsest first, so it could be made progressive
  (fitted so that every cut decodes well, with the range coder flushed at
  each grid).
- C3 (DeepMind, https://github.com/google-deepmind/c3_neural_compression,
  commit `e63e751`) is the same idea in JAX. It wasn't run.
