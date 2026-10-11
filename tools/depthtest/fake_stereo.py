"""
fake_stereo.py -- a left/right pair from one frame and a depth model, the way
an optional "fake 3D" mode would draw it: each eye shifts pixels sideways by
half the disparity its inverse depth implies, far pixels drawn first so near
ones win, and holes filled from the neighbouring pixel.

    python tools/depthtest/fake_stereo.py FRAMES_DIR OUT_DIR

Writes red/cyan anaglyphs (NAME-fake.png) and, for frames that came with
real stereo, the real pair's anaglyph beside it (NAME-real.png).
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from eval_depth import DA3Onnx, load_frames   # noqa: E402

MAX_SHIFT = 8          # pixels of disparity for the nearest point, at 320 wide


def eye(img, inv, sign):
    h, w = inv.shape
    lo, hi = np.percentile(inv, 2), np.percentile(inv, 98)
    n = np.clip((inv - lo) / max(hi - lo, 1e-6), 0, 1)
    shift = np.round(sign * n * MAX_SHIFT * w / 320 / 2).astype(int)
    out = np.zeros_like(img)
    filled = np.zeros((h, w), bool)
    order = np.argsort(n, axis=None)                     # far first, near overwrite
    ys, xs = np.unravel_index(order, n.shape)
    xt = np.clip(xs + shift[ys, xs], 0, w - 1)
    out[ys, xt] = img[ys, xs]
    filled[ys, xt] = True
    for x in range(1, w):                                # holes: copy from the left
        m = ~filled[:, x]
        out[m, x] = out[m, x - 1]
        filled[m, x] = filled[m, x - 1]
    return out


def anaglyph(left, right):
    return np.dstack([left[:, :, 0], right[:, :, 1], right[:, :, 2]])


def main(folder, out):
    os.makedirs(out, exist_ok=True)
    model = DA3Onnx(252)
    for name, left, right in load_frames(folder):
        import cv2
        inv = cv2.resize(model(left).astype(np.float32), (left.shape[1], left.shape[0]))
        l, r = eye(left, inv, +1), eye(left, inv, -1)
        Image.fromarray(anaglyph(l, r)).save(os.path.join(out, f"{name}-fake.png"))
        if right is not None:
            Image.fromarray(anaglyph(left, right)).save(os.path.join(out, f"{name}-real.png"))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
