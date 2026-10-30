# tables.py: the two integer tables of Ragtime's Cool-chic range coder
# (docs/PICTURE_CODEC.md, "Latent coding"). Run it to write tables.txt,
# which the C++ decoder and encoder take as they are; never edit that file
# by hand. The values come from exact decimal arithmetic, so they don't
# depend on the machine's floating point.
#
#   K[s], s = 0..2560: round(2^32 / (256 * b * ln 2)), b = exp(-5 + s/256),
#         the Laplace scale's index as Cool-chic 5.0.1's ARM gives it.
#   P[f], f = 0..1023: round(2^31 * 2^(-f/1024)).
import os
from decimal import Decimal, getcontext, ROUND_HALF_EVEN

getcontext().prec = 60
N_SCALE = 2561
N_FRAC = 1024


def _round(x):
    return int(x.to_integral_value(rounding=ROUND_HALF_EVEN))


def make():
    ln2 = Decimal(2).ln()
    k = [_round(Decimal(2) ** 32 / (256 * (Decimal(-5) + Decimal(s) / 256).exp() * ln2)) for s in range(N_SCALE)]
    p = [_round(Decimal(2) ** 31 * (Decimal(2) ** (Decimal(-f) / N_FRAC))) for f in range(N_FRAC)]
    return k, p


PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tables.txt")


def load():
    k, p = [], []
    for line in open(PATH):
        if line.startswith("#") or not line.strip():
            continue
        name, i, v = line.split()
        (k if name == "K" else p).append(int(v))
    assert len(k) == N_SCALE and len(p) == N_FRAC
    return k, p


if __name__ == "__main__":
    k, p = make()
    with open(PATH, "w") as f:
        f.write("# Ragtime Cool-chic range coder tables, written by tables.py. Do not edit.\n")
        f.write("# K s value: round(2^32 / (256 * exp(-5 + s/256) * ln 2)), s = 0..2560\n")
        f.write("# P f value: round(2^31 * 2^(-f/1024)), f = 0..1023\n")
        for s, v in enumerate(k):
            f.write(f"K {s} {v}\n")
        for i, v in enumerate(p):
            f.write(f"P {i} {v}\n")
    print(f"wrote {PATH}: K {k[0]}..{k[-1]}, P {p[0]}..{p[-1]}")
