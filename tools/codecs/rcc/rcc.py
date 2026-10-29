# rcc.py: the reference for how Ragtime codes Cool-chic's latents
# (docs/PICTURE_CODEC.md, "Latent coding"): the fixed-point Laplace model,
# the range coder, and the grid table. Plain Python integers throughout, so
# it says exactly what the C++ must do. Slow, but it is the reference.
from typing import List, Tuple

from tables import load

K, P = load()

TOTAL_BITS = 24
TOTAL = 1 << TOTAL_BITS
AC_MAX_VAL = 64            # latents are in [-64, 63]
N_SYMBOLS = 2 * AC_MAX_VAL
N_MU = 2 * AC_MAX_VAL * 256  # mu index: mu = -64 + i/256, i = 0..32767
N_SCALE = len(K)           # scale index: log b = -5 + s/256, s = 0..2560
TOP = 1 << 56              # range stays in [2^56, 2^64)
MASK64 = (1 << 64) - 1


def clamp(v: int, lo: int, hi: int) -> int:
    return lo if v < lo else hi if v > hi else v


def half_tail(d: int, k: int) -> int:
    """exp(-|d| / (256 b)) / 2 for d in 1/256ths, in 1/2^24ths."""
    e = abs(d) * k                 # the exponent, base 2, in Q32
    ip = e >> 32
    if ip >= 24:
        return 0
    return P[(e >> 22) & 1023] >> (8 + ip)


def cdf_edge(d: int, k: int) -> int:
    """The Laplace CDF at d (1/256ths from mu), in 1/2^24ths."""
    t = half_tail(d, k)
    return t if d < 0 else TOTAL - t


def cum(i: int, mu: int, s: int) -> int:
    """Cumulative frequency below symbol index i (symbol i - 64), i = 0..128."""
    if i <= 0:
        return 0
    if i >= N_SYMBOLS:
        return TOTAL
    # The edge between symbols i - 65 and i - 64 is at i - 64.5.
    mu_q8 = mu - AC_MAX_VAL * 256
    d = (i - AC_MAX_VAL) * 256 - 128 - mu_q8
    return i + (((TOTAL - N_SYMBOLS) * cdf_edge(d, K[s])) >> TOTAL_BITS)


def model(mu_index: int, scale_index: int) -> Tuple[int, int]:
    return clamp(mu_index, 0, N_MU - 1), clamp(scale_index, 0, N_SCALE - 1)


class Encoder:
    """LZMA's range encoder widened to a 64-bit range: low is 64 bits and a
    carry, and a cache byte (with a count of 0xFF bytes after it) holds
    output that a carry could still change."""

    def __init__(self):
        self.low = 0
        self.range = MASK64
        self.cache = 0
        self.cache_size = 1
        self.out = bytearray()

    def _shift_low(self):
        if self.low < 0xFF << 56 or self.low > MASK64:
            carry = self.low >> 64
            temp = self.cache
            while True:
                self.out.append((temp + carry) & 0xFF)
                temp = 0xFF
                self.cache_size -= 1
                if self.cache_size == 0:
                    break
            self.cache = (self.low >> 56) & 0xFF
        self.cache_size += 1
        self.low = (self.low & ((1 << 56) - 1)) << 8

    def encode(self, value: int, mu_index: int, scale_index: int):
        mu, s = model(mu_index, scale_index)
        i = clamp(value, -AC_MAX_VAL, AC_MAX_VAL - 1) + AC_MAX_VAL
        lo, hi = cum(i, mu, s), cum(i + 1, mu, s)
        r = self.range >> TOTAL_BITS
        self.low += r * lo
        self.range = r * (hi - lo)
        while self.range < TOP:
            self.range = (self.range << 8) & MASK64
            self._shift_low()

    def finish(self) -> bytes:
        # The value in [low, low + range) with the most zero bits below it,
        # so that the zeros a decoder pads with land inside the interval.
        for bits in range(64, -1, -1):
            mask = (1 << bits) - 1
            v = (self.low + mask) & ~mask
            if v < self.low + self.range:
                break
        self.low = v
        for _ in range(9):
            self._shift_low()
        run = bytes(self.out[1:])      # the first byte is always 0
        return run.rstrip(b"\0")       # and a decoder pads with zeros


class Decoder:
    """Reads one grid's run. Bytes past the end (or past what has arrived)
    read as zero; reliable() says whether every symbol so far was decoded
    from bytes that had arrived."""

    def __init__(self, run: bytes, have: int = -1):
        self.run = run
        self.have = len(run) if have < 0 else min(have, len(run))
        self.pos = 0
        self.range = MASK64
        self.code = 0
        for _ in range(8):
            self.code = (self.code << 8) | self._byte()

    def _byte(self) -> int:
        b = self.run[self.pos] if self.pos < self.have else 0
        self.pos += 1
        return b

    def reliable(self) -> bool:
        # Bytes the encoder wrote are final, and the run has trailing zeros
        # stripped: a byte read past a complete run is a true zero.
        return self.pos <= self.have or self.have == len(self.run)

    def decode(self, mu_index: int, scale_index: int) -> int:
        mu, s = model(mu_index, scale_index)
        r = self.range >> TOTAL_BITS
        v = min(self.code // r, TOTAL - 1)
        lo, hi = 0, N_SYMBOLS          # find i: cum(i) <= v < cum(i + 1)
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if cum(mid, mu, s) <= v:
                lo = mid
            else:
                hi = mid
        c0, c1 = cum(lo, mu, s), cum(lo + 1, mu, s)
        self.code -= r * c0
        self.range = r * (c1 - c0)
        while self.range < TOP:
            self.range = (self.range << 8) & MASK64
            self.code = ((self.code << 8) | self._byte()) & MASK64
        return lo - AC_MAX_VAL


def put_varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def get_varint(data: bytes, pos: int) -> Tuple[int, int]:
    n, shift = 0, 0
    while True:
        if pos >= len(data) or shift > 28:
            raise ValueError("bad varint")
        b = data[pos]
        pos += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n, pos


def pack_grids(runs: List[bytes]) -> bytes:
    """The latent part: each run's length as a varint, coarsest first, then the runs."""
    return b"".join(put_varint(len(r)) for r in runs) + b"".join(runs)


def unpack_grids(data: bytes, n_grids: int) -> Tuple[List[int], int]:
    """Run lengths and where the first run starts."""
    lengths, pos = [], 0
    for _ in range(n_grids):
        n, pos = get_varint(data, pos)
        lengths.append(n)
    return lengths, pos
