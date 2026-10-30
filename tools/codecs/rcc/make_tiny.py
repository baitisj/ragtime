# make_tiny.py OUT.png: draws the 160x120 test picture behind
# tests/data/coolchic/ (a sky, a sun, hills, a red fan and a callsign
# plate), so the test vectors carry no one else's picture.
import math
import random
import sys

from PIL import Image, ImageDraw

W, H = 160, 120
im = Image.new("RGB", (W, H))
px = im.load()
for y in range(H):
    for x in range(W):
        t = y / H
        px[x, y] = (int(30 + 60 * t), int(40 + 90 * t), int(120 + 100 * (1 - t)))
d = ImageDraw.Draw(im)
d.ellipse((100, 12, 140, 52), fill=(250, 210, 90))
d.polygon([(0, 120), (40, 70), (75, 105), (110, 60), (160, 110), (160, 120)], fill=(40, 90, 50))
for i in range(9):
    a = i * math.pi / 8
    d.line((40, 60, 40 + 30 * math.cos(a), 60 - 30 * math.sin(a)), fill=(230, 60, 60), width=2)
d.rectangle((12, 88, 70, 112), fill=(20, 20, 30))
d.text((16, 94), "AG7EW 73", fill=(255, 255, 255))
random.seed(7)
for _ in range(60):
    x, y = random.randrange(W), random.randrange(H // 2)
    px[x, y] = (255, 255, 255)
im.save(sys.argv[1])
