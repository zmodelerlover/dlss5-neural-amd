"""The network is handed depth only while the guide probe finds it usable, on every route.

The probe said JUNK or FLAT and only logged it: haveDepth was set for any source that dispatched, so
kUseDepth handed the network a constant plane (a 32-bit game that clears depth before present) or
numbers that are not depth at all. Now SetDepthUsable (core/addon/probes.inc) holds the verdict and
RecordNetwork, which every route records through, ANDs it into kUseDepth. The rule, modelled below
and matched line for line against the drain:
  - only a reading of depth that frame fed counts (netDepth otherwise holds what it last did);
  - JUNK closes at once: it is never depth;
  - FLAT closes on the second reading of a moving scene; a still one (a menu) does not count;
  - a reading that varies opens and starts the FLAT count over, and so does every probe re-arm
    (a guide moved), which feeds a buffer not read yet, as before the gate existed;
  - five readings that are not real and moving end the probe, unless depth is withheld as FLAT: a
    menu that moves reads FLAT too, and stopping then would withhold the scene that follows in the
    same buffer for the session. So the probe looks on, 1200, 2400, then every 4800 presents.
netDepth is still written while closed, so the probe can open it again; the gate is on kUseDepth,
never on haveDepth. Both panels say "unusable" (the 32-bit one through WireStatus depthActive 2).

    python tools/depth_gate_check.py
"""
import re
import sys
from pathlib import Path


class Gate:
    def __init__(self):
        self.usable, self.flat = True, 0

    def set(self, usable):
        if usable:
            self.flat = 0
        self.usable = usable

    def reading(self, fed, junk=False, real=False, moving=True):
        if fed and junk:
            self.set(False)
        elif fed and not real and moving and self.bump() >= 2:
            self.set(False)
        elif fed and real:
            self.set(True)
        return self.usable

    def bump(self):
        self.flat += 1
        return self.flat

    def rearm(self):
        self.set(True)
        return self.usable


FLAT, VARIES, JUNK = dict(), dict(real=True), dict(junk=True)
g = Gate()
assert not g.reading(True, **JUNK), "JUNK closes at once"
g = Gate()
assert [g.reading(True, **FLAT) for _ in range(2)] == [True, False], "FLAT twice in a scene closes"
g = Gate()
assert all(g.reading(True, moving=False) for _ in range(9)), "a still menu never counts"
g = Gate()
assert g.reading(True, **FLAT) and g.reading(True, **VARIES) and g.reading(True, **FLAT), \
    "a reading that varies starts the FLAT count over"
g = Gate()
g.reading(True, **JUNK)
assert g.reading(True, **VARIES), "a reading that varies opens it again"
g = Gate()
g.reading(True, **FLAT)
assert g.rearm() and g.reading(True, **FLAT), "a re-arm starts the FLAT count over"
g.reading(True, **FLAT)
assert not g.usable and g.rearm(), "a re-arm opens it"
g = Gate()
assert all(g.reading(False, **k) for k in (JUNK, FLAT, FLAT, FLAT)), "depth not fed moves nothing"


class Probe(Gate):
    """The drain's schedule too: a first reading at present 120, then one every 600, and the stop
    rule after the verdict -- real and moving ends it, and so do five readings that are not, unless
    FLAT withholds depth, which backs the next reading off instead."""

    def run(self, scene, presents):
        on, junk, nxt, self.looks = True, 0, 120, 0
        for frame in range(presents):
            if not (on and frame >= nxt):
                continue
            nxt, self.looks = frame + 600, self.looks + 1
            fed, kind, moving = scene(frame)
            self.reading(fed, moving=moving, **kind)
            if kind.get("real") and moving:
                on = False
            else:
                junk += 1
                if junk >= 5 and self.flat < 2:
                    on = False
                elif junk >= 5:
                    nxt = frame + (600 << min(junk - 4, 3))
        self.on = on
        return self.usable


def menu_then_scene(frame):
    """Logos with no depth for 10 s, a moving FLAT menu to 40 s, a spinner to 50 s, then the scene,
    all in one depth buffer, at 60 fps."""
    if frame < 600:
        return False, FLAT, True
    return (True, FLAT, True) if frame < 3000 else (True, VARIES, True)


p = Probe()
assert p.run(menu_then_scene, 20000) and not p.on, "a moving menu before the scene must not withhold depth for good"
p = Probe()
assert not p.run(lambda f: (True, JUNK, True), 20000) and not p.on and p.looks == 5, "JUNK keeps its cap"
p = Probe()
assert p.run(lambda f: (True, FLAT, False), 20000) and not p.on and p.looks == 5, "a still menu stops at five"
p = Probe()
assert not p.run(lambda f: (True, FLAT, True), 100000) and p.on and p.looks <= 5 + 3 + 100000 // 4800, \
    "depth FLAT for good (a 32-bit Tomb Raider) is looked at at most every 4800 presents"

root = Path(__file__).resolve().parent.parent
src = {rel: (root / rel).read_text(encoding="utf-8-sig") for rel in (
    "core/addon/neural.cpp", "core/addon/probes.inc", "core/x86bridge/host64.cpp",
    "core/x86bridge/panel32.cpp", "core/ui/view_logic.cpp")}
neural, probes = src["core/addon/neural.cpp"], src["core/addon/probes.inc"]
fails = []


def need(ok, what):
    if not ok:
        fails.append(what)


need("At<uint8_t>(r, rt::kUseDepth) = haveDepth && g.depthUsable.load() ? 1 : 0;" in neural,
     "neural.cpp: kUseDepth does not follow the probe's verdict")
need("depthUsable" not in neural[neural.index("bool haveDepth = false;"):neural.index("haveDepth = true;")],
     "neural.cpp: the gate is on the depth dispatch, so netDepth stops and the gate can never open")
need("g.pendingGuides = true; g.probedDepth = haveDepth;" in neural,
     "neural.cpp: the probe does not record whether the frame it copied fed depth")
need(re.search(r"!g\.depthUsable\.load\(\)\s*\? GuideSource::Unusable", neural),
     "neural.cpp: the 64-bit panel does not say the depth is withheld")
drain = ("if (g.probedDepth && depthJunk)\n"
         "SetDepthUsable(false, \"JUNK\");\n"
         "else if (g.probedDepth && !depthReal && zero < n && ++g.flatProbes >= 2)\n"
         "SetDepthUsable(false, \"FLAT in two readings of a moving scene\");\n"
         "else if (g.probedDepth && depthReal)\n"
         "SetDepthUsable(true, \"it varies\");")
need(drain in re.sub(r"\n\s+", "\n", probes), "probes.inc: the drain's rule is not the one modelled here")
need("else if (++g.junkProbes >= 5 && g.flatProbes < 2)" in probes,
     "probes.inc: five readings end the probe even while FLAT withholds depth, so it never comes back")
need("g.nextGuideProbe = g.status.frame + (600ull << std::min(g.junkProbes - 4, 3u));" in probes,
     "probes.inc: the probe past five readings under FLAT is not the back-off modelled here")
need("depth fed %d (handed %d)" in neural, "neural.cpp: the probe's arm line does not say whether depth is handed")
setter = re.search(r"void SetDepthUsable\(bool usable, const char \*why\)\s*\{(.*?)\n\}", probes, re.S)
need(setter and re.search(r"if \(usable\)\s*g\.flatProbes = 0;\s*if \(g\.depthUsable\.exchange\(usable\) == "
                          r"usable\)\s*return;\s*Log\(.*ResetTemporal\(", setter.group(1), re.S),
     "probes.inc: SetDepthUsable does not reset the FLAT count, log a change or drop the history")
rearm = re.search(r"void RearmGuideProbe\(bool replaced\)\s*\{(.*?)\n\}", probes, re.S)
need(rearm and "SetDepthUsable(true," in rearm.group(1), "probes.inc: a re-arm does not open the gate")
need("s.depthActive=!transport&&built&&g.gameDepthActive?(g.depthUsable.load()?1u:2u):0u;"
     in src["core/x86bridge/host64.cpp"], "host64.cpp: the 32-bit panel is not told the depth is withheld")
need("s.depthSource=w.depthActive==2?ui::GuideSource::Unusable:" in src["core/x86bridge/panel32.cpp"],
     "panel32.cpp: depthActive 2 is not shown as unusable")
need("case GuideSource::Unusable:" in src["core/ui/view_logic.cpp"], "view_logic.cpp: Unusable has no name")

if fails:
    print("FAIL\n  " + "\n  ".join(fails))
    sys.exit(1)
print("PASS the depth gate: JUNK, FLAT twice in a moving scene, varies, re-arm, not-fed readings, "
      "a moving menu before the scene, the probe's stop and back-off, kUseDepth on every route, and "
      "both panels")
