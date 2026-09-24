"""Temporal stability of a framecheck sequence run, against the sequence's known motion.

    python tools/temporal_metrics.py --seq corpus/pan-int --run runs/pan-int [--spatial runs/pan-int-off]
                                     [--kind runtime] [--json out.json]
    python tools/temporal_metrics.py --seq corpus/pan-int          # the corpus's own truth check

Ported from the reference's flicker_stats.py (max over RGB of a frame-to-frame delta, mean, p99,
share above 1/255 and 4/255), with the delta taken after warping the previous frame by the true
motion and only over pixels that have a true correspondence -- so motion is not counted as flicker
and newly revealed pixels are not counted as either.

  TI          |out_t - warp(out_t-1)| on valid pixels; `gain` is TI_out / TI_in, the amplification.
              With the network skipping, t-1 is the last frame it evaluated and the warp follows
              the motion across the gap (`gap` per frame)
  detail      Laplacian variance of out / of in: below 1 is blur, the guard against "stable" meaning soft
  motion EPE  captured netMotion against the truth, raster pixels, on frames the network ran
  With --spatial (the same sequence with Temporal forced off):
  disocclusion  mean |out - spatial| where valid = 0 (not frame 0 or the cut): ghosting
  hud           mean |out - spatial| on the HUD mask: trails
  convergence   frames after the cut until |out - spatial| is back within 10% of its pre-cut level
"""
import argparse
import csv
import json
from pathlib import Path

import numpy as np

FORMATS = {10: (np.float16, 4), 28: (np.uint8, 4), 34: (np.float16, 2), 2: (np.float32, 4), 16: (np.float32, 2)}


def load_ppm(path):
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0


class Run:
    """The captures of one framecheck --seq run, by frame and kind."""

    def __init__(self, folder):
        self.dir = Path(folder)
        with (self.dir / "capture.csv").open() as f:
            self.shapes = {r["file"]: (int(r["width"]), int(r["height"]), int(r["dxgi_format"]))
                           for r in csv.DictReader(f)}
        with (self.dir / "seq.csv").open() as f:
            self.rows = {int(r["frame"]): r for r in csv.DictReader(f)}

    def get(self, t, kind):
        name = f"s{t:04d}-{kind}.raw"
        if name not in self.shapes:
            return None
        w, h, fmt = self.shapes[name]
        dtype, comps = FORMATS[fmt]
        a = np.fromfile(self.dir / name, dtype).reshape(h, w, comps).astype(np.float32)
        return a / 255.0 if dtype == np.uint8 else a

    def ran(self, t):
        return self.rows.get(t, {}).get("ran") == "1"


class RefRun:
    """The reference's DLSS5_FLICKER_DUMP captures (Magpie-DLSS5-AMD 0.29 over seq_player, five
    consecutive evaluations per folder; D:/lab-temporal/ref_run.ps1), placed on the sequence by
    content. A dump holds the network input (`color`: the FSR3 output fitted onto the network
    surface), `history` (the previous evaluation's output) and `motion` (after its feed), so
    evaluation k's output is dump k+1's history. The five inputs are placed as one block of
    consecutive frames, scored only where the sequence moves at all: in a still stretch neighbouring
    frames differ too little to place one frame alone."""

    def __init__(self, folders, seq):
        nw, nh = next(((w, h) for w, h in ((1280, 720), (1600, 900)) if seq.w <= w and seq.h <= h), (1920, 1080))
        # NativeInputGeometry::Make: the whole input fitted onto the network surface, centred
        if seq.w * nh >= seq.h * nw:
            fw, fh = nw, (seq.h * nw + seq.w // 2) // seq.w
        else:
            fw, fh = (seq.w * nh + seq.h // 2) // seq.h, nh
        ys, xs = np.mgrid[0:seq.h, 0:seq.w].astype(np.float32)
        gx, gy = (nw - fw) // 2 + (xs + .5) * fw / seq.w - .5, (nh - fh) // 2 + (ys + .5) * fh / seq.h - .5
        same = (nw, nh) == (seq.w, seq.h)
        to_seq = lambda a: a if same else sample(a, gx, gy)
        lo = hi = None
        for t in range(seq.n):
            f = seq.frame(t)
            lo, hi = (f, f) if lo is None else (np.minimum(lo, f), np.maximum(hi, f))
        moving = (hi - lo).max(axis=-1) > 4 / 255
        moving = moving if moving.any() else np.ones_like(moving)
        self.out, self.fed, self.motion, self.src, self.rows, self.placed = {}, {}, {}, {}, {}, []
        for folder in map(Path, folders):
            dumps = sorted(folder.glob("flicker-*-color.rgba16f"), key=lambda p: int(p.name.split("-")[2]))
            prefixes = [str(p)[:-len("-color.rgba16f")] for p in dumps]
            if not dumps:
                self.placed.append(dict(folder=folder.name, used=False, why="no dumps"))
                continue
            if any(p.stat().st_size != nw * nh * 8 for p in dumps):
                # The add-on hooks the first FFX dispatch it sees; now and then that is the FSR4 upscale
                # after it, and the network ran on the enlarged picture: not the route being measured.
                self.placed.append(dict(folder=folder.name, used=False, why="network surface is not the input's"))
                continue
            colors = [to_seq(np.fromfile(p + "-color.rgba16f", np.float16).reshape(nh, nw, 4)[..., :3]
                             .astype(np.float32)) for p in prefixes]
            err = np.array([[np.abs(c - f)[moving].mean() for c in colors] for f in map(seq.frame, range(seq.n))]).T
            n = len(colors)
            o = int(np.argmin([sum(err[k, o + k] for k in range(n)) for o in range(seq.n - n + 1)]))
            alone = err.argmin(axis=1).tolist()
            block = [float(err[k, o + k]) for k in range(n)]
            # Frames lost between dumps (the capture outran by a dump's stall) break the block: then
            # the block fits far worse than each frame alone, and the folder says nothing about flicker.
            fits = max(block) <= 2 * float(err.min(axis=1).max()) + 0.5 / 255
            self.placed.append(dict(folder=folder.name, frames=[o, o + n - 1], alone=alone, used=fits,
                                    err_255=[round(e * 255, 2) for e in block]))
            if not fits:
                continue
            for k, p in enumerate(prefixes):
                t = o + k
                self.fed[t], self.src[t] = colors[k], folder
                m = np.fromfile(p + "-motion.f32", np.float32).reshape(-1, 4)
                shape = (seq.h, seq.w) if m.shape[0] == seq.w * seq.h else (nh, nw)
                self.motion[t] = m.reshape(*shape, 4)[..., :2]
                if k + 1 < n:
                    hist = np.fromfile(prefixes[k + 1] + "-history.f32", np.float32).reshape(nh, nw, 4)[..., :3]
                    self.out[t], self.rows[t] = to_seq(hist), {"ran": "1"}

    def get(self, t, kind):
        return dict(runtime=self.out, stabilised=self.fed, motion=self.motion).get(kind, {}).get(t)

    def ran(self, t):
        return t in self.out

    def follows(self, t, t0):
        return t == t0 + 1 and self.src.get(t) == self.src.get(t0)


def resize_to(a, w, h):
    """Nearest resample of a (h', w', c) array onto w x h; the identity when sizes match."""
    if a.shape[1] == w and a.shape[0] == h:
        return a
    ys = (np.arange(h) * a.shape[0] // h)
    xs = (np.arange(w) * a.shape[1] // w)
    return a[ys][:, xs]


def warp(prev, mv):
    """prev sampled at cur + mv: the previous frame moved onto this one."""
    ys, xs = np.mgrid[0:prev.shape[0], 0:prev.shape[1]].astype(np.float32)
    return sample(prev, xs + mv[..., 0], ys + mv[..., 1])


def delta_stats(d):
    m = np.abs(d).max(axis=-1).ravel() if d.ndim > 1 else np.abs(d)
    if m.size == 0:
        return None
    return dict(mean=float(m.mean()), p99=float(np.percentile(m, 99)),
                gt1=float((m > 1 / 255).mean()), gt4=float((m > 4 / 255).mean()))


def textured(img, radius=4, floor=2 / 255):
    """Pixels whose (2r+1)^2 neighbourhood has a luma standard deviation above `floor`."""
    g = img[..., :3].mean(axis=-1).astype(np.float64)
    k = 2 * radius + 1
    pad = np.pad(g, radius, mode="edge")
    c1 = np.pad(pad, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    c2 = np.pad(pad ** 2, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    box = lambda c: c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    mean, sq = box(c1) / k ** 2, box(c2) / k ** 2
    return np.sqrt(np.maximum(sq - mean ** 2, 0)) > floor


def laplacian_var(img, mask):
    g = img[..., :3].mean(axis=-1)
    lap = -4 * g[1:-1, 1:-1] + g[:-2, 1:-1] + g[2:, 1:-1] + g[1:-1, :-2] + g[1:-1, 2:]
    m = mask[1:-1, 1:-1]
    return float(lap[m].var()) if m.any() else float("nan")


class Seq:
    def __init__(self, folder):
        self.dir = Path(folder)
        self.meta = json.loads((self.dir / "meta.json").read_text())
        self.w, self.h, self.n = self.meta["width"], self.meta["height"], self.meta["frames"]
        hud = self.dir / "hud.u8"
        self.hud = np.fromfile(hud, np.uint8).reshape(self.h, self.w).astype(bool) if hud.exists() else None

    def frame(self, t):
        return load_ppm(self.dir / f"frame{t:04d}.ppm")

    def mv(self, t):
        path = self.dir / f"mv{t:04d}.f32"
        if not path.exists():  # recorded play (seq_from_video): no truth, its valid mask is the still part
            return np.zeros((self.h, self.w, 2), np.float32)
        return np.fromfile(path, np.float32).reshape(self.h, self.w, 2)

    def valid(self, t):
        return np.fromfile(self.dir / f"valid{t:04d}.u8", np.uint8).reshape(self.h, self.w).astype(bool)


def mean_of(rows, key, sub=None):
    vals = [(r[key] if sub is None else r[key][sub]) for r in rows if r.get(key) is not None]
    vals = [v for v in vals if v == v]
    return float(np.mean(vals)) if vals else None


def chain(seq, t, t0):
    """Where each pixel of frame t was in frame t0 < t, following the true motion frame by frame,
    and whether that correspondence held all the way back."""
    ys, xs = np.mgrid[0:seq.h, 0:seq.w].astype(np.float32)
    x, y, ok = xs, ys, np.ones((seq.h, seq.w), bool)
    for k in range(t, t0, -1):
        xi = np.clip(np.rint(x), 0, seq.w - 1).astype(int)
        yi = np.clip(np.rint(y), 0, seq.h - 1).astype(int)
        ok &= seq.valid(k)[yi, xi]
        step = sample(seq.mv(k), x, y)
        x, y = x + step[..., 0], y + step[..., 1]
    return x, y, ok


def sample(img, x, y):
    """img at (x, y), bilinear, clamped."""
    h, w = img.shape[:2]
    x, y = np.clip(x, 0, w - 1), np.clip(y, 0, h - 1)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    top = img[y0, x0] * (1 - fx) + img[y0, x1] * fx
    bottom = img[y1, x0] * (1 - fx) + img[y1, x1] * fx
    return top * (1 - fy) + bottom * fy


def analyse(seq, run=None, spatial=None, kind="runtime"):
    """Per frame, against the last frame that has an output to compare with: the previous frame,
    or with the network skipping, the previous one it evaluated -- the motion chained across the
    gap, so a skip is measured as the jump the viewer sees and not as flicker."""
    frames, cut = [], seq.meta.get("cut")
    n = seq.n if run is None else min(seq.n, max(run.rows) + 1)
    # Frames whose output means something: the runtime's own buffer only changes when it ran,
    # while the composition is a new picture on every present.
    shown = [t for t in range(n) if run is None or kind != "runtime" or run.ran(t)]
    last = None
    for t in range(n):
        row = dict(frame=t)
        frames.append(row)
        if t not in shown:
            continue
        if last is not None and hasattr(run, "follows") and not run.follows(t, last[0]):
            last = None  # separate reference captures: their outputs do not follow one another
        cur_in = seq.frame(t)
        out = run.get(t, kind) if run else None
        out = resize_to(out[..., :3], seq.w, seq.h) if out is not None else None
        if last is not None:
            t0, prev_in, prev_out, _ = last
            x, y, valid = chain(seq, t, t0)
            row["gap"] = t - t0
            row["ti_in"] = delta_stats((cur_in - sample(prev_in, x, y))[valid])
            if out is not None and prev_out is not None:
                row["ti_out"] = delta_stats((out - sample(prev_out, x, y))[valid])
                if row["ti_in"] and row["ti_in"]["mean"] > 0:
                    row["gain"] = row["ti_out"]["mean"] / row["ti_in"]["mean"]
                if seq.hud is not None:
                    row["hud_ti"] = delta_stats((out - prev_out)[seq.hud])
        # What the network was actually handed, when the run captured it (the stabiliser's frame).
        fed = run.get(t, "stabilised") if run else None
        fed = resize_to(fed[..., :3], seq.w, seq.h) if fed is not None else None
        if fed is not None and last is not None and last[3] is not None:
            row["ti_fed"] = delta_stats((fed - sample(last[3], x, y))[valid])
        if out is not None:
            inner = np.ones((seq.h, seq.w), bool) if seq.hud is None else ~seq.hud
            row["detail"] = laplacian_var(out, inner) / max(laplacian_var(cur_in, inner), 1e-12)
            if run.ran(t) and t > 0 and seq.meta.get("truth", True):
                cap = run.get(t, "motion")
                if cap is not None:
                    k = cap.shape[1] / seq.w  # raster pixels per frame pixel
                    err = np.hypot(*np.moveaxis(resize_to(cap, seq.w, seq.h) - seq.mv(t) * k, -1, 0))
                    v = seq.valid(t)
                    if v.any():
                        row["epe_median"], row["epe_p95"] = float(np.median(err[v])), float(np.percentile(err[v], 95))
                    # Where there is something to match: a flat region has no motion to measure, and
                    # an optical flow rightly answers zero there.
                    tex = v & textured(cur_in)
                    if tex.any():
                        row["epe_tex_median"] = float(np.median(err[tex]))
                        row["epe_tex_p95"] = float(np.percentile(err[tex], 95))
            ref = spatial.get(t, kind) if spatial else None
            if ref is not None:
                d = np.abs(out - resize_to(ref[..., :3], seq.w, seq.h)).max(axis=-1)
                row["vs_spatial"] = float(d.mean())
                if t > 0 and t != cut:
                    hole = ~seq.valid(t) & (np.ones_like(d, bool) if seq.hud is None else ~seq.hud)
                    row["disocclusion"] = float(d[hole].mean()) if hole.any() else None
                if seq.hud is not None:
                    row["hud"] = float(d[seq.hud].mean())
        last = (t, cur_in, out, fed)
    summary = dict(sequence=seq.meta["name"], frames=n)
    body = [r for r in frames if r["frame"] > 1]
    for key in ("ti_in", "ti_fed", "ti_out", "hud_ti"):
        if any(r.get(key) for r in body):
            summary[key] = {s: mean_of(body, key, s) for s in ("mean", "p99", "gt1", "gt4")}
    for key in ("gain", "detail", "epe_median", "epe_p95", "epe_tex_median", "epe_tex_p95", "disocclusion", "hud",
                "vs_spatial"):
        summary[key] = mean_of(body, key)
    if cut is not None and spatial is not None:
        base = [r["vs_spatial"] for r in frames if 2 <= r["frame"] < cut and "vs_spatial" in r]
        after = [(r["frame"] - cut, r["vs_spatial"]) for r in frames if r["frame"] >= cut and "vs_spatial" in r]
        if base and after:
            level = np.mean(base) * 1.1
            summary["convergence"] = next((k for k, v in after if v <= level), None)
    return summary, frames


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--seq", type=Path, required=True)
    p.add_argument("--run", type=Path)
    p.add_argument("--spatial", type=Path)
    p.add_argument("--kind", default="runtime")
    p.add_argument("--json", type=Path)
    p.add_argument("--ref", type=Path, nargs="+", help="reference dump folders instead of --run")
    p.add_argument("--compare", type=Path, nargs="*", default=[],
                   help="seq_run cases, measured on exactly the frames the reference has")
    a = p.parse_args()
    seq = Seq(a.seq)
    run = RefRun(a.ref, seq) if a.ref else Run(a.run) if a.run else None
    summary, frames = analyse(seq, run, Run(a.spatial) if a.spatial else None, a.kind)
    if a.json:
        a.json.write_text(json.dumps(dict(summary=summary, frames=frames), indent=1))
    if a.ref:
        for placed in run.placed:
            print(placed)
        at = {r["frame"] for r in frames if r.get("ti_out")}
        print(f"\n{seq.meta['name']}: {len(at)} frame pairs {sorted(at)}  (x1e-3; TI on valid pixels)")
        print(f"{'case':34} {'TI_out':>7} {'>4/255':>7} {'gain':>6} {'detail':>6}")
        cases = [("reference", frames)] + [(str(c).replace("\\", "/").split("runs/")[-1],
                                            json.loads((c / seq.dir.name / "metrics.json").read_text())["frames"])
                                           for c in a.compare]
        for name, rows in cases:
            rows = [r for r in rows if r["frame"] in at]
            cells = [(mean_of(rows, "ti_out", "mean"), 1000, 7, 3), (mean_of(rows, "ti_out", "gt4"), 1000, 7, 2),
                     (mean_of(rows, "gain"), 1, 6, 2), (mean_of(rows, "detail"), 1, 6, 3)]
            print(f"{name:34} " + " ".join("-".rjust(w) if v is None else f"{v * s:{w}.{d}f}" for v, s, w, d in cells))
        return
    print(json.dumps(summary, indent=1))
    if a.run is None:
        # The corpus checks itself. Warped by its own truth the input must line up with the frame
        # before: exactly for whole-pixel motion, and for sub-pixel motion -- where a bilinear warp
        # of sharp edges cannot be exact -- at least twice as well as no motion or reversed motion.
        worst, bad = 0.0, []
        for t in (t for t in range(1, seq.n) if t not in seq.meta.get("content_changes", [])):
            v, mv, prev, cur = seq.valid(t), seq.mv(t), seq.frame(t - 1), seq.frame(t)
            err = {k: np.abs(cur - warp(prev, m)).max(axis=-1)[v] for k, m in (("truth", mv), ("zero", 0 * mv), ("neg", -mv))}
            if not v.any():
                continue
            p99 = float(np.percentile(err["truth"], 99))
            worst = max(worst, p99)
            moving = np.abs(mv[v]).max() > 0
            if p99 > 2.5 / 255 and not (moving and err["truth"].mean() * 2 < min(err["zero"].mean(), err["neg"].mean())):
                bad.append(t)
        print(f"truth check: worst p99 {worst * 255:.2f}/255, frames failing {bad or 'none'} ->", "FAIL" if bad else "OK")
        raise SystemExit(1 if bad else 0)


if __name__ == "__main__":
    main()
