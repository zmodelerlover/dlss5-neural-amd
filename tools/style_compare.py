"""Compare a Neural Rendering style against what this add-on would do to the same frame.

Two lossless screenshots of the SAME scene, taken on any machine (NVIDIA through RenoDX or
OptiScaler, or AMD through this add-on): one with the style off (Model A / Default) and one with
a style on (Model B / Natural, Model C / Cinematic). This applies the add-on's own grading chain
to the "off" shot and measures how far the result is from the "on" shot.

    python tools/style_compare.py --off default.png --on natural.png --style 1
    python tools/style_compare.py --off default.png --on cinematic.png --style 2 --floor default2.png

The chain is the one in neural.cpp / docs/styles-model-abc.md, read from the kernel:
    exposure   c = saturate(c * 2^k75)
    contrast   c = saturate(c + k77 * (smoothstep(c) - c))
    saturation HSL: S' = saturate(S * (1 + k78)), hue and lightness held
with Model B = (-0.10, -0.25, -0.10) and Model C = (0, 0, -0.15), scaled by --strength.

It also grid-searches the three coefficients that best explain the "on" shot, so a mismatch says
which knob is wrong and by how much rather than just "different". Needs numpy and Pillow.
"""
import argparse
import itertools
import sys

import numpy as np
from PIL import Image

STYLES = {1: (-0.10, -0.25, -0.10), 2: (0.0, 0.0, -0.15)}


def load(path, width):
    img = Image.open(path).convert("RGB")
    if width and img.width > width:
        img = img.resize((width, int(img.height * width / img.width)), Image.BOX)
    return np.asarray(img, dtype=np.float32) / 255.0


def grade(c, expo, con, sat):
    c = np.clip(c * (2.0 ** expo), 0.0, 1.0)
    c = np.clip(c + con * (c * c * (3.0 - 2.0 * c) - c), 0.0, 1.0)
    M = c.max(axis=2, keepdims=True)
    m = c.min(axis=2, keepdims=True)
    d = M - m
    L = (M + m) * 0.5
    denom = np.where(L > 0.5, 2.0 - M - m, M + m)
    S = np.where(d > 1e-6, d / np.maximum(denom, 1e-6), 0.0)
    Sp = np.clip(S * (1.0 + sat), 0.0, 1.0)
    ratio = np.where(S > 1e-6, Sp / np.maximum(S, 1e-6), 1.0)
    c = np.where(d > 1e-6, L + ratio * (c - L), c)
    return np.clip(c, 0.0, 1.0)


def mae(a, b):
    return float(np.abs(a - b).mean())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--off", required=True, help="screenshot with the style off (Model A / Default)")
    ap.add_argument("--on", required=True, help="screenshot with the style on")
    ap.add_argument("--style", type=int, choices=(1, 2), required=True, help="1 = Model B / Natural, 2 = Model C / Cinematic")
    ap.add_argument("--strength", type=float, default=1.0, help="LocalToneStrength / StyleStrength used, 0..1")
    ap.add_argument("--floor", help="a second style-off shot of the same scene, to measure the noise floor")
    ap.add_argument("--width", type=int, default=640, help="downscale to this width before comparing (0 = full)")
    ap.add_argument("--no-fit", action="store_true", help="skip the coefficient search")
    a = ap.parse_args()

    off = load(a.off, a.width)
    on = load(a.on, a.width)
    if off.shape != on.shape:
        sys.exit(f"shape mismatch: {off.shape} vs {on.shape}; the two shots must be the same size")
    t = max(0.0, min(1.0, a.strength))
    expo, con, sat = (k * t for k in STYLES[a.style])
    pred = grade(off, expo, con, sat)

    print(f"style {a.style} at strength {t:.2f}: exposure {expo:+.3f} stops, contrast {con:+.3f}, saturation {sat:+.3f}")
    print(f"  on  vs off        : MAE {mae(on, off):.5f}   (how much the style changed the picture there)")
    print(f"  pred vs off       : MAE {mae(pred, off):.5f}   (how much our chain changes it)")
    print(f"  on  vs pred       : MAE {mae(on, pred):.5f}   (the disagreement; this is the number that matters)")
    for i, ch in enumerate("RGB"):
        print(f"    {ch}: on-pred mean {float((on[..., i] - pred[..., i]).mean()):+.5f}   |on-pred| p95 {float(np.percentile(np.abs(on[..., i] - pred[..., i]), 95)):.5f}")
    if a.floor:
        floor = load(a.floor, a.width)
        print(f"  off vs off(again) : MAE {mae(off, floor):.5f}   (noise floor: anything below this is not evidence)")

    if a.no_fit:
        return
    best = None
    grid_e = np.arange(-0.40, 0.201, 0.05)
    grid_c = np.arange(-0.60, 0.301, 0.05)
    grid_s = np.arange(-0.40, 0.201, 0.05)
    small_off = load(a.off, 320)
    small_on = load(a.on, 320)
    for e, c, s in itertools.product(grid_e, grid_c, grid_s):
        err = mae(small_on, grade(small_off, e, c, s))
        if best is None or err < best[0]:
            best = (err, e, c, s)
    err, e, c, s = best
    print(f"  best fit of our chain to the 'on' shot: exposure {e:+.2f}, contrast {c:+.2f}, saturation {s:+.2f}  (MAE {err:.5f})")
    print("  read: if the fit lands on the table values, the chain is right and any residual is noise or")
    print("  history drift; if it lands elsewhere, the slot it moved is the one whose kernel reading is off;")
    print("  if no fit gets near the floor, the difference is not a grade of the output at all.")


if __name__ == "__main__":
    main()
