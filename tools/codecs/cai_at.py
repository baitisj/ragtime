# cai_at.py PICTURE BYTES...: CompressAI zoo models at byte budgets, real entropy coding.
# At each scale (the picture shrunk, coded, decoded and scaled back up)
# the highest quality that fits; the best by SSIM is kept per model.
import os, sys, time, warnings
warnings.filterwarnings("ignore")
import numpy as np, torch, torch.nn.functional as F
from PIL import Image
from compressai.zoo import image_models
from skimage.metrics import structural_similarity as ssim, peak_signal_noise_ratio as psnr

torch.set_num_threads(4)
src = Image.open(sys.argv[1]).convert("RGB")
stem = os.path.splitext(os.path.basename(sys.argv[1]))[0]
os.makedirs("out", exist_ok=True)
W, H = src.size
ref = np.asarray(src).astype(np.float64)
budgets = [int(b) for b in sys.argv[2:]]
HEADER = 6  # model id, quality, the coded picture's size: a few bytes beside the strings

def run(net, img):
    x = torch.from_numpy(np.asarray(img)).permute(2, 0, 1).float().unsqueeze(0) / 255
    h, w = x.shape[2:]
    ph, pw = (64 - h % 64) % 64, (64 - w % 64) % 64
    xp = F.pad(x, (0, pw, 0, ph), mode="replicate")
    with torch.no_grad():
        t0 = time.time(); enc = net.compress(xp); t1 = time.time()
        dec = net.decompress(enc["strings"], enc["shape"]); t2 = time.time()
    n = sum(len(s[0]) for s in enc["strings"]) + HEADER
    y = dec["x_hat"][:, :, :h, :w].clamp(0, 1)[0].permute(1, 2, 0).numpy()
    return n, Image.fromarray((y * 255 + 0.5).astype(np.uint8)), t1 - t0, t2 - t1

for name in ["bmshj2018-hyperprior", "mbt2018-mean", "mbt2018", "cheng2020-attn"]:
    qs = range(1, 9) if name.startswith("bmshj") or name.startswith("mbt") else range(1, 7)
    best = {b: None for b in budgets}
    for scale in (1, 1.5, 2, 3):
        img = src if scale == 1 else src.resize((round(W / scale), round(H / scale)), Image.LANCZOS)
        for q in qs:
            net = image_models[name](quality=q, pretrained=True).eval()
            net.update()
            n, out, te, td = run(net, img)
            if scale != 1: out = out.resize((W, H), Image.LANCZOS)
            a = np.asarray(out).astype(np.float64)
            s = ssim(ref, a, channel_axis=2, data_range=255); p = psnr(ref, a, data_range=255)
            print(f"{name} scale {scale} q{q}: {n} B, SSIM {s:.3f}, PSNR {p:.2f}, enc {te:.1f}s dec {td:.1f}s", flush=True)
            for b in budgets:
                if n <= b and (best[b] is None or s > best[b][0]):
                    best[b] = (s, p, n, scale, q, te, td, out)
            if n > max(budgets): break
    for b, v in best.items():
        if v:
            s, p, n, scale, q, te, td, out = v
            out.save(f"out/{stem}_cai_{name}_{b}.png")
            print(f"BEST {name} {b}: scale {scale} q{q} {n} B SSIM {s:.3f} PSNR {p:.2f} enc {te:.1f}s dec {td:.1f}s")
