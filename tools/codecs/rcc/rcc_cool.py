# rcc_cool.py: plugs rcc.py's latent coding into Cool-chic 5.0.1 in place of
# constriction, from outside (import this before writing or reading).
# Everything else in the bitstream stays Cool-chic's: the video, frame and
# Cool-chic headers, the exp-Golomb weights, the grids coarsest first. The
# latent bytes (n_bytes_latent in the Cool-chic header) become the grid
# table and one flushed run per grid (docs/PICTURE_CODEC.md).
#
# Run from a Cool-chic checkout with PYTHONPATH=.:<this folder>.
from typing import List

import torch

import coolchic.bitstream.component.coolchic as cc

import rcc

state = {
    "runs": [],        # encoding: each grid's run, coarsest first
    "lengths": [],     # decoding: each run's length
    "data": b"",       # decoding: the runs
    "start": 0,        # decoding: where the next run starts
    "have": -1,        # decoding: bytes of the latent part that have arrived (-1: all)
    "offset": 0,       # decoding: where the runs start in the latent part
    "reliable": [],    # decoding: per grid, how many values were decoded from arrived bytes
}


class GridCoder:
    """Stands in for Cool-chic's RangeCoder, one per grid."""

    def __init__(self, mode, run=b"", have=-1):
        self.enc = rcc.Encoder() if mode == "encode" else None
        self.dec = rcc.Decoder(run, have) if mode == "decode" else None
        self.good = 0          # values decoded while reliable
        self.lost = False

    def encode(self, x: torch.Tensor, idx_mu_scale: torch.Tensor):
        for v, (m, s) in zip(x.tolist(), idx_mu_scale.tolist()):
            self.enc.encode(int(v), int(m), int(s))

    def decode(self, idx_mu_scale: torch.Tensor) -> torch.Tensor:
        out = []
        for m, s in idx_mu_scale.tolist():
            v = self.dec.decode(int(m), int(s))
            if not self.lost and self.dec.reliable():
                self.good += 1
            else:
                self.lost = True
                v = 0          # not arrived: taken as zero
            out.append(v)
        return torch.tensor(out, dtype=torch.int64)


class LatentPart:
    """Stands in for the RangeCoder that encode_decode_coolchic makes once for
    all the grids: it only gathers the runs (encode) or splits them (decode)."""

    def __init__(self):
        state["runs"] = []

    def get_bitstream_bytes(self) -> bytes:
        return rcc.pack_grids(state["runs"])

    def load_bitstream(self, raw: bytes):
        # The grid count is known only once the header is read; parse lazily.
        state["data"] = raw
        state["lengths"] = None
        state["reliable"] = []


cc.RangeCoder = LatentPart
_entropy = cc.entropy_coding_latent_arm


def _n_grids_from_caller():
    # encode_decode_coolchic's param, found on the stack (it holds n_latent_grids).
    import inspect

    for f in inspect.stack():
        p = f.frame.f_locals.get("param")
        if p is not None and hasattr(p, "n_latent_grids"):
            return p.n_latent_grids
    raise RuntimeError("no Cool-chic param on the stack")


def per_grid(encoder_data, ctx, dim, w, b, sw, sb, range_coder, mode, n_spatial_context):
    if mode == "encode":
        coder = GridCoder("encode")
        out = _entropy(encoder_data, ctx, dim, w, b, sw, sb, coder, mode, n_spatial_context)
        state["runs"].append(coder.enc.finish())
        return out
    if state["lengths"] is None:
        n = _n_grids_from_caller()
        data = state["data"]
        have = state["have"] if state["have"] >= 0 else len(data)
        try:
            lengths, pos = rcc.unpack_grids(data[:have], n)
        except ValueError:
            lengths, pos = [0] * n, len(data) + 1   # the table itself hasn't arrived
        state["lengths"], state["start"] = lengths, pos
    k = len(state["reliable"])
    start = state["start"]
    length = state["lengths"][k]
    have = state["have"] if state["have"] >= 0 else len(state["data"])
    arrived = max(0, min(length, have - start))
    run = state["data"][start:start + length] if start <= len(state["data"]) else b""
    coder = GridCoder("decode", run, arrived if arrived < length else -1)
    if start > have:
        coder.lost = True
    out = _entropy(encoder_data, ctx, dim, w, b, sw, sb, coder, mode, n_spatial_context)
    state["start"] = start + length
    state["reliable"].append((tuple(dim), coder.good, dim[0] * dim[1]))
    return out


cc.entropy_coding_latent_arm = per_grid


def set_have(n_latent_bytes: int):
    """Decode as if only this many bytes of the latent part had arrived (-1: all)."""
    state["have"] = n_latent_bytes


def runs() -> List[bytes]:
    return list(state["runs"])
