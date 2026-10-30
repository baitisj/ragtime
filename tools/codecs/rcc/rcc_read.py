# rcc_read.py IN.rcc OUT.png [HAVE]: decodes a Ragtime Cool-chic bitstream,
# or, with HAVE, as much of it as the first HAVE bytes allow: the grids that
# have arrived, the grid being received as far as its bytes go, the rest
# taken as zero (docs/PICTURE_CODEC.md, "Partial streams"). Prints what was
# decoded. Run from a Cool-chic checkout with PYTHONPATH=.:<this folder>.
import os
import sys
import tempfile

import rcc_cool
from coolchic.bitstream.decode import decode_video
from coolchic.bitstream.header.header import CoolChicHeader, FrameHeader, VideoHeader

path, out = sys.argv[1:3]
data = open(path, "rb").read()
have = int(sys.argv[3]) if len(sys.argv) > 3 else len(data)

# The latent part is last; its size is in the Cool-chic header.
rest = VideoHeader().read_header(data)
rest = FrameHeader().read_header(rest)
h = CoolChicHeader()
rest = h.read_header(rest)
n_latent = h.get_value("n_bytes_latent")
latent_at = len(data) - n_latent
if have < latent_at:
    sys.exit(f"only {have} of the {latent_at} bytes of headers and weights: nothing to show yet")
rcc_cool.set_have(have - latent_at)

# Cool-chic reads from a file: give it the stream with what hasn't arrived as zeros.
with tempfile.NamedTemporaryFile(suffix=".rcc", delete=False) as t:
    t.write(data[:have] + bytes(len(data) - have))
try:
    decode_video(t.name, decoded_path=out)
finally:
    os.unlink(t.name)
for (hh, ww), good, n in rcc_cool.state["reliable"]:
    print(f"grid {hh}x{ww}: {good} of {n} values")
print(f"{have} of {len(data)} bytes -> {out}")
