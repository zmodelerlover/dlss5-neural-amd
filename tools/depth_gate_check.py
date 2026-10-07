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

Phase 2, where the depth comes from. On D3D11 (64-bit, and the 32-bit bridge through the same
d3d11_guides.h) the guide was copied at present only, and a game that clears depth before Present
(a 32-bit Tomb Raider) leaves a constant plane there. Once the probe withholds that copy
(FallBackToPreClear), it is taken just before the game's clears instead (SnapshotDepthBeforeClear),
and the probe looks again. Only then, and the first time on trial: an engine that clears at the
start of a frame gets the previous frame's depth before a clear, where the copy at present is this
frame's, and a menu that moves is withheld just the same before the scene arrives in that buffer.
So the first reading that varies before a clear (depthVaried; on the 32-bit bridge WireStatus
depthActive 3) goes back to the copy at present once, and the probe looks again; withheld there a
second time, the copy before the clears stays until the guide takes another buffer. A copy before a
clear that reads no better is withheld, not swapped back. The D3D12 pre-clear snapshot records
which buffer it is of and at which present: a new pick, or the pick let go at a teardown, drops it,
and RecordNetwork hands it over (and the panel names it) only while it is still of the pick and at
most two presents old.

    python tools/depth_gate_check.py
"""
import re
import sys
from pathlib import Path


class Gate:
    def __init__(self):
        self.usable, self.flat, self.varied = True, 0, False

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
        self.varied = fed and real  # depthVaried, FallBackToPreClear's cue; a re-arm keeps it
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

    def __init__(self):
        super().__init__()
        self.on, self.junk, self.nxt, self.looks = True, 0, 120, 0

    def arm(self, frame):  # RearmGuideProbe
        self.on, self.junk, self.nxt = True, 0, frame + 120
        return self.rearm()

    def due(self, frame):
        return self.on and frame >= self.nxt

    def look(self, frame, fed, kind, moving):
        self.nxt, self.looks = frame + 600, self.looks + 1
        self.reading(fed, moving=moving, **kind)
        if kind.get("real") and moving:
            self.on = False
        else:
            self.junk += 1
            if self.junk >= 5 and self.flat < 2:
                self.on = False
            elif self.junk >= 5:
                self.nxt = frame + (600 << min(self.junk - 4, 3))

    def run(self, scene, presents):
        for frame in range(presents):
            if self.due(frame):
                self.look(frame, *scene(frame))
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


class Copy:
    """d3d11_guides.h: what the network is fed of one depth buffer, copied at present or, once
    switched, just before the game's last clear. Content is a label; "flat" is a cleared buffer."""

    def __init__(self, buf="A"):
        self.chosen, self.pre, self.retried, self.fresh, self.snap = buf, False, False, False, None

    def take(self, buf):  # SettleGuide
        self.chosen, self.pre, self.retried, self.fresh = buf, False, False, False

    def fall_back(self, withheld, varied=False):  # FallBackToPreClear
        if self.chosen is None:
            return False
        if self.pre and varied and not self.retried:
            self.pre = self.fresh = False
            self.retried = True
            return True
        if not withheld or self.pre:
            return False
        self.pre = True
        return True

    def clear(self, buf, content):  # SnapshotDepthBeforeClear, before the clear runs
        if self.pre and buf == self.chosen:
            self.snap, self.fresh = content, True

    def present(self, content):  # PrepareGuide
        if not self.fresh:
            self.snap = content
        self.fresh = False
        return self.snap


def clears_before_present(c, n):
    """Draws frame n's scene, then clears before Present: the buffer at present is flat."""
    c.clear("A", f"scene{n}")
    return c.present("flat")


def clears_at_start(c, n):
    """Clears first, over the last frame's scene, then draws this one: real at present."""
    c.clear("A", f"scene{n - 1}")
    return c.present(f"scene{n}")


c = Copy()
assert clears_before_present(c, 1) == "flat", "copied at present until the probe withholds it"
assert c.fall_back(True) and clears_before_present(c, 2) == "scene2", "then this frame's, before its clear"
assert not c.fall_back(True) and c.pre, "a copy before a clear that reads no better is not swapped back"
assert c.present("flat") == "flat", "a present with no clear of the buffer since is copied at present"
assert c.fall_back(False, varied=True) and not c.pre, "one that varies goes back to the copy at present"
assert clears_before_present(c, 3) == "flat" and not c.fall_back(False, varied=True), "only from before a clear"
assert c.fall_back(True) and c.pre and not c.fall_back(False, varied=True) and c.pre, \
    "withheld at present a second time: copied before the clears until another buffer is taken"
c.take("B")
assert not c.pre and not c.retried and c.present("real") == "real", "a new buffer is copied at present again"
c = Copy()
assert clears_at_start(c, 5) == "scene5" and not c.fall_back(False), "a real copy at present is never switched"
c.pre = True
assert clears_at_start(c, 6) == "scene5", "why only as a fallback: before a clear it is a frame old"
c = Copy()
c.fall_back(True)
c.clear("shadow", "shadow0")
assert c.present("flat") == "flat", "another buffer's clear is not this guide's copy"



def session(clears_at_start, menu_until, presents=20000, copy=Copy):
    """One depth buffer at 60 fps: a menu that moves over FLAT depth until menu_until, then the
    scene. The switch is made where BridgePresent makes it, after the frame's clears and before
    PrepareGuide, and each switch re-arms the probe. Returns the copy, the probe, the number of
    switches, and how many of the last 10000 presents were handed another frame's depth."""
    p, c, switches, stale = Probe(), copy(), 0, 0
    drawn = lambda k: "flat" if k < menu_until else f"scene{k}"
    for n in range(1, presents):
        c.clear("A", drawn(n - 1) if clears_at_start else drawn(n))
        if c.fall_back(not p.usable, p.varied):
            switches += 1
            p.arm(n)
        got = c.present(drawn(n) if clears_at_start else "flat")
        stale += n >= presents - 10000 and got != drawn(n)
        if p.due(n):
            p.look(n, True, FLAT if got == "flat" else VARIES, True)
    return c, p, switches, stale


class Latched(Copy):
    """The switch as it first landed: kept from the first withheld copy on."""

    def fall_back(self, withheld, varied=False):
        return Copy.fall_back(self, withheld)


AT_START, BEFORE_PRESENT = True, False  # an engine that clears at frame start; Tomb Raider
c, p, switches, stale = session(AT_START, 0)
assert not c.pre and p.usable and switches == 0 and stale == 0, "depth real at present is never switched"
c, p, switches, stale = session(AT_START, 3000)
assert not c.pre and p.usable and switches == 2 and stale == 0, \
    "a moving menu before the scene: back to this frame's depth once the copy before a clear varies"
assert session(AT_START, 3000, copy=Latched)[3] == 10000, "latched, the scene got the last frame's for good"
for menu_until in (0, 3000):
    c, p, switches, stale = session(BEFORE_PRESENT, menu_until)
    assert c.pre and p.usable and switches == 3 and stale == 0, \
        f"Tomb Raider pays one more probe cycle and then keeps the copy before the clears ({menu_until})"
c, p, switches, stale = session(BEFORE_PRESENT, 10 ** 9)
assert c.pre and not p.usable and switches == 1, "a copy before a clear that reads no better stays withheld"


class Snapshot:
    """The D3D12 pre-clear snapshot's provenance. `of` is set only by a copy of the pick and
    dropped wherever the pick changes, so RecordNetwork needs no look at the pick itself."""

    def __init__(self):
        self.best, self.of, self.at, self.frame = None, None, 0, 0

    def pick(self, buf):  # SettleD3D12Depth's take, or ReleaseSwapchainSized with None
        self.best, self.of = buf, None

    def clear(self, buf):  # SnapshotBeforeClear copies only the pick
        if buf is not None and buf == self.best:
            self.of, self.at = buf, self.frame

    def fresh(self):  # RecordNetwork
        return self.of is not None and self.frame - self.at <= 2


d = Snapshot()
assert not d.fresh(), "nothing copied yet: no depth"
d.pick("A")
d.clear("A")
d.frame = 2
assert d.fresh(), "two presents old is still the frame's"
d.frame = 3
assert not d.fresh(), "a game that stopped clearing: no depth, not a frozen frame"
d.clear("A")
d.pick("B")
assert not d.fresh(), "a snapshot of the buffer before a new pick is no depth"
d.clear("A")
assert not d.fresh(), "a clear of a buffer that is not the pick is not copied"
d.clear("B")
d.pick(None)
d.pick("B")  # the same address back after a teardown, within two presents
assert not d.fresh(), "a snapshot from before a teardown is not the new buffer's"

root = Path(__file__).resolve().parent.parent
src = {rel: (root / rel).read_text(encoding="utf-8-sig") for rel in (
    "core/addon/neural.cpp", "core/addon/probes.inc", "core/x86bridge/host64.cpp",
    "core/x86bridge/panel32.cpp", "core/ui/view_logic.cpp", "core/shared/d3d11_guides.h",
    "core/shared/guide_choice.h", "core/transport/d3d11/D3D11Transport.inc",
    "core/x86bridge/frontend32.cpp", "core/transport/d3d12/D3D12Guides.inc", "core/addon/panel64.inc",
    "core/transport/d3d12/D3D12Transport.inc")}
neural, probes, panel = src["core/addon/neural.cpp"], src["core/addon/probes.inc"], src["core/addon/panel64.inc"]
fails = []


def need(ok, what):
    if not ok:
        fails.append(what)


need("At<uint8_t>(r, rt::B->kUseDepth) = haveDepth && g.depthUsable.load() ? 1 : 0;" in neural,
     "neural.cpp: kUseDepth does not follow the probe's verdict")
need("depthUsable" not in neural[neural.index("bool haveDepth = false;"):neural.index("haveDepth = true;")],
     "neural.cpp: the gate is on the depth dispatch, so netDepth stops and the gate can never open")
need("g.pendingGuides = true; g.probedDepth = haveDepth;" in neural,
     "neural.cpp: the probe does not record whether the frame it copied fed depth")
need(re.search(r"!g\.depthUsable\.load\(\)\s*\? GuideSource::Unusable", panel),
     "panel64.inc: the 64-bit panel does not say the depth is withheld")
drain = ("if (g.probedDepth && depthJunk)\n"
         "SetDepthUsable(false, \"JUNK\");\n"
         "else if (g.probedDepth && !depthReal && zero < n && ++g.flatProbes >= 2)\n"
         "SetDepthUsable(false, \"FLAT in two readings of a moving scene\");\n"
         "else if (g.probedDepth && depthReal)\n"
         "SetDepthUsable(true, \"it varies\");\n"
         "g.depthVaried.store(g.probedDepth && depthReal);")
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
need("s.depthActive=!transport&&built&&g.gameDepthActive?(!g.depthUsable.load()?2u:g.depthVaried.load()?3u:1u):0u;"
     in src["core/x86bridge/host64.cpp"], "host64.cpp: the 32-bit frontend is not told the depth is withheld, or that it varied")
need("s.depthSource=w.depthActive==2?ui::GuideSource::Unusable:" in src["core/x86bridge/panel32.cpp"],
     "panel32.cpp: depthActive 2 is not shown as unusable")
need("case GuideSource::Unusable:" in src["core/ui/view_logic.cpp"], "view_logic.cpp: Unusable has no name")

flat = {rel: re.sub(r"\s+", " ", text) for rel, text in src.items()}
guides, front = flat["core/shared/d3d11_guides.h"], src["core/x86bridge/frontend32.cpp"]
d3d11 = flat["core/transport/d3d11/D3D11Transport.inc"]
need("const bool beforeClear = guide.snapFresh; guide.snapFresh = false; if (!beforeClear) "
     "ctx->CopyResource(guide.snap.Get(), guide.chosen.Get());" in guides,
     "d3d11_guides.h: PrepareGuide copies at present over the copy taken before the clear")
need("if (guide.external || guide.chosen == nullptr) return false; if (guide.preClear && varied && "
     "!guide.presentRetried) { guide.preClear = guide.snapFresh = false; guide.presentRetried = true;" in guides
     and "if (!withheld || guide.preClear) return false; guide.preClear = true;" in guides,
     "d3d11_guides.h: FallBackToPreClear is not the rule modelled here")
need(re.search(r"void SnapshotDepthBeforeClear\(.*?if \(!guide\.preClear .*?!= guide\.chosen\.Get\(\)\) "
               r"return; reinterpret_cast<ID3D11DeviceContext\*>\(cmd->get_native\(\)\) "
               r"->CopyResource\(guide\.snap\.Get\(\), guide\.chosen\.Get\(\)\); guide\.snapFresh = true;",
               guides), "d3d11_guides.h: the copy before a clear is not of the guide's buffer, on the clearing context")
need("guide.preClear = guide.snapFresh = guide.presentRetried = false;" in flat["core/shared/guide_choice.h"],
     "guide_choice.h: a take keeps the old buffer's switch to the copy before a clear")
need("void OnDepthCleared(command_list* cmd, device*, resource_view dsv) override {" in d3d11
     and "d3d11guides::SnapshotDepthBeforeClear(cmd, dsv, gameDevice, g.guideDepth);" in d3d11,
     "D3D11Transport.inc: the 64-bit route takes no copy before a clear")
need("if (d3d11guides::FallBackToPreClear(g.guideDepth, !g.depthUsable.load(), g.depthVaried.load(), Log)) "
     "RearmGuideProbe(false);" in d3d11,
     "D3D11Transport.inc: withheld or varied depth does not switch the copy and re-probe")
need("!g.status.windowHidden && !g.bridge.failed && !g.noBridge.load() && g.stage.load() >= 3) "
     "d3d11guides::SnapshotDepthBeforeClear(" in d3d11,
     "D3D11Transport.inc: the copy before a clear runs where ObserveD3D11 stands down")
need("reshade::addon_event::clear_depth_stencil_view>(OnClear);" in front
     and "d3d11guides::SnapshotDepthBeforeClear(cmd,dsv,g.game11.Get(),g.guideDepth);" in front,
     "frontend32.cpp: the 32-bit bridge takes no copy before a clear")
state = front[front.index("bool StateRequest("):front.index("\n}\n", front.index("bool StateRequest("))]
need("if(d3d11guides::FallBackToPreClear(g.guideDepth,controls.status.depthActive==2,"
     "controls.status.depthActive==3,Log))g.guideTaken|=1;" in state,
     "frontend32.cpp: the helper's withheld or varied depth does not switch the copy and re-probe")
need("v.preClear=v.snapFresh=v.presentRetried=false;" in front, "frontend32.cpp: ClearGuide keeps the switch")
clear = front[front.index("bool OnClear("):front.index("return false;}", front.index("bool OnClear("))]
need("if(!dev||dev->get_api()!=device_api::d3d11)return false;std::lock_guard lock(g.lock);" in clear
     and "g.enabled&&!g.failed&&!g.hidden&&" in clear,
     "frontend32.cpp: OnClear locks for every API's clears, or copies while nothing reads it")
d12 = flat["core/transport/d3d12/D3D12Guides.inc"]
need(re.search(r"g\.snapshotOf = nullptr; if \(g\.depthSnapshot != nullptr\) g\.parked\.push_back", d12)
     and re.search(r"cmd->CopyResource\(g\.depthSnapshot\.Get\(\), native\);.*g\.snapshotOf = native; "
                   r"g\.snapshotAt = g\.status\.frame;", d12),
     "D3D12Guides.inc: the snapshot does not record which buffer it is of and when")
need("bool SnapshotFresh() const { return snapshotOf != nullptr && status.frame - snapshotAt <= 2; }" in neural
     and "if (g.settings.useDepth.load() && (fromGame || g.SnapshotFresh()))" in neural,
     "neural.cpp: a stale D3D12 snapshot, or one of another buffer, is still handed over")
need(re.search(r"st\.depthSource = !g\.gameDepthActive && !g\.SnapshotFresh\(\)\s+\? GuideSource::None", panel),
     "panel64.inc: the panel names a snapshot as the source that RecordNetwork does not hand over")
d12src = src["core/transport/d3d12/D3D12Guides.inc"]
settle = d12src[d12src.index("void SettleD3D12Depth()"):]
settle = settle[:settle.index("\n}\n")]
need(settle.index("g.depthBest = best->res;") < settle.index("g.snapshotOf = nullptr;"),
     "D3D12Guides.inc: the D3D12 take keeps the old buffer's snapshot as this one's")
picks = neural + d12src + src["core/transport/d3d12/D3D12Transport.inc"]
need(re.search(r"g\.depthBest\.Reset\(\);\s*g\.snapshotOf = nullptr;", picks)
     and picks.count("g.depthBest = ") == 1 and picks.count("g.depthBest.Reset()") == 1,
     "the D3D12 pick changes somewhere that does not drop the snapshot's provenance")

if fails:
    print("FAIL\n  " + "\n  ".join(fails))
    sys.exit(1)
print("PASS the depth gate: JUNK, FLAT twice in a moving scene, varies, re-arm, not-fed readings, "
      "a moving menu before the scene, the probe's stop and back-off, kUseDepth on every route, "
      "both panels, the D3D11 and 32-bit copy before a clear as a fallback only and on trial the "
      "first time (a moving menu, then a scene cleared at frame start), and the D3D12 "
      "snapshot's provenance")
