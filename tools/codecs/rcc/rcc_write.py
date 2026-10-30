# rcc_write.py WORKDIR PICTURE.png OUT.rcc: writes a picture Cool-chic has
# already fitted (cc_encode.py or cc_encode_prog.py, WORKDIR holding
# 0000-frame_encoder.pt) as a Ragtime Cool-chic bitstream: Cool-chic 5.0.1's,
# with rcc.py's latent coding (docs/PICTURE_CODEC.md). Also dumps the
# integer latents, coarsest grid first, to OUT.latents.txt, for the C++
# tests. Run from a Cool-chic checkout with PYTHONPATH=.:<this folder>.
import argparse
import os
import sys

import torch

import rcc_cool  # noqa: F401  (plugs the latent coding in)
from coolchic.bitstream.component.constants import AC_MAX_VAL
from coolchic.bitstream.encode import encode_frame
from coolchic.component.frame import load_frame_encoder
from coolchic.utils.codingstructure import CodingStructure
from coolchic.utils.parsecli import get_coding_structure_from_args

workdir, picture, out = sys.argv[1:4]
args = argparse.Namespace(input=picture, intra_pos="0", p_pos="", n_frames=1, frame_offset=0)
structure = CodingStructure(**get_coding_structure_from_args(args))
encoder = load_frame_encoder(os.path.join(workdir, "0000-frame_encoder.pt"))
data = encode_frame(encoder, out, structure)

cc_enc = encoder.coolchic_enc["residue"]
latent = cc_enc.get_quantize_latent(
    quantizer_noise_type="none", quantizer_type="hardround", AC_MAX_VAL=AC_MAX_VAL
)
with open(out + ".latents.txt", "w") as f:
    f.write("# grid index (0 finest), height, width, then the values row by row\n")
    for i in range(len(latent) - 1, -1, -1):
        g = latent[i].to(torch.int64).view(latent[i].shape[-2:])
        f.write(f"grid {i} {g.shape[0]} {g.shape[1]}\n")
        for row in g.tolist():
            f.write(" ".join(str(v) for v in row) + "\n")
runs = rcc_cool.runs()
print(f"{out}: {len(data)} bytes, latent runs (coarsest first) {[len(r) for r in runs]}")
