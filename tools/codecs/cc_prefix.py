# cc_prefix.py BITSTREAM ORIGINAL.png OUTDIR: decode a Cool-chic bitstream as
# if it had been cut off. The finest K latent grids are taken as zero, as a
# receiver that has only the coarse grids would, for K = 0, 1, 2 and so on.
# Also counts the bytes each grid takes, coded on its own, so each cut has a
# byte count. Run from a Cool-chic checkout with PYTHONPATH=. (it patches
# Cool-chic's decoder from outside; nothing in the checkout changes).
import os
import sys

import numpy as np
import torch
from PIL import Image

import coolchic.bitstream.component.coolchic as cc
import coolchic.component.core.upsampling as upsampling
from coolchic.bitstream.component.rangecoder import RangeCoder
from coolchic.bitstream.decode import decode_video

state = {"zero": 0, "sizes": [], "bytes": []}

# Each latent grid as it is decoded, coarsest first: code it again with a
# fresh range coder to count its bytes.
decode_latent = cc.entropy_coding_latent_arm


def counting(encoder_data, ctx, dim, w, b, sw, sb, rc, mode, n_spatial_context):
    out = decode_latent(encoder_data, ctx, dim, w, b, sw, sb, rc, mode, n_spatial_context)
    if mode == "decode":
        fresh = RangeCoder()
        decode_latent(out.clone(), ctx, dim, w, b, sw, sb, fresh, "encode", n_spatial_context)
        state["bytes"].append(len(fresh.get_bitstream_bytes()))
        state["sizes"].append(tuple(dim))
    return out


cc.entropy_coding_latent_arm = counting

# The grids going into the synthesis, finest first: zero the finest K.
upsample = upsampling.Upsampling.forward


def zeroing(self, decoder_side_latent):
    k = state["zero"]
    grids = [torch.zeros_like(x) if i < k else x for i, x in enumerate(decoder_side_latent)]
    return upsample(self, grids)


upsampling.Upsampling.forward = zeroing


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 10 * np.log10(255.0**2 / mse)


def decode(k, outdir):
    state["zero"] = k
    state["sizes"].clear()
    state["bytes"].clear()
    path = os.path.join(outdir, f"k{k}.png")
    decode_video(bits, decoded_path=path, verbosity=0)
    return path, np.array(Image.open(path).convert("RGB"))


bits, ref_path, outdir = sys.argv[1:4]
os.makedirs(outdir, exist_ok=True)
ref = np.array(Image.open(ref_path).convert("RGB"))
total = os.path.getsize(bits)

path, img = decode(0, outdir)
sizes, per_grid = list(state["sizes"]), list(state["bytes"])
print(f"file {total} B; latents {sum(per_grid)} B; header and weights about {total - sum(per_grid)} B")
print("grids, coarsest first: " + ", ".join(f"{h}x{w} {n} B" for (h, w), n in zip(sizes, per_grid)))
print(f"whole: {total} B, PSNR {psnr(img, ref):.2f} dB -> {path}")

# Hyperlatent grids (the coarse pairs) feed only the entropy model, so the
# synthesis has fewer grids than the bitstream: stop when a cut changes
# nothing. The byte counts hold while the k finest grids are synthesis
# grids, which they are for the first few.
last = None
for k in range(1, len(sizes) + 1):
    path, img = decode(k, outdir)
    if last is not None and np.array_equal(img, last):
        break
    last = img
    have = total - sum(per_grid[len(per_grid) - k:])
    print(f"without the {k} finest: about {have} B, PSNR {psnr(img, ref):.2f} dB -> {path}")
