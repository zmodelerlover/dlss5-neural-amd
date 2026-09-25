"""The guide-switching rule, and the two frames that broke it.

SettleGuide picks which of the game's own buffers is depth and which is motion, from a bind tally.
Picking from one frame was the first bug: the GTA San Andreas log has the depth guide leave a
buffer bound 96440 times for one bound 9 times, on the frame the game changed resolution and drew
no scene pass. Nothing demotes a chosen guide, so every frame after that fed the network the wrong
depth.

Making a challenger win three presents *running* fixed that and broke the cold start. An engine
that rotates two or three depth targets never presents the same one three times in a row, so the
challenger changed every present, the streak reset every present, and the slot stayed empty for as
long as the game ran -- depth silently gone, in a game that has depth. Property 7 below is that
game, and it fails against the streak rule alone.

So there are two rules, for two different questions. With nothing chosen there is no incumbent to
protect and the only question is having seen enough: the tally is left standing for three presents
and the leader of the total is taken. With an incumbent the question is stability, and a challenger
still has to win three presents running.

This is that decision written out once more, in the smallest form that can be asserted against. If
the two disagree, this file is the one that is wrong.

A take is also when the guide probe has to look again. It latched off after one good reading, or
five junk ones, and nothing but the effect's feed re-armed it: a game that moved to another buffer,
or re-took one after a resize, was never probed again, and on the 32-bit bridge a motion buffer once
demoted stayed demoted for the session, whatever the game took next. SettleGuide now says when it
took, every route hands that to RearmGuideProbe, and the 32-bit frontend carries it to the helper in
FRAME. Properties 11-12 hold the return value; the source checks at the end hold the call sites.

    python tools/guide_switch_check.py
"""
import re
from pathlib import Path

HOLD = 3  # presents, for both rules -- the same constant SettleGuide uses


class Guide:
    def __init__(self):
        self.chosen = None
        self.challenger = None
        self.frames = 0
        self.cold_frames = 0

    def settle(self, tally):
        """tally: {resource: binds}, kept across presents unless cleared. Mirrors SettleGuide,
        True when it took a buffer."""
        if not tally:
            tally.clear()
            return False
        best = max(tally, key=lambda r: tally[r])
        if best == self.chosen:
            self.challenger, self.frames = None, 0
            tally.clear()
            return False

        if self.chosen is None:
            self.cold_frames += 1
            if self.cold_frames < HOLD:
                return False  # deliberately not cleared: leaving it standing is what accumulates
            self.cold_frames = 0
        elif best != self.challenger:
            self.challenger, self.frames = best, 1
            tally.clear()
            return False
        else:
            self.frames += 1
            if self.frames < HOLD:
                tally.clear()
                return False

        self.chosen, self.challenger, self.frames = best, None, 0
        tally.clear()
        return True


def run(frames, clear_each=False, takes=None):
    """Each entry is one present's binds. The tally is the add-on's, and lives across presents.
    takes, if given, collects the index of every present that took a buffer."""
    g = Guide()
    tally = {}
    for i, present in enumerate(frames):
        for res, binds in present.items():
            tally[res] = tally.get(res, 0) + binds
        if g.settle(tally) and takes is not None:
            takes.append(i)
        if clear_each:
            tally.clear()
    return g.chosen


scene, odd, other = "scene", "odd", "other"

# 1. Nothing chosen yet: still three presents of evidence before anything is taken.
assert run([{scene: 96440}]) is None
assert run([{scene: 96440}] * 2) is None
assert run([{scene: 96440}] * 3) == scene

# 2. The first reported bug. One frame without the scene pass must not hand the guide away.
steady = [{scene: 96440}] * 10
assert run(steady + [{odd: 9}] + steady) == scene

# 3. Two odd frames are still not enough, and they do not accumulate across an interruption.
assert run(steady + [{odd: 9}, {odd: 9}] + steady) == scene
assert run(steady + [{odd: 9}, {scene: 96440}, {odd: 9}] + steady) == scene

# 4. A real switch still happens -- the game moved to a new buffer and keeps using it.
assert run(steady + [{other: 5000}] * 3) == other

# 5. Two challengers taking turns cancel each other out rather than either one winning.
assert run(steady + [{odd: 9}, {other: 9}] * 6) == scene

# 6. The incumbent winning the frame clears a challenger's streak, even at two.
assert run(steady + [{odd: 9}, {odd: 9}, {scene: 96440}, {odd: 9}, {odd: 9}]) == scene

# 7. The second reported bug: an engine that rotates its depth targets. Nothing is ever presented
#    three times running, and under the streak rule alone the slot stayed empty forever.
assert run([{"depthA": 96440}, {"depthB": 96440}] * 50) in ("depthA", "depthB")
assert run([{"depthA": 9}, {"depthB": 9}, {"depthC": 9}] * 33) in ("depthA", "depthB", "depthC")

# 8. Rotation does not cost the scene pass its win: a shadow map bound in every present still
#    loses to the scene pass, which outbinds it by an order of magnitude.
assert run([{scene: 96440, "shadow": 900}, {scene: 96440, "shadow": 900}] * 5) == scene

# 9. And a cold start is still not decided by one odd present on its own.
assert run([{odd: 9}] + [{scene: 96440}] * 2) == scene

# 10. The tally has to stand across those three presents. The 32-bit bridge used to clear it at the
#     end of every present, which left the third present alone to decide the cold start.
assert run([{scene: 96440}] * 2 + [{odd: 9}]) == scene
assert run([{scene: 96440}] * 2 + [{odd: 9}], clear_each=True) == odd

# 11. SettleGuide says it took exactly when it took: the cold take on the third present and a
#     challenger on its third win running. A steady frame, an odd one or a challenger short of its
#     streak is not a take, so the probe is not pushed back on every present.
takes = []
run(steady + [{odd: 9}, {odd: 9}] + steady + [{other: 5000}] * 3 + [{other: 5000}] * 4, takes=takes)
assert takes == [2, 24], takes
takes = []
run([{"depthA": 96440}, {"depthB": 96440}] * 50, takes=takes)
assert takes == [2], takes

# 12. A resize empties the slot, and re-taking the same buffer is a take too: the probe looks again,
#     so a buffer demoted as not velocity is demoted again rather than trusted because it came back.
g, tally = Guide(), {}


def present(binds):
    for res, b in binds.items():
        tally[res] = tally.get(res, 0) + b
    return g.settle(tally)


assert [present({scene: 96440}) for _ in range(4)] == [False, False, True, False]
g.chosen = None  # ReleaseSwapchainSized
assert [present({scene: 96440}) for _ in range(3)] == [False, False, True]

# The source: every take reaches RearmGuideProbe, on every route that picks a guide.
root = Path(__file__).resolve().parent.parent


def src(rel):
    return (root / rel).read_text(encoding="utf-8")


fails = []


def need(ok, what):
    if not ok:
        fails.append(what)


choice = src("core/shared/guide_choice.h")
settle_fn = choice[choice.index("bool SettleGuide("):choice.index("} // namespace guides")]
need(settle_fn.count("return true;") == 1 and re.search(
    r"guide\.chosen = best->res;.*tally\.clear\(\);\s*return true;\s*\}\s*$", settle_fn, re.S),
    "guide_choice.h: SettleGuide returns true only after the take")
need("return;" not in settle_fn, "guide_choice.h: a SettleGuide path returns nothing")

probes = src("core/addon/probes.inc")
rearm = re.search(r"void RearmGuideProbe\(\)\s*\{(.*?)\n\}", probes, re.S)
need(rearm is not None and "g.probeGuides.store(true);" in rearm.group(1)
     and "g.nextGuideProbe = g.status.frame + 120;" in rearm.group(1)
     and "g.junkProbes = 0;" in rearm.group(1),
     "probes.inc: RearmGuideProbe arms the probe 120 presents out and starts the junk count over")

neural = src("core/addon/neural.cpp")
need("probeGuides.store(true)" not in neural, "neural.cpp: re-arms the probe by hand, not RearmGuideProbe")
d12 = neural[neural.index("void SettleD3D12Depth()"):]
d12 = d12[:d12.index("\n}\n")]
need("RearmGuideProbe();" in d12 and
     d12.index("g.depthBest = best->res;") < d12.index("RearmGuideProbe();"),
     "neural.cpp: the D3D12 depth take does not re-probe")
feed = neural[neural.index("void AdoptFeedEffect()"):]
need("if (!first)\n        RearmGuideProbe();" in feed[:feed.index("\n}\n")],
     "neural.cpp: the effect's feed changing hands does not re-probe")

d3d11 = src("core/transport/d3d11/D3D11Transport.inc")
need(re.search(r"const bool tookDepth = SettleGuide\(g\.guideDepth, g_depthTally, Log\);\n"
               r"\s*if \(SettleGuide\(g\.guideMotion, g_motionTally, Log\) \|\| tookDepth\)\n"
               r"\s*RearmGuideProbe\(\);", d3d11),
     "D3D11Transport.inc: a take does not reach RearmGuideProbe")

front = src("core/x86bridge/frontend32.cpp")
need("if(SettleGuide(g.guideDepth,g_depthTally,Log))g.guideTaken|=1;"
     "if(SettleGuide(g.guideMotion,g_motionTally,Log))g.guideTaken|=2;" in front,
     "frontend32.cpp: a take is not recorded for FRAME")
need("f.guideTaken=g.guideTaken;g.guideTaken=0;" in front, "frontend32.cpp: FRAME does not carry the take")

host = src("core/x86bridge/host64.cpp")
work = host[host.index("Result FrameWork("):host.index("void Reply(")]
need("f.guideTaken<=3" in work, "host64.cpp: FRAME's guideTaken is not validated")
take = "if(f.guideTaken){if(f.guideTaken&2)g.guideMotion.failed=false;RearmGuideProbe();}"
need(take in work and work.index(take) < work.index("g.guideMotion.ready="),
     "host64.cpp: a take from the frontend does not re-probe, or reaches motion a frame late")

# No call site may drop what SettleGuide returns.
for rel in ("core/transport/d3d11/D3D11Transport.inc", "core/x86bridge/frontend32.cpp"):
    for m in re.finditer(r"SettleGuide\(g\.", src(rel)):
        before = src(rel)[:m.start()].rstrip()
        need(before.endswith(("if(", "if (", "=")), f"{rel}: a SettleGuide call drops its result")

if fails:
    for f in fails:
        print("FAIL", f)
    raise SystemExit(1)
print("guide switching: 12 properties hold, and every take re-probes")
