"""
eval_depth.py -- how well monocular depth models see DOS game frames.

Scores each model against true stereo: frames from games that draw real
stereo (Descent 2 in i-glasses and VFX1 modes, MadSpace) give a left and a
right eye, and the disparity OpenCV finds between them is ground truth for
the relative depth of every textured pixel. The score is the Spearman rank
correlation between a model's inverse depth for the left eye and that
disparity, so scale and offset do not matter. A plain "bottom of the screen
is near" ramp is scored alongside as the free baseline.

    python tools/depthtest/eval_depth.py build/depthtest/frames build/depthtest/out

Frames: NAME-eyes.png (two eyes side by side), d2igl-*.png (interlaced,
even lines left) and anything else as mono, shown but not scored.
Timings are single-image CPU latency on this PC, only useful relative to
each other.
"""
import glob
import os
import sys
import time

import cv2
import numpy as np
import torch
from PIL import Image
from scipy.stats import spearmanr

torch.set_grad_enabled(False)
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)


# ------------------------------------------------------------------ frames

def load_frames(folder):
    frames = []
    for path in sorted(glob.glob(os.path.join(folder, "*.png"))):
        name = os.path.splitext(os.path.basename(path))[0]
        img = np.asarray(Image.open(path).convert("RGB"))
        if name.endswith("-eyes"):
            w = img.shape[1] // 2
            frames.append((name, img[:, :w], img[:, w:]))
        elif name.startswith("d2igl"):
            frames.append((name, img[0::2], img[1::2]))
        else:
            frames.append((name, img, None))
    return frames


def true_disparity(left, right, flip=False):
    """Disparity of the left eye, larger nearer, and where it can be trusted.

    The frames are small and their textures alias differently in each eye,
    so both are blurred slightly, only confident SGBM matches are kept, and
    matches that disagree with their neighbourhood are dropped. `flip` is for
    a title whose eyes come out swapped."""
    gl = cv2.GaussianBlur(cv2.cvtColor(left, cv2.COLOR_RGB2GRAY), (0, 0), 1.2)
    gr = cv2.GaussianBlur(cv2.cvtColor(right, cv2.COLOR_RGB2GRAY), (0, 0), 1.2)
    sgbm = cv2.StereoSGBM_create(minDisparity=-32, numDisparities=64, blockSize=11, uniquenessRatio=25,
                                 disp12MaxDiff=1, speckleWindowSize=200, speckleRange=1, P1=8 * 121, P2=32 * 121)
    d = sgbm.compute(gl, gr).astype(np.float32) / 16.0
    valid = d > -32
    med = cv2.medianBlur(np.where(valid, d, 0).astype(np.float32), 5)
    valid &= np.abs(med - d) <= 1
    return (-d if flip else d), valid


# ------------------------------------------------------------------ models

def prep(img, size, mult=14):
    """Resize so the short side is `size` (multiples of `mult`), normalise."""
    h, w = img.shape[:2]
    s = size / min(h, w)
    nh, nw = max(mult, int(round(h * s / mult)) * mult), max(mult, int(round(w * s / mult)) * mult)
    x = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_CUBIC).astype(np.float32) / 255.0
    return ((x - MEAN) / STD).transpose(2, 0, 1)[None]


class Ramp:
    name = "ramp (free baseline)"

    def __call__(self, img):
        h, w = img.shape[:2]
        return np.linspace(0, 1, h, dtype=np.float32)[:, None].repeat(w, 1)


class HF:
    def __init__(self, repo, size, label):
        from transformers import AutoModelForDepthEstimation
        self.m = AutoModelForDepthEstimation.from_pretrained(repo).eval()
        self.size, self.name = size, f"{label} @{size}"

    def __call__(self, img):
        out = self.m(pixel_values=torch.from_numpy(prep(img, self.size))).predicted_depth
        return out[0].numpy()       # relative inverse depth


class MidasSmall:
    def __init__(self):
        self.m = torch.hub.load("intel-isl/MiDaS", "MiDaS_small", trust_repo=True).eval()
        self.name = "MiDaS v2.1 Small @256"

    def __call__(self, img):
        return self.m(torch.from_numpy(prep(img, 256, mult=32)))[0].numpy()


class DA3Onnx:
    def __init__(self, size):
        import onnxruntime as ort
        path = glob.glob(os.path.expanduser(
            "~/.cache/huggingface/hub/models--onnx-community--depth-anything-v3-small/snapshots/*/onnx/model.onnx"))[0]
        self.s = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
        self.size, self.name = size, f"Depth Anything V3 Small @{size}"

    def __call__(self, img):
        x = prep(img, self.size)[:, None]           # batch, views, C, H, W
        depth = self.s.run(["predicted_depth"], {"pixel_values": x})[0][0, 0]
        return 1.0 / np.maximum(depth, 1e-6)        # depth -> inverse depth


class FastDepth:
    """MIT's FastDepth (MobileNet, trained on NYU indoor scenes), the int8
    TFLite builds from STMicroelectronics' model zoo; square input, depth
    output. FASTDEPTH_DIR holds fastdepth_SIZE_int8.tflite."""

    def __init__(self, size):
        from ai_edge_litert.interpreter import Interpreter
        path = os.path.join(os.environ.get("FASTDEPTH_DIR", "."), f"fastdepth_{size}_int8.tflite")
        self.i = Interpreter(path)
        self.i.allocate_tensors()
        self.inp, self.out = self.i.get_input_details()[0], self.i.get_output_details()[0]
        self.size, self.name = size, f"FastDepth int8 @{size}"

    def __call__(self, img):
        x = cv2.resize(img, (self.size, self.size), interpolation=cv2.INTER_LINEAR)[None]
        self.i.set_tensor(self.inp["index"], x.astype(np.uint8))
        self.i.invoke()
        scale, zero = self.out["quantization"]
        depth = (self.i.get_tensor(self.out["index"])[0, :, :, 0].astype(np.float32) - zero) * scale
        return 1.0 / np.maximum(depth, 1e-3)     # depth -> inverse depth


def build_models():
    return [
        Ramp(),
        MidasSmall(),
        FastDepth(256),
        FastDepth(320),
        HF("Intel/dpt-swinv2-tiny-256", 256, "DPT-SwinV2-Tiny"),
        HF("depth-anything/Depth-Anything-V2-Small-hf", 518, "Depth Anything V2 Small"),
        HF("depth-anything/Depth-Anything-V2-Small-hf", 252, "Depth Anything V2 Small"),
        HF("depth-anything/Depth-Anything-V2-Small-hf", 196, "Depth Anything V2 Small"),
        HF("xingyang1/Distill-Any-Depth-Small-hf", 518, "Distill-Any-Depth Small"),
        HF("xingyang1/Distill-Any-Depth-Small-hf", 252, "Distill-Any-Depth Small"),
        DA3Onnx(504),
        DA3Onnx(252),
    ]


# ------------------------------------------------------------------ run

def colour(d):
    d = d.astype(np.float32)
    lo, hi = np.percentile(d, 2), np.percentile(d, 98)
    u8 = np.clip((d - lo) / max(hi - lo, 1e-6) * 255, 0, 255).astype(np.uint8)
    return cv2.applyColorMap(u8, cv2.COLORMAP_TURBO)[:, :, ::-1]


def main(folder, out):
    os.makedirs(out, exist_ok=True)
    frames = load_frames(folder)
    models = build_models()
    # MadSpace's eyes come out swapped: its nearest floor has the least disparity
    truth = {n: true_disparity(l, r, flip=n.startswith("ms-")) for n, l, r in frames if r is not None}
    tile_h, tile_w = 120, 192
    sheet = np.zeros(((len(frames)) * tile_h, (len(models) + 2) * tile_w, 3), np.uint8)
    results = []
    for mi, m in enumerate(models):
        scores, times = [], []
        for fi, (name, left, right) in enumerate(frames):
            m(left)                                          # warm up
            t0 = time.perf_counter()
            for _ in range(3):
                pred = m(left)
            times.append((time.perf_counter() - t0) / 3 * 1000)
            pred = cv2.resize(pred.astype(np.float32), (left.shape[1], left.shape[0]), interpolation=cv2.INTER_LINEAR)
            if name in truth:
                disp, valid = truth[name]
                if valid.sum() > 200:
                    scores.append(spearmanr(pred[valid], disp[valid]).correlation)
            sheet[fi * tile_h:(fi + 1) * tile_h, (mi + 2) * tile_w:(mi + 3) * tile_w] = \
                cv2.resize(colour(pred), (tile_w, tile_h), interpolation=cv2.INTER_NEAREST)
            if mi == 0:
                sheet[fi * tile_h:(fi + 1) * tile_h, 0:tile_w] = cv2.resize(left, (tile_w, tile_h), interpolation=cv2.INTER_NEAREST)
                if name in truth:
                    disp, valid = truth[name]
                    c = colour(np.where(valid, disp, np.median(disp[valid])))
                    c[~valid] = 0
                    sheet[fi * tile_h:(fi + 1) * tile_h, tile_w:2 * tile_w] = cv2.resize(c, (tile_w, tile_h), interpolation=cv2.INTER_NEAREST)
        results.append((m.name, float(np.mean(scores)) if scores else float("nan"),
                        float(np.min(scores)) if scores else float("nan"), float(np.median(times))))
        print(f"{m.name:34s} rho mean {results[-1][1]:+.3f} min {results[-1][2]:+.3f}  cpu {results[-1][3]:7.1f} ms", flush=True)
    Image.fromarray(sheet).save(os.path.join(out, "sheet.png"))
    with open(os.path.join(out, "results.txt"), "w") as f:
        f.write("columns: frame | true disparity | " + " | ".join(m.name for m in models) + "\n")
        for r in results:
            f.write(f"{r[0]:34s} rho mean {r[1]:+.3f} min {r[2]:+.3f}  cpu {r[3]:7.1f} ms\n")
        f.write("scored frames: " + ", ".join(sorted(truth)) + "\n")
        for n in sorted(truth):
            f.write(f"  {n}: {int(truth[n][1].sum())} trusted pixels\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
