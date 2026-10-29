# Checks Ragtime's C++ Cool-chic encoder (image/src/coolchic/Fit*) against
# Cool-chic 5.0.1's own PyTorch code: the same parameters and target give
# the same loss and the same gradient, tensor by tensor (the relative L2
# error of each tensor's gradient). Where a latent's probability sits right
# at the 2^-16 floor, float rounding decides whether it gets a gradient, so
# the ARM's and the latents' gradients agree to about 1e-3, the rest to 1e-6.
#
#   ragtime_coolchic_fit grad lop 37 45 5 DIR [DROP]     (the C++ side)
#   PYTHONPATH=<Cool-Chic checkout> python coolchic_fit_check.py lop 37 45 DIR [DROP]
#
# Needs torch, einops and fvcore (Cool-chic's requirements).
import sys

import numpy as np
import torch

from coolchic.component.core.coolchic import CoolChicEncoder, CoolChicEncoderParameter
from coolchic.training.loss import loss_function

ARCH = {
    "lop": (["8-1-linear-relu", "3-1-linear-none", "3-3-residual-none"], 6, 2),
    "mop": (["16-1-linear-relu", "3-1-linear-none", "3-3-residual-relu", "3-3-residual-none"], 10, 4),
    "hop": (["48-1-linear-relu", "3-1-linear-none", "3-3-residual-relu", "3-3-residual-none"], 14, 6),
}


def main():
    arch, h, w, d = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    drop = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    layers, ctx, ifce = ARCH[arch]
    p = CoolChicEncoderParameter(
        layers_synthesis=layers, linear_stabiliser_synth=True, ups_k_size=8,
        ups_preconcat_k_size=7, ifce_resolution=(0, 2), output_feature_ifce=ifce,
        spatial_context_arm=ctx, linear_stabiliser_arm=True, n_hidden_layers_arm=2,
        latent_resolution=(0, 6), hyperlatent_resolution=(4, 6), flag_common_randomness=False,
        img_size=(h, w), final_upsampling_type="bicubic")
    enc = CoolChicEncoder(p)
    enc.train()

    flat = np.fromfile(f"{d}/params.bin", dtype=np.float32)
    names = [l.split() for l in open(f"{d}/names.txt")]
    named = dict(enc.named_parameters())
    for name, off, size in names:
        off, size = int(off), int(size)
        t = named[name]
        with torch.no_grad():
            t.copy_(torch.from_numpy(flat[off:off + size].copy()).view(t.shape))
    target = torch.from_numpy(np.fromfile(f"{d}/target.bin", dtype=np.float32)).view(1, 3, h, w)

    if drop:
        orig = CoolChicEncoder.discard_hyperlatent

        def dropping(self, lat):
            g = orig(self, lat)
            return [torch.zeros_like(x) if i < drop else x for i, x in enumerate(g)]
        CoolChicEncoder.discard_hyperlatent = dropping

    out = enc.forward(quantizer_noise_type="gaussian", quantizer_type="softround",
                      soft_round_temperature=torch.tensor(0.3), noise_parameter=torch.tensor(0.0))
    decoded = torch.clamp(out["raw_out"], 0.0, 1.0)
    loss = loss_function(decoded, {"residue": out["rate"]}, target, {"mse": 1.0}, lmbda=0.02,
                         compute_logs=True)
    loss.loss.backward()
    c_loss = [float(x) for x in open(f"{d}/loss.txt").read().split()]
    print(f"loss  torch {loss.loss.item():.7f}  C++ {c_loss[0]:.7f}")
    print(f"rate  torch {out['rate'].sum().item():.3f}  C++ {c_loss[2]:.3f}")

    grads = np.fromfile(f"{d}/grads.bin", dtype=np.float32)
    worst = 0.0
    for name, off, size in names:
        off, size = int(off), int(size)
        t = named[name]
        g = t.grad.detach().numpy().reshape(-1) if t.grad is not None else np.zeros(size)
        c = grads[off:off + size]
        err = np.linalg.norm(g - c) / max(np.linalg.norm(g), 1e-12)
        worst = max(worst, err if np.abs(g).max() > 1e-9 else 0.0)
        flag = "" if err < 1e-3 or np.abs(g).max() < 1e-9 else "   <-- differs"
        print(f"{name:60s} max|g| {np.abs(g).max():.3e}  rel err {err:.2e}{flag}")
    print(f"worst relative error {worst:.2e}")
    # The test vector for ragtime_coolchic_fit_test: PyTorch's loss and
    # gradient beside the C++ side's inputs.
    torch_grads = np.concatenate([
        (named[n].grad.detach().numpy().reshape(-1) if named[n].grad is not None
         else np.zeros(int(sz), dtype=np.float32)) for n, _, sz in names]).astype(np.float32)
    torch_grads.tofile(f"{d}/torch_grads.bin")
    with open(f"{d}/torch_loss.txt", "w") as f:
        f.write(f"{loss.loss.item():.9g} {out['rate'].sum().item():.9g}\n")


main()
