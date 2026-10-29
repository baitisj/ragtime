# jxl_at.py PICTURE BYTES...: JPEG XL (libjxl via imagecodecs) at a byte budget, largest
# quality that fits; at tiny budgets also tries coding a smaller picture
# and scaling it back up, as cjxl's --resampling does.
import os, sys, time
import numpy as np, imagecodecs
from PIL import Image

src = np.asarray(Image.open(sys.argv[1]).convert("RGB"))
stem = os.path.splitext(os.path.basename(sys.argv[1]))[0]
os.makedirs("out", exist_ok=True)
H, W = src.shape[:2]

def code(img, d, effort=9):
    return imagecodecs.jpegxl_encode(img, effort=effort, distance=d)

best = None
for scale in (1, 2, 3, 4):
    img = src if scale == 1 else np.asarray(Image.fromarray(src).resize((W // scale, H // scale), Image.LANCZOS))
    for budget in map(int, sys.argv[2:]):
        lo, hi, fit = 0.5, 25.0, None
        if len(code(img, hi)) > budget:
            print(f"scale 1/{scale} budget {budget}: even distance 25 is {len(code(img, hi))} bytes")
            continue
        for _ in range(18):
            mid = (lo + hi) / 2
            b = code(img, mid)
            if len(b) <= budget:
                hi, fit = mid, (mid, b)
            else:
                lo = mid
        d, b = fit
        t0 = time.time(); dec = imagecodecs.jpegxl_decode(b); t1 = time.time()
        out = dec if scale == 1 else np.asarray(Image.fromarray(dec).resize((W, H), Image.LANCZOS))
        name = f"out/{stem}_jxl_{budget}_s{scale}"
        open(name + ".jxl", "wb").write(b)
        Image.fromarray(out).save(name + ".png")
        print(f"scale 1/{scale} budget {budget}: distance {d:.2f}, {len(b)} bytes, decode {1000*(t1-t0):.0f} ms -> {name}.png")
