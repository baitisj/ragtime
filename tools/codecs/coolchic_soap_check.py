# coolchic_soap_check.py DIR: test vectors for the C++ port of Cool-chic's
# optimiser (image/src/coolchic/FitOptim.cpp). Runs Cool-chic 5.0.1's SOAP
# with the two parameter groups train.py makes (the networks' weights:
# SOAP with weight decay; the latents: Adam) on a few tensors, through
# random gradients, and writes DIR/shapes.txt ("latent|model d0 d1 .."),
# init.bin, grads.bin (all steps) and final.bin, float32. Every
# preconditioned dimension's Gram matrix has full rank from the first
# gradient on, so the eigenbases are the same up to sign, and the results
# with them. Run from a Cool-chic checkout with PYTHONPATH=.
import sys

import numpy as np
import torch

from coolchic.training.soap import SOAP

STEPS = 25
FREQ = 3
SHAPES = [
    ("model", [3, 3]),
    ("model", [4, 4]),
    ("model", [3, 3, 1, 1]),
    ("model", [2, 2, 3, 3]),
    ("model", [5]),
    ("latent", [1, 1, 4, 6]),
    ("latent", [1, 1, 2, 3]),
]

out = sys.argv[1]
torch.manual_seed(5)
params = [torch.nn.Parameter(torch.randn(s) * 0.3) for _, s in SHAPES]
init = torch.cat([p.detach().flatten() for p in params])
model = [p for (k, _), p in zip(SHAPES, params) if k == "model"]
latent = [p for (k, _), p in zip(SHAPES, params) if k == "latent"]
opt = SOAP(
    [
        {"params": model, "lr": 0.01, "betas": (0.95, 0.95), "precondition_frequency": FREQ,
         "max_precond_dim": 256, "merge_dims": False, "precondition_1d": False,
         "weight_decay": 0.01},
        {"params": latent, "lr": 0.01, "betas": (0.9, 0.999), "precondition_frequency": 1,
         "max_precond_dim": 0, "merge_dims": False, "precondition_1d": False,
         "weight_decay": 0.0},
    ]
)
grads = []
for step in range(STEPS):
    for p in params:
        p.grad = torch.randn(p.shape)
    grads.append(torch.cat([p.grad.flatten() for p in params]))
    opt.step()
final = torch.cat([p.detach().flatten() for p in params])

with open(f"{out}/shapes.txt", "w") as f:
    for k, s in SHAPES:
        f.write(k + " " + " ".join(str(d) for d in s) + "\n")
init.numpy().astype(np.float32).tofile(f"{out}/init.bin")
torch.stack(grads).numpy().astype(np.float32).tofile(f"{out}/grads.bin")
final.numpy().astype(np.float32).tofile(f"{out}/final.bin")
print(f"{STEPS} steps, {init.numel()} parameters")
