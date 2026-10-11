"""
fake3d.py -- the PC prototype of an optional "fake 3D" mode: a stereo pair
from a mono game frame and FastDepth, the one depth model fast enough on the
Quest 3 (7.6 ms on its GPU, docs: tools/depthtest).

FastDepth's raw depth maps are blotchy and change from frame to frame, so the
pipeline cleans them up the way the headset could afford to every frame:

  1. FastDepth 256 int8 on the frame -> inverse depth
  2. normalise to 0..1 between percentiles whose range follows the scene
     slowly, so the depth scale does not jump between frames
  3. time smoothing: an exponential average with the previous depth maps
  4. edge-guided filter (He's guided filter, the frame's luma as the guide),
     so depth edges follow the picture's edges
  5. each eye samples the frame shifted by half the disparity at its pixel:
     one texture read per pixel, as a fragment shader would; mid depth sits
     at the screen, nearer comes out, farther goes in

Runs on sequences of frames from titles with real stereo (Descent 2 in
i-glasses mode: even lines left, odd right), feeds only the left eye to the
model, and scores the depth at each step against the real pair's disparity
(Spearman, as eval_depth.py) and for flicker (mean change of the 0..1 depth
between frames).

    FASTDEPTH_DIR=... python tools/depthtest/fake3d.py SEQ_DIR OUT_DIR

SEQ_DIR holds fNN.png, interlaced. Writes OUT_DIR/scores.txt, sheet.png
(frame, real disparity, raw / smoothed / filtered depth, fake and real
anaglyphs for a few frames), fake.gif and real.gif (red/cyan anaglyph
animations) and sbs.mp4 (fake left | right, for a headset video player).
"""
import glob
import os
import sys
import time

import cv2
import numpy as np
from PIL import Image
from scipy.stats import spearmanr

sys.path.insert(0, os.path.dirname(__file__))
from eval_depth import FastDepth, true_disparity   # noqa: E402

DISPARITY = 4.0        # pixels between the eyes for the nearest point, at 320 wide
TIME_KEEP = 0.6        # share of the previous depth map kept each frame
RANGE_KEEP = 0.9       # share of the previous normalisation range kept
GUIDE_RADIUS = 4       # guided filter window, pixels at 320 wide
GUIDE_EPS = 2e-3       # guided filter smoothing, in (0..1 luma)^2


def guided_filter(guide, src, r, eps):
    """He, Sun and Tang's guided filter with box means: src smoothed, its
    edges taken from guide (both float32 0..1)."""
    k = (2 * r + 1, 2 * r + 1)
    mean = lambda x: cv2.boxFilter(x, -1, k, borderType=cv2.BORDER_REFLECT)
    mi, ms = mean(guide), mean(src)
    cov = mean(guide * src) - mi * ms
    var = mean(guide * guide) - mi * mi
    a = cov / (var + eps)
    b = ms - a * mi
    return mean(a) * guide + mean(b)


class Fake3D:
    """The per-frame pipeline with its state between frames."""

    def __init__(self, model, smooth=True, guide=True):
        self.model, self.smooth, self.guide = model, smooth, guide
        self.lo = self.hi = None
        self.prev = None

    def depth(self, img):
        h, w = img.shape[:2]
        inv = cv2.resize(self.model(img).astype(np.float32), (w, h), interpolation=cv2.INTER_LINEAR)
        lo, hi = np.percentile(inv, 2), np.percentile(inv, 98)
        if self.smooth and self.lo is not None:
            lo = RANGE_KEEP * self.lo + (1 - RANGE_KEEP) * lo
            hi = RANGE_KEEP * self.hi + (1 - RANGE_KEEP) * hi
        self.lo, self.hi = lo, hi
        n = np.clip((inv - lo) / max(hi - lo, 1e-6), 0, 1).astype(np.float32)
        if self.smooth and self.prev is not None:
            n = TIME_KEEP * self.prev + (1 - TIME_KEEP) * n
        self.prev = n
        if self.guide:
            luma = cv2.cvtColor(img, cv2.COLOR_RGB2GRAY).astype(np.float32) / 255
            n = np.clip(guided_filter(luma, n, max(1, round(GUIDE_RADIUS * w / 320)), GUIDE_EPS), 0, 1)
        return n

    @staticmethod
    def eyes(img, n):
        """Left and right eye: each samples the frame half a disparity
        sideways; near (n = 1) comes out of the screen, far goes in."""
        h, w = n.shape
        d = (n - 0.5) * DISPARITY * w / 320          # pixels, + is nearer
        xs, ys = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
        left = cv2.remap(img, xs + d / 2, ys, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        right = cv2.remap(img, xs - d / 2, ys, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        return left, right


def anaglyph(left, right):
    return np.dstack([left[:, :, 0], right[:, :, 1], right[:, :, 2]])


def colour(d):
    return cv2.applyColorMap((np.clip(d, 0, 1) * 255).astype(np.uint8), cv2.COLORMAP_TURBO)[:, :, ::-1]


def main(seq, out):
    os.makedirs(out, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(seq, "f*.png")))
    frames = [np.asarray(Image.open(p).convert("RGB")) for p in paths]
    lefts, rights = [f[0::2] for f in frames], [f[1::2] for f in frames]
    model = FastDepth(256)
    variants = {"raw": Fake3D(model, False, False), "smoothed": Fake3D(model, True, False),
                "smoothed+edges": Fake3D(model, True, True)}
    depths = {k: [] for k in variants}
    post_ms = []
    for left in lefts:
        for k, v in variants.items():
            t0 = time.perf_counter()
            depths[k].append(v.depth(left))
            if k == "smoothed+edges":
                post_ms.append((time.perf_counter() - t0) * 1000)
    truth = [true_disparity(l, r) for l, r in zip(lefts, rights)]

    lines = [f"{len(frames)} frames from {seq}; left eye to FastDepth 256 int8; disparity {DISPARITY} px at 320",
             "variant           rho mean   rho min   flicker"]
    for k in variants:
        rho = [spearmanr(d[v], td[v]).correlation for d, (td, v) in zip(depths[k], truth) if v.sum() > 200]
        flick = np.mean([np.abs(a - b).mean() for a, b in zip(depths[k][1:], depths[k][:-1])])
        lines.append(f"{k:16s}  {np.mean(rho):+.3f}    {np.min(rho):+.3f}    {flick:.4f}")
    real_flick = []
    for (a, va), (b, vb) in zip(truth[1:], truth[:-1]):
        lo, hi = np.percentile(a[va], 2), np.percentile(a[va], 98)
        m = va & vb
        real_flick.append(np.abs((a[m] - b[m]) / max(hi - lo, 1e-6)).mean())
    lines.append(f"real disparity flicker (same units, trusted pixels): {np.mean(real_flick):.4f}")
    lines.append(f"PC time per frame, model + steps 2-4: {np.median(post_ms):.1f} ms")
    open(os.path.join(out, "scores.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))

    best = variants["smoothed+edges"]
    fake_gif, real_gif, sheet_rows = [], [], []
    h, w = lefts[0].shape[:2]
    vw = cv2.VideoWriter(os.path.join(out, "sbs.mp4"), cv2.VideoWriter_fourcc(*"mp4v"), 30, (w * 4, h * 2))
    for i, (left, right) in enumerate(zip(lefts, rights)):
        fl, fr = best.eyes(left, depths["smoothed+edges"][i])
        fake_gif.append(Image.fromarray(cv2.resize(anaglyph(fl, fr), (w * 2, h * 2), interpolation=cv2.INTER_NEAREST)))
        real_gif.append(Image.fromarray(cv2.resize(anaglyph(left, right), (w * 2, h * 2), interpolation=cv2.INTER_NEAREST)))
        vw.write(cv2.resize(np.hstack([fl, fr]), (w * 4, h * 2), interpolation=cv2.INTER_NEAREST)[:, :, ::-1])
        if i % 15 == 0:
            td, v = truth[i]
            lo, hi = np.percentile(td[v], 2), np.percentile(td[v], 98)
            tc = colour((td - lo) / max(hi - lo, 1e-6))
            tc[~v] = 0
            sheet_rows.append(np.hstack([left, tc, colour(depths["raw"][i]), colour(depths["smoothed"][i]),
                                         colour(depths["smoothed+edges"][i]), anaglyph(fl, fr), anaglyph(left, right)]))
    vw.release()
    fake_gif[0].save(os.path.join(out, "fake.gif"), save_all=True, append_images=fake_gif[1:], duration=33, loop=0)
    real_gif[0].save(os.path.join(out, "real.gif"), save_all=True, append_images=real_gif[1:], duration=33, loop=0)
    Image.fromarray(np.vstack(sheet_rows)).save(os.path.join(out, "sheet.png"))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
