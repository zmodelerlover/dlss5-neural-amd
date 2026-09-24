"""Cut a stretch of a lossless game recording into a sequence folder, for framecheck --seq and seq_player.

    python tools/seq_from_video.py <video.mkv> --start 200 --count 64 --out lab/real/gow2-calm

Recorded play has no true motion, so the sequence says so (truth: false, no mv files: the metrics read
zero motion) and its valid mask is where the picture did not move: every channel within 1/255 of the
frame before, over a 5x5 neighbourhood. Flicker is then measured only where the input held still,
the reference's own flicker_stats "static" measure, with nothing estimated.
"""
import argparse
import json
import subprocess
from pathlib import Path

import numpy as np


def load_ppm(path):
    parts = path.read_bytes().split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8).reshape(h, w, 3)


def still(cur, prev, radius=2):
    """Pixels whose whole (2r+1)^2 neighbourhood changed by at most one step."""
    moved = (np.abs(cur.astype(np.int16) - prev).max(axis=-1) > 1).astype(np.int32)
    k = 2 * radius + 1
    c = np.pad(np.pad(moved, radius), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) == 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("video", type=Path)
    p.add_argument("--start", type=int, required=True)
    p.add_argument("--count", type=int, required=True)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    subprocess.run(["ffmpeg", "-v", "error", "-i", str(a.video), "-vf",
                    f"select=between(n\\,{a.start}\\,{a.start + a.count - 1})", "-fps_mode", "passthrough",
                    "-start_number", "0", str(a.out / "frame%04d.ppm")], check=True)
    frames = sorted(a.out.glob("frame*.ppm"))
    assert len(frames) == a.count, f"got {len(frames)} frames"
    prev, cover = None, []
    for i, f in enumerate(frames):
        cur = load_ppm(f)
        v = np.ones(cur.shape[:2], bool) if prev is None else still(cur, prev)
        v.astype(np.uint8).tofile(a.out / f"valid{i:04d}.u8")
        cover.append(float(v.mean()))
        prev = cur
    h, w = prev.shape[:2]
    meta = dict(name=a.out.name, width=w, height=h, frames=a.count, truth=False,
                source=a.video.name, start=a.start, still_share=round(float(np.mean(cover[1:])), 3))
    (a.out / "meta.json").write_text(json.dumps(meta, indent=1))
    print(json.dumps(meta))


if __name__ == "__main__":
    main()
