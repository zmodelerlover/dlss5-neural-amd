"""Synthetic frame sequences with known motion, for the temporal instrument.

    python tools/seqgen.py --base a.png --base2 b.png --out corpus [--size 960x540] [--frames 48]

One folder per sequence, read by `amd-nr-framecheck.exe --seq <folder>`:
  frame<i>.ppm  what the game presented (P6, RGB8)
  mv<i>.f32     float32 x,y per pixel, in pixels, prev = cur + mv (the FFX and estimator convention),
                what a game's velocity buffer would say, HUD included
  valid<i>.u8   1 where the pixel has a true, visible correspondence in frame i-1 and is not HUD
  hud.u8        1 on the static HUD (hud-over-pan only)
  meta.json     name, size, frame count, cut frame, frames whose content changes without motion

Sub-pixel motion is exact: the world is drawn at 4x and every frame is a 4x4 box average of an
integer-shifted window, so a 0.25 px pan is a 1 texel shift up there and no frame is blurrier
than another because of its phase.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

SS = 4


def world_from(path, w, h, margin):
    """The base image at 1x, sized to cover the view plus every pan, then drawn at SS x."""
    im = Image.open(path).convert("RGB")
    ww, wh = w + margin, h + margin
    scale = max(ww / im.width, wh / im.height)
    im = im.resize((max(ww, round(im.width * scale)), max(wh, round(im.height * scale))), Image.LANCZOS)
    im = im.crop((0, 0, ww, wh))
    return np.asarray(im.resize((ww * SS, wh * SS), Image.BICUBIC), dtype=np.float32) / 255.0


def view(world4, ox4, oy4, w, h):
    """The w x h frame whose top-left sits at (ox4, oy4) in 4x texels."""
    win = world4[oy4:oy4 + h * SS, ox4:ox4 + w * SS]
    return win.reshape(h, SS, w, SS, 3).mean(axis=(1, 3))


def inside(w, h, mv):
    """Where cur + mv still lands on the screen."""
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    px, py = xs + mv[..., 0], ys + mv[..., 1]
    return (px >= 0) & (px <= w - 1) & (py >= 0) & (py <= h - 1)


class Writer:
    def __init__(self, root, name, w, h, **meta):
        self.dir = Path(root) / name
        self.dir.mkdir(parents=True, exist_ok=False)
        self.w, self.h, self.n = w, h, 0
        self.meta = dict(name=name, width=w, height=h, **meta)

    def frame(self, rgb, mv, valid):
        q = np.clip(np.rint(rgb * 255.0), 0, 255).astype(np.uint8)
        (self.dir / f"frame{self.n:04d}.ppm").write_bytes(b"P6\n%d %d\n255\n" % (self.w, self.h) + q.tobytes())
        mv.astype(np.float32).tofile(self.dir / f"mv{self.n:04d}.f32")
        valid.astype(np.uint8).tofile(self.dir / f"valid{self.n:04d}.u8")
        self.n += 1

    def close(self):
        self.meta["frames"] = self.n
        (self.dir / "meta.json").write_text(json.dumps(self.meta, indent=2))


def pan(out, name, world4, w, h, frames, v4, first=0, writer=None):
    """Camera moving v4 texels (4x) a frame: the content moves the other way, so mv = +v."""
    wr = writer or Writer(out, name, w, h, pan_px=[v4[0] / SS, v4[1] / SS])
    mv = np.empty((h, w, 2), np.float32)
    mv[..., 0], mv[..., 1] = v4[0] / SS, v4[1] / SS
    ox0 = SS * 8 if v4[0] >= 0 else world4.shape[1] - w * SS - SS * 8
    oy0 = SS * 8 if v4[1] >= 0 else world4.shape[0] - h * SS - SS * 8
    for t in range(frames):
        valid = inside(w, h, mv) if t > first else np.zeros((h, w), bool)
        wr.frame(view(world4, ox0 + v4[0] * t, oy0 + v4[1] * t, w, h), mv, valid)
    return wr


def static(out, name, base, frames, dither, seed):
    h, w = base.shape[:2]
    rng = np.random.default_rng(seed)
    wr = Writer(out, name, w, h, dither_255=dither)
    zero = np.zeros((h, w, 2), np.float32)
    for t in range(frames):
        noise = rng.integers(-dither, dither + 1, size=base.shape).astype(np.float32) / 255.0
        wr.frame(base + noise, zero, np.full((h, w), t > 0))
    return wr


def sprite_run(out, name, base, sprite, frames, speed):
    """A textured disc crossing a still background, bouncing off the edges."""
    h, w = base.shape[:2]
    sh, sw = sprite.shape[:2]
    yy, xx = np.mgrid[0:sh, 0:sw]
    disc = ((xx - sw / 2 + 0.5) / (sw / 2)) ** 2 + ((yy - sh / 2 + 0.5) / (sh / 2)) ** 2 <= 1.0
    span = w - sw
    wr = Writer(out, name, w, h, speed_px=speed)

    def place(t):
        s = (t * speed) % (2 * span)
        return int(s if s <= span else 2 * span - s), (h - sh) // 2

    prev_mask = None
    px_prev = None
    for t in range(frames):
        x0, y0 = place(t)
        rgb = base.copy()
        mask = np.zeros((h, w), bool)
        mask[y0:y0 + sh, x0:x0 + sw] = disc
        rgb[mask] = sprite[disc]
        mv = np.zeros((h, w, 2), np.float32)
        if t > 0:
            mv[mask, 0] = px_prev - x0  # where it was, relative to where it is
            valid = np.where(mask, inside(w, h, mv), ~prev_mask)
        else:
            valid = np.zeros((h, w), bool)
        wr.frame(rgb, mv, valid)
        prev_mask, px_prev = mask, x0
    return wr


def hud_layer(w, h):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((20, h - 70, 320, h - 40), fill=(20, 20, 20, 200), outline=(255, 255, 255, 255), width=2)
    d.rectangle((24, h - 66, 220, h - 44), fill=(200, 40, 40, 255))
    d.text((w - 260, 24), "SCORE 0012345   AMMO 30/120", fill=(255, 255, 255, 255), stroke_width=2,
           stroke_fill=(0, 0, 0, 255))
    d.ellipse((w // 2 - 6, h // 2 - 6, w // 2 + 6, h // 2 + 6), outline=(255, 255, 255, 255), width=2)
    a = np.asarray(im, dtype=np.float32) / 255.0
    return a[..., :3], a[..., 3:4]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--base", type=Path, required=True)
    p.add_argument("--base2", type=Path, required=True, help="the other scene: cut target and sprite")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--size", default="960x540")
    p.add_argument("--frames", type=int, default=48)
    p.add_argument("--only", default="", help="comma list of sequence names")
    a = p.parse_args()
    w, h = map(int, a.size.lower().split("x"))
    n = a.frames
    margin = 3 * n + 64
    world = world_from(a.base, w, h, margin)
    world2 = world_from(a.base2, w, h, margin)
    still = view(world, SS * 8, SS * 8, w, h)
    still2 = view(world2, SS * 8, SS * 8, w, h)
    cut = min(30, n - 8)

    def cut_seq(out, name):
        wr = pan(out, name, world, w, h, cut, (8, 4))
        wr.meta["cut"] = cut
        # The second scene has no correspondence in the first, while the game still reports the
        # camera's motion across the cut, as an engine would.
        return pan(out, name, world2, w, h, n - cut, (8, 4), writer=wr)

    def hud_seq(out, name):
        rgb_h, a_h = hud_layer(w, h)
        mask = a_h[..., 0] > 0
        wr = Writer(out, name, w, h, pan_px=[2, 1])
        mask.astype(np.uint8).tofile(wr.dir / "hud.u8")
        mv = np.zeros((h, w, 2), np.float32)
        mv[..., 0], mv[..., 1] = 2, 1
        for t in range(n):
            bg = view(world, SS * 8 + 8 * t, SS * 8 + 4 * t, w, h)
            valid = (inside(w, h, mv) & ~mask) if t > 0 else np.zeros((h, w), bool)
            wr.frame(bg * (1 - a_h) + rgb_h * a_h, mv, valid)
        return wr

    def flicker_seq(out, name):
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
        glow = np.exp(-(((xx - w * 0.6) / (w * 0.12)) ** 2 + ((yy - h * 0.4) / (h * 0.15)) ** 2))[..., None]
        # The light changes the picture where nothing moves: those frames are the test, not a flaw.
        wr = Writer(out, name, w, h, light_period=6, content_changes=list(range(3, n, 3)))
        zero = np.zeros((h, w, 2), np.float32)
        for t in range(n):
            on = (t // 3) % 2 == 1
            wr.frame(still * (1 + (0.6 * glow if on else 0)), zero, np.full((h, w), t > 0))
        return wr

    def fine_seq(out, name):
        yy, xx = np.mgrid[0:h, 0:w]
        grid = ((xx % 2 == 0) | (yy % 3 == 0)).astype(np.float32)[..., None]
        checker = (((xx // 1) + (yy // 1)) % 2).astype(np.float32)[..., None]
        band = (xx > w // 2)[..., None]
        base = 0.5 * still + 0.5 * np.where(band, checker, grid) * 0.8
        return static(out, name, base, n, 1, 7)

    sprite = still2[h // 2 - 90:h // 2 + 90, w // 2 - 90:w // 2 + 90]
    small = still2[h // 2 - 60:h // 2 + 60, w // 2 - 80:w // 2 + 80]
    seqs = {
        "static-dither": lambda o, s: static(o, s, still, n, 1, 1),
        "pan-int": lambda o, s: pan(o, s, world, w, h, n, (8, 4)),
        "pan-sub025": lambda o, s: pan(o, s, world, w, h, n, (1, 1)),
        "pan-sub05": lambda o, s: pan(o, s, world, w, h, n, (2, -1)),
        "object-fast": lambda o, s: sprite_run(o, s, still, small, n, 80),
        "disocclusion": lambda o, s: sprite_run(o, s, still, sprite, n, 6),
        "cut": cut_seq,
        "flicker-light": flicker_seq,
        "hud-over-pan": hud_seq,
        "fine-texture": fine_seq,
    }
    only = [s for s in a.only.split(",") if s]
    for name, make in seqs.items():
        if only and name not in only:
            continue
        make(a.out, name).close()
        print(f"{name}: {n} frames at {w}x{h}")


if __name__ == "__main__":
    main()
