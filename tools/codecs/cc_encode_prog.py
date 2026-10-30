# cc_encode_prog.py [cc_encode.py's arguments]: Cool-chic's encoder, fitted
# for a progressive preview. On each training step, half the time the finest
# K synthesis grids (K from 1 to PROG_MAX, 3 by default) are taken as zero
# before the picture is made, so the network learns to give a fair picture
# from the coarse grids alone, as a receiver with a cut-off stream would
# (cc_prefix.py). The rate is always the whole bitstream's, and validation
# and the bitstream are the whole picture's. Run from a Cool-chic checkout
# with PYTHONPATH=.; it patches Cool-chic from outside.
import os
import random
import runpy
import sys

import torch

import coolchic.component.core.coolchic as core

PROG_MAX = int(os.environ.get("PROG_MAX", "3"))
PROG_SHARE = float(os.environ.get("PROG_SHARE", "0.5"))

discard = core.CoolChicEncoder.discard_hyperlatent


def truncating(self, decoder_side_latent):
    grids = discard(self, decoder_side_latent)
    # Only while fitting: validation and the bitstream run without gradients.
    if torch.is_grad_enabled() and random.random() < PROG_SHARE:
        k = random.randint(1, PROG_MAX)
        grids = [torch.zeros_like(x) if i < k else x for i, x in enumerate(grids)]
    return grids


core.CoolChicEncoder.discard_hyperlatent = truncating

sys.argv[0] = "cc_encode.py"
runpy.run_path("cc_encode.py", run_name="__main__")
