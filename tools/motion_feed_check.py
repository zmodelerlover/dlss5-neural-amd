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
is not there, and the estimator took the motion over without a word. And the companion effect's field
comes only from the provider its AMDNR_MV_PROVIDER samples (issue #18: any provider used to do, and the
field arrived all zero), paired here row by row with the effect's blocks; two all-zero readings hand
motion back.

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


def probe_zeroed(v16, cap):
    """probes.inc: a half with all ones in its exponent (inf, NaN), or longer than FeedCap()."""
    h = np.asarray(v16, np.float16)
    with np.errstate(invalid="ignore"):
        return (((h.view(np.uint16) & 0x7C00) == 0x7C00) | (np.abs(h.astype(np.float32)) > cap)).any(axis=-1)


# The guide probe's count of what the feed will zero is the feed's own rule, on the field as it came.
field = np.array([[65504, 0], [-65504, 3], [np.inf, 0], [np.nan, 1], [3.5, -2.0], [1306, 0],
                  [1307, 0], [0, -1307], [-0.0, 0], [0, 0]], np.float16)
shader = ((field.astype(np.float32).view(np.uint32) & 0x7FFFFFFF) > np.float32(cap).view(np.uint32)).any(axis=-1)
assert probe_zeroed(field, cap).tolist() == shader.tolist() == [True] * 4 + [False, False, True, True, False, False], \
    "the probe's zeroed share is not the feed's rule"

inc = (ROOT / "core/temporal/motion_feed.inc").read_text()
cpp = (ROOT / "core/addon/neural.cpp").read_text()
probes = (ROOT / "core/addon/probes.inc").read_text()
host = (ROOT / "core/x86bridge/host64.cpp").read_text()
src = "".join(p.read_text(encoding="utf-8", errors="replace") for p in (ROOT / "core").rglob("*")
              if p.suffix in (".cpp", ".inc", ".h"))
flow = [" ".join(s.split()) for s in re.findall(r"opticalFlow\s*\.\s*(?:store|exchange)\s*\((.*?)\);", src, re.S)]
# Each provider the add-on looks for is the one the effect samples under its define (issue #18): the
# kMvProviders row shares a word with that block's AMDNR_PROVIDER_NAME, and with no other block's.
fx = (ROOT / "effects/AMD_Neural_Feed.fx").read_text(encoding="utf-8")
feed = (ROOT / "core/addon/feed_provider.inc").read_text(encoding="utf-8")
words = lambda text: {w for w in re.split(r"[^a-z0-9]+", text.lower()) if w and w not in ("fx", "motion")}
names = {int(n): words(name) for n, name in re.findall(
    r'#(?:el)?if AMDNR_MV_PROVIDER == (\d)[^#]*?(?:#[^d][^#]*?)*?#define AMDNR_PROVIDER_NAME "([^"]+)"', fx)}
names[0] = words(re.search(r'#else[^#]*#define AMDNR_PROVIDER_NAME "([^"]+)"', fx).group(1))
rows = re.findall(r'\{ "([^"]+)", "([^"]+)", (\d) \}', feed)
paired = [(f, d) for f, t, d in rows if {k for k, v in names.items() if v & words(f + " " + t)} == {int(d)}]
fails = [why for ok, why in [
    (len(names) == 5 and len(rows) >= 5 and len(paired) == len(rows),
     f"feed_provider.inc: kMvProviders does not pair with the effect's blocks ({len(paired)} of {len(rows)} rows, "
     f"{len(names)} blocks)"),
    ("FeedProviderOn(below)" in cpp and "!g.feedMotionZero" in cpp and "g.feedMotionZero = true" in probes,
     "neural.cpp/probes.inc: motion from the effect no longer needs its own provider, or an all-zero field keeps it"),
    # A still menu or pause reads all zero honestly: only a scene that moved counts, and the effect is retried.
    ("else if (sceneMoved)\n                    ++g.feedZeroProbes;" in probes and "g.feedRetryAt = g.status.frame + wait;" in probes
     and "g.status.frame >= g.feedRetryAt" in cpp,
     "probes.inc/neural.cpp: a still scene can demote the effect's motion, or the demotion lasts the session"),
    ("asuint(v) & 0x7fffffff" in inc, "motion_feed.inc: the shader lost the bit compare"),
    (not re.search(r"maxPx\s*<=\s*0\.0f\)\s*\n\s*return;", inc), "motion_feed.inc: FeedMotion skips again with no cap set"),
    (re.search(r"\bFeedMotion\(cmd,\s*haveMotion\)", cpp) is not None, "neural.cpp: FeedMotion(cmd, haveMotion) is not called"),
    (not re.search(r"if\s*\(haveMotion\)\s*\n\s*FeedMotion", cpp), "neural.cpp: FeedMotion is gated on haveMotion again"),
    (re.search(r"^\s+DemoteMotion\(", probes, re.M) is not None,
     "probes.inc: the probe no longer demotes a field longer than the raster"),
    ("const float cap = FeedCap();" in probes
     and "return (h & 0x7c00) == 0x7c00 || std::abs(HalfToFloat(h)) > cap;" in probes
     and "past += zeroed(row[x * 2]) || zeroed(row[x * 2 + 1]) ? 1 : 0;" in probes,
     "probes.inc: the share the feed will zero is not counted by the feed's rule"),
    ("const float maxPx = FeedCap();" in inc, "motion_feed.inc: the feed and the probe do not share one cap"),
    ("!g.guideMotion.failed" in host, "host64.cpp: a demoted motion guide is still read on the x86 bridge"),
    (flow and all(re.fullmatch(r"AMDNR_WITH_FFX \? \w+ : 0", s) for s in flow)
     and not re.search(r"\.opticalFlow\s*=[^=]", src),
     "core: an OpticalFlow write is not forced to 0 without FFX, so a release build can drop the game's vectors"),
] if not ok]
if fails:
    print("\n".join(fails))
    sys.exit(1)
print("motion feed: PASS")
