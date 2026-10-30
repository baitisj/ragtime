# Cool-chic test vectors

Test vectors for the Cool-chic decoder (docs/PICTURE_CODEC.md), made by
`tools/codecs/rcc/make_vectors.sh` from the reference code in
`tools/codecs/rcc/`.

The picture is our own: `tiny-source.png`, 160×120, drawn by
`tools/codecs/rcc/make_tiny.py`. Cool-chic 5.0.1 fitted it with the light
decoder at λ 0.01 (`cc_encode.py ... --lmbda 0.01 --n_itr 2000
--dec_cfg_residue cfg/dec/intra/lop.cfg`).

| File | What |
| --- | --- |
| `tiny.rcc` | The stream, 884 bytes: 450 of headers and weights, then the latent part |
| `tiny.rcc.latents.txt` | Its integer latents, coarsest grid first: `grid <index> <height> <width>`, then the values row by row (index 0 is the finest) |
| `tiny.png` | The whole stream decoded by the Python reference, 33.58 dB against the source |
| `tiny-530.png`, `tiny-663.png`, `tiny-795.png` | The first 530, 663 and 795 bytes decoded as a partial stream: 14.54, 19.52 and 22.22 dB |
| `tiny.manifest` | For each decode, how many values of each grid were good (decoded from bytes that had arrived) |

A C++ decoder should get every latent exactly, the same good counts for
each cut, and pictures within a level or so of these. Synthesis in
floating point can differ by one here and there.

At 265 and 397 bytes the weights haven't all arrived, so there is nothing
to show.
