# metrics.py ORIGINAL DECODED...: PSNR and SSIM of each decoded picture against the original.
import sys, numpy as np
from PIL import Image
from skimage.metrics import peak_signal_noise_ratio as psnr, structural_similarity as ssim
ref = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.float64)
for p in sys.argv[2:]:
    im = np.asarray(Image.open(p).convert("RGB")).astype(np.float64)
    print(f"{p}: PSNR {psnr(ref, im, data_range=255):.2f} dB, SSIM {ssim(ref, im, channel_axis=2, data_range=255):.3f}")
