#!/usr/bin/env python3
"""Draws Ragtime's icon, app/res/ragtime.svg: the leaf from the cover of
Scott Joplin's Maple Leaf Rag (1899), tipped as it is there, going
digital, block by block, on the cover's cream with its red rim.

The left of the leaf stays smooth. Right of SPLIT the leaf is sampled into
square blocks that grow to the right, fade from the leaf's green to mint,
and are dropped or drift off ever more often, with a trail of loose
blocks beyond. The randomness is seeded, so the drawing is the same each
run. Then run make_icons.sh for the PNG and ICO.

    tools/icon/make_leaf_svg.py         # needs rsvg-convert, numpy, Pillow
"""

import io
import os
import random
import subprocess

import numpy as np
from PIL import Image

CREAM, RED, GREEN, MINT = '#f1e4c8', '#a8322d', '#2e5e52', '#6fae8f'
SEED = 12
SPLIT, END = 136, 218       # the blocks start at SPLIT and stop growing at END
PLACE = 'translate(104 126) rotate(20) scale(0.92)'

# A sugar maple leaf, tip up and stem down, in a frame of about +-100: the
# right half, mirrored for the left.
RIGHT = [(0, -108), (8, -94), (17, -99), (17, -84), (29, -87), (25, -70), (38, -70), (30, -54),
         (22, -34), (26, -28),
         (36, -40), (42, -54), (52, -46), (62, -62), (68, -48), (84, -52), (80, -40), (104, -36),
         (88, -26), (96, -16), (80, -14), (80, -4), (62, -2),
         (46, 8),
         (60, 12), (74, 12), (70, 22), (82, 34), (62, 34), (56, 44), (42, 38), (28, 46), (12, 48), (4, 51)]
LEAF = 'M' + ' L'.join('%g %g' % p for p in RIGHT + [(-x, y) for x, y in reversed(RIGHT) if x]) + ' Z'
STEM = 'M1 48 C0 72 -4 94 -12 114'

# Loose blocks beyond the leaf: x, y, size, how far from green to mint.
TRAIL = [(196, 70, 10, .8), (204, 96, 12, .85), (200, 128, 10, .85), (214, 58, 11, .9),
         (218, 84, 14, .95), (220, 112, 12, .95), (226, 40, 9, 1), (228, 140, 9, 1)]


def leaf_mask():
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 256 256">'
           f'<rect width="256" height="256"/><path fill="#fff" transform="{PLACE}" d="{LEAF}"/></svg>')
    png = subprocess.run(['rsvg-convert'], input=svg.encode(), capture_output=True, check=True).stdout
    return np.asarray(Image.open(io.BytesIO(png)).convert('L'), dtype=float) / 255.0


def mix(t):
    a = [int(GREEN[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(MINT[i:i + 2], 16) for i in (1, 3, 5)]
    return '#%02x%02x%02x' % tuple(round(a[k] + (b[k] - a[k]) * t) for k in range(3))


def main():
    random.seed(SEED)
    mask = leaf_mask()

    def cover(x, y, s):
        a = mask[max(0, int(y * 4)):int((y + s) * 4), int(x * 4):int((x + s) * 4)]
        return a.mean() if a.size else 0

    blocks = []
    x = SPLIT
    while x < END:
        s = 6 + (x - SPLIT) * 0.12
        t = (x - SPLIT) / (END - SPLIT)
        for r in range(-int(130 / s) - 1, int(130 / s) + 1):
            y = 122 + (r - 0.5) * s
            c = cover(x, y, s)
            if c > 0.45:
                if random.random() < 0.45 * t * t:
                    continue
                dx = s * random.uniform(1.0, 2.4) if random.random() < 0.35 * t * t else 0
                blocks.append((x + dx, y, s, mix(t)))
            elif c > 0.05 and random.random() < 0.3 * t:
                blocks.append((x + s * random.uniform(0.5, 1.5), y, s, mix(t)))
        x += s
    blocks += [(bx, by, bs, mix(t)) for bx, by, bs, t in TRAIL]

    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">',
           '  <!-- Ragtime: the Maple Leaf Rag\'s leaf going digital. Drawn by',
           '       tools/icon/make_leaf_svg.py; edit that, not this. -->',
           f'  <defs><clipPath id="smooth"><rect width="{SPLIT + 1}" height="256"/></clipPath></defs>',
           f'  <rect x="8" y="8" width="240" height="240" rx="44" fill="{CREAM}"/>',
           '  <g clip-path="url(#smooth)">',
           f'    <path fill="{GREEN}" transform="{PLACE}" d="{LEAF}"/>',
           f'    <path fill="none" stroke="{GREEN}" stroke-width="5.5" stroke-linecap="round" '
           f'transform="{PLACE}" d="{STEM}"/>',
           '  </g>']
    for bx, by, s, colour in blocks:
        gap = max(1.3, s * 0.16)
        if bx + s - gap > 236 or by < 16 or by + s > 240:
            continue
        out.append(f'  <rect x="{bx + gap / 2:.1f}" y="{by + gap / 2:.1f}" width="{s - gap:.1f}" '
                   f'height="{s - gap:.1f}" fill="{colour}"/>')
    out += [f'  <rect x="8" y="8" width="240" height="240" rx="44" fill="none" stroke="{RED}" stroke-width="8"/>',
            '</svg>']

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'app', 'res', 'ragtime.svg')
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
