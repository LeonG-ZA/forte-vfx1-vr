"""
export_fastdepth.py -- FastDepth's int8 TFLite model as a plain file the
app's own C code runs (src/vfxdepth): layers in order, weights and biases
dequantised to float32.

    python tools/depthtest/export_fastdepth.py fastdepth_256_int8.tflite fastdepth.vfd [frame.png]

The model is a chain of PAD, CONV_2D, DEPTHWISE_CONV_2D, RESIZE_NEAREST
(x2) and ADD (three skip connections), every layer ending in a ReLU. The
export is checked by running the file through a NumPy version of the same
layers and comparing with LiteRT on a frame: the C code then has a reference.

File (little-endian): "VFDN", u32 version 1, u32 tensors, u32 layers;
per tensor u32 h, w, c; per layer u32 type (1 pad, 2 conv, 3 depthwise,
4 resize x2, 5 add), u32 in, in2, out, k, stride, pad top, bottom, left,
right, f32 clamp (the largest value the int8 output could hold), then for
conv cout*k*k*cin floats (out, ky, kx, in) and cout biases, for depthwise
k*k*c floats (ky, kx, c) and c biases. Tensor 0 is the 256x256x3 input,
0..1 per channel; the last layer's output is depth.
"""
import struct
import sys

import numpy as np
from ai_edge_litert.interpreter import Interpreter, OpResolverType

PAD, CONV, DW, RESIZE, ADD = 1, 2, 3, 4, 5


def load(path):
    it = Interpreter(path, experimental_op_resolver_type=OpResolverType.BUILTIN_WITHOUT_DEFAULT_DELEGATES)
    it.allocate_tensors()
    td = {t["index"]: t for t in it.get_tensor_details()}
    return it, td


def deq(it, td, idx):
    t = td[idx]
    q = t["quantization_parameters"]
    v = it.get_tensor(idx).astype(np.float32)
    sc, zp = q["scales"].astype(np.float32), q["zero_points"].astype(np.float32)
    if len(sc) > 1:                                    # per channel, along the quantized dimension
        shape = [1] * v.ndim
        shape[q["quantized_dimension"]] = -1
        return (v - zp.reshape(shape)) * sc.reshape(shape)
    return (v - zp[0]) * sc[0] if len(sc) else v


def build(it, td):
    ops = it._get_ops_details()
    ids = {}

    def tid(i):
        if i not in ids:
            ids[i] = len(ids)
        return ids[i]

    layers = []
    first = ops[0]
    assert first["op_name"] == "QUANTIZE"
    ids[first["outputs"][0]] = 0                       # the quantized input is tensor 0
    for o in ops[1:]:
        name, ins, out = o["op_name"], [i for i in o["inputs"] if i >= 0], o["outputs"][0]
        if name == "QUANTIZE":                         # the final int8 -> uint8: same values
            continue
        oq = td[out]["quantization_parameters"]
        clamp = float((127 - oq["zero_points"][0]) * oq["scales"][0])
        L = dict(type=0, ins=[tid(i) for i in ins[:1]], in2=0, out=tid(out), k=0, stride=1, pads=(0, 0, 0, 0),
                 clamp=clamp, w=None, b=None)
        if name == "PAD":
            p = it.get_tensor(ins[1])
            L.update(type=PAD, pads=(int(p[1][0]), int(p[1][1]), int(p[2][0]), int(p[2][1])))
        elif name in ("CONV_2D", "DEPTHWISE_CONV_2D"):
            w = deq(it, td, ins[1])
            b = deq(it, td, ins[2])
            inq = td[ins[0]]["quantization_parameters"]
            wq = td[ins[1]]["quantization_parameters"]
            b = it.get_tensor(ins[2]).astype(np.float32) * inq["scales"][0] * wq["scales"].astype(np.float32)
            k = w.shape[1]
            hin, hout = td[ins[0]]["shape"][1], td[out]["shape"][1]
            stride = 1 if (hin - k) // 1 + 1 == hout else 2
            assert (hin - k) // stride + 1 == hout, (name, hin, hout, k)
            L.update(type=CONV if name == "CONV_2D" else DW, k=k, stride=stride, w=w.astype(np.float32),
                     b=b.astype(np.float32))
        elif name == "RESIZE_NEAREST_NEIGHBOR":
            L.update(type=RESIZE)
        elif name == "ADD":
            L.update(type=ADD, in2=tid(ins[1]))
        else:
            raise SystemExit("unhandled op " + name)
        layers.append(L)
    shapes = [None] * len(ids)
    for orig, new in ids.items():
        s = td[orig]["shape"]
        shapes[new] = (int(s[1]), int(s[2]), int(s[3]))
    return shapes, layers


def run(shapes, layers, img):
    """NumPy version of the C code: img is 256x256x3 uint8."""
    t = {0: img.astype(np.float32) / 255.0}
    for L in layers:
        x = t[L["ins"][0]]
        if L["type"] == PAD:
            a, b_, c, d = L["pads"]
            y = np.pad(x, ((a, b_), (c, d), (0, 0)))
        elif L["type"] == CONV:
            k, s, w = L["k"], L["stride"], L["w"]
            h = (x.shape[0] - k) // s + 1
            wd = (x.shape[1] - k) // s + 1
            y = np.zeros((h, wd, w.shape[0]), np.float32)
            for ky in range(k):
                for kx in range(k):
                    patch = x[ky:ky + s * h:s, kx:kx + s * wd:s, :]
                    y += patch @ w[:, ky, kx, :].T
            y += L["b"]
        elif L["type"] == DW:
            k, s, w = L["k"], L["stride"], L["w"][0]
            h = (x.shape[0] - k) // s + 1
            wd = (x.shape[1] - k) // s + 1
            y = np.zeros((h, wd, x.shape[2]), np.float32)
            for ky in range(k):
                for kx in range(k):
                    y += x[ky:ky + s * h:s, kx:kx + s * wd:s, :] * w[ky, kx, :]
            y += L["b"]
        elif L["type"] == RESIZE:
            y = x.repeat(2, axis=0).repeat(2, axis=1)
        else:
            y = x + t[L["in2"]]
        t[L["out"]] = np.clip(y, 0, L["clamp"])
    return t[layers[-1]["out"]][:, :, 0]


def write(path, shapes, layers):
    with open(path, "wb") as f:
        f.write(b"VFDN" + struct.pack("<III", 1, len(shapes), len(layers)))
        for s in shapes:
            f.write(struct.pack("<III", *s))
        for L in layers:
            f.write(struct.pack("<10If", L["type"], L["ins"][0], L["in2"], L["out"], L["k"], L["stride"],
                                *L["pads"], L["clamp"]))
            if L["type"] == CONV:
                f.write(L["w"].astype("<f4").tobytes())        # (out, ky, kx, in)
                f.write(L["b"].astype("<f4").tobytes())
            elif L["type"] == DW:
                f.write(L["w"][0].astype("<f4").tobytes())     # (ky, kx, c)
                f.write(L["b"].astype("<f4").tobytes())


def main(src, dst, frame=None):
    it, td = load(src)
    shapes, layers = build(it, td)
    write(dst, shapes, layers)
    print(f"{len(layers)} layers, {len(shapes)} tensors -> {dst}")
    from PIL import Image
    if frame:
        img = np.asarray(Image.open(frame).convert("RGB").resize((256, 256), Image.BILINEAR))
    else:
        rng = np.random.default_rng(1)
        img = rng.integers(0, 256, (256, 256, 3), dtype=np.uint8)
    inp, out = it.get_input_details()[0], it.get_output_details()[0]
    it.set_tensor(inp["index"], img[None])
    it.invoke()
    sc, zp = out["quantization"]
    ref = (it.get_tensor(out["index"])[0, :, :, 0].astype(np.float32) - zp) * sc
    mine = run(shapes, layers, img)
    c = np.corrcoef(ref.ravel(), mine.ravel())[0, 1]
    print(f"NumPy float vs LiteRT int8: correlation {c:.4f}, mean abs {np.abs(ref - mine).mean():.4f} "
          f"(depth range {ref.min():.2f}..{ref.max():.2f})")
    np.save(dst + ".ref.npy", np.stack([ref, mine]))
    np.save(dst + ".input.npy", img)


if __name__ == "__main__":
    main(*sys.argv[1:])
