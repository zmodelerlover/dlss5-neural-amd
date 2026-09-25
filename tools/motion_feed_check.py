"""The motion feed's cap, and the game buffer that needed it.

Tomb Raider's motion guide took a 960x540 R16G16_FLOAT that is not velocity: it reads 65504 px (FP16
max) on every pixel. The feed only ran when MotionMaxPx was set, so that field reached the runtime's
history warp and the output smooth as it stood, both fetched off the frame, and the screen showed the
network alone. Replayed through framecheck on a still scene: 3.72e-3 of flicker, the same as no history
at all, against 0.61e-3 with zero motion.

So the feed runs every frame and anything longer than the raster -- or inf, or NaN -- becomes zero, by
comparing the float's bits. The model below is that compare; the source checks say the feed still does
it, still always runs, and that the probe still hands such a field back to the estimator. One more says
OpticalFlow reads as 0 in a build without FFX: at 2 it passed over the game's vectors for a flow that
is not there, and the estimator took the motion over without a word.

    python tools/motion_feed_check.py
"""
import re
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent


def feed(v, have, cap):
    """kMotionFeedShader: scale by 0 when no source ran, then |v| bits against the cap's bits."""
    with np.errstate(invalid="ignore", over="ignore"):
        v = np.asarray(v, np.float32) * np.float32(1 if have else 0)  # 0*inf is NaN, caught below
    bad = ((v.view(np.uint32) & 0x7FFFFFFF) > np.float32(cap).view(np.uint32)).any(axis=-1)
    v[bad] = 0
    return v


cap = max(1306, 734)
assert not feed(np.full((4, 2), 65504, np.float16), True, cap).any(), "Tomb Raider's field must go"
assert not feed(np.full((4, 2), -65504, np.float16), True, cap).any()
odd = feed([[np.inf, 0], [np.nan, 1], [3.5, -2.0], [1306, 0], [1306.5, 0], [-0.0, 0]], True, cap)
assert odd[:2].tolist() == [[0, 0], [0, 0]], "inf and NaN become zero"
assert odd[2:4].tolist() == [[3.5, -2.0], [1306, 0]], "real vectors, up to the raster, pass untouched"
assert odd[4].tolist() == [0, 0], "past the raster is not motion"
assert not feed([[5, 5], [np.inf, 1]], False, cap).any(), "no source this frame: zero, never stale"

inc = (ROOT / "core/temporal/motion_feed.inc").read_text()
cpp = (ROOT / "core/addon/neural.cpp").read_text()
host = (ROOT / "core/x86bridge/host64.cpp").read_text()
src = "".join(p.read_text(encoding="utf-8", errors="replace") for p in (ROOT / "core").rglob("*")
              if p.suffix in (".cpp", ".inc", ".h"))
flow = [" ".join(s.split()) for s in re.findall(r"opticalFlow\s*\.\s*(?:store|exchange)\s*\((.*?)\);", src, re.S)]
fails = [why for ok, why in [
    ("asuint(v) & 0x7fffffff" in inc, "motion_feed.inc: the shader lost the bit compare"),
    (not re.search(r"maxPx\s*<=\s*0\.0f\)\s*\n\s*return;", inc), "motion_feed.inc: FeedMotion skips again with no cap set"),
    (re.search(r"\bFeedMotion\(cmd,\s*haveMotion\)", cpp) is not None, "neural.cpp: FeedMotion(cmd, haveMotion) is not called"),
    (not re.search(r"if\s*\(haveMotion\)\s*\n\s*FeedMotion", cpp), "neural.cpp: FeedMotion is gated on haveMotion again"),
    ("DemoteMotion(" in cpp, "neural.cpp: the probe no longer demotes a field longer than the raster"),
    ("!g.guideMotion.failed" in host, "host64.cpp: a demoted motion guide is still read on the x86 bridge"),
    (flow and all(re.fullmatch(r"AMDNR_WITH_FFX \? \w+ : 0", s) for s in flow)
     and not re.search(r"\.opticalFlow\s*=[^=]", src),
     "core: an OpticalFlow write is not forced to 0 without FFX, so a release build can drop the game's vectors"),
] if not ok]
if fails:
    print("\n".join(fails))
    sys.exit(1)
print("motion feed: PASS")
