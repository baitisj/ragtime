# test_rcc.py: round trips through rcc.py's coder, with random models, cut
# runs and edge cases. Run with python3 test_rcc.py.
import math
import random

import rcc


def check_model():
    # Frequencies all positive and summing to TOTAL, for a spread of models.
    for mu in (0, 1, 16383, 16384, 16511, 32767):
        for s in (0, 1, 640, 1280, 2000, 2560):
            c = [rcc.cum(i, mu, s) for i in range(rcc.N_SYMBOLS + 1)]
            assert c[0] == 0 and c[-1] == rcc.TOTAL
            assert all(b > a for a, b in zip(c, c[1:])), (mu, s)
    # Close to the true quantised Laplace where the leak doesn't dominate.
    mu, s = 16384 + 77, 1280 + 200
    b = math.exp(-5 + s / 256)
    m = -64 + mu / 256
    for x in (-3, -1, 0, 1, 2):
        f = lambda t: 0.5 * math.exp((t - m) / b) if t < m else 1 - 0.5 * math.exp(-(t - m) / b)
        want = f(x + 0.5) - f(x - 0.5)
        i = x + 64
        got = (rcc.cum(i + 1, mu, s) - rcc.cum(i, mu, s)) / rcc.TOTAL
        assert abs(got - want) < 1e-3, (x, got, want)


def round_trip(n, seed):
    rng = random.Random(seed)
    syms = []
    for _ in range(n):
        mu = rng.randrange(-200, 33000)          # out of range: clamped
        s = rng.randrange(-10, 2600)
        m, sc = rcc.model(mu, s)
        centre = -64 + m / 256
        width = math.exp(-5 + sc / 256)
        v = int(round(centre + rng.gauss(0, 1) * width * 1.4))
        v = rcc.clamp(v, -64, 63) if rng.random() > 0.01 else rng.randrange(-64, 64)
        syms.append((v, mu, s))
    enc = rcc.Encoder()
    for v, mu, s in syms:
        enc.encode(v, mu, s)
    run = enc.finish()
    dec = rcc.Decoder(run)
    for v, mu, s in syms:
        assert dec.decode(mu, s) == v
    # Cut runs: every symbol decoded while reliable() is right.
    for have in sorted({0, 1, 2, len(run) // 3, len(run) // 2, len(run) - 1}):
        if have < 0 or have > len(run):
            continue
        dec = rcc.Decoder(run, have)
        ok = 0
        for v, mu, s in syms:
            got = dec.decode(mu, s)
            if not dec.reliable():
                break
            assert got == v, (have, ok)
            ok += 1
    return len(run)


check_model()
for seed in range(40):
    round_trip(random.Random(seed).choice([0, 1, 2, 5, 50, 500, 3000]), seed)
# All zeros with a sharp model: a run of a byte or two, or nothing.
enc = rcc.Encoder()
for _ in range(1000):
    enc.encode(0, 16384, 0)
print("1000 sure zeros:", len(enc.finish()), "bytes")
print("varint", rcc.get_varint(rcc.put_varint(300), 0), rcc.unpack_grids(rcc.pack_grids([b"ab", b"", b"c" * 200]), 3))
print("ok")
