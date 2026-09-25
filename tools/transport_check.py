"""Only the transport factory may ask which graphics API it is on, and the hand-offs between the
routes and the runtime that no offline run can see.

Every per-API decision the add-on makes goes through the port in core/transport/FrameTransport.hpp; a
`device_api::` anywhere else is the add-on branching on the API again, which is the coupling the
port was built to remove.

The rest is read from the text, because framecheck never copies `composed` to a back buffer and
never submits through ReShade, so its InfoQueue sees none of it:
  - on every route a list is executed before the runtime is told about it, the order the runtime's
    own detour keeps (tools/runtime-patches.json, 0x8873), except a list whose Close failed, which
    RetireUnsubmitted notifies unexecuted so its job still retires;
  - on every route a copy out of `composed` follows its barrier into COPY_SOURCE;
  - on D3D12 a failed RecordNetwork still runs the tail (back buffer to present, completion Signal),
    except the `cmd_list == nullptr` exit, which a graphics queue never takes;
  - a readback is mapped only once its fence has landed, and parked, not freed, when it has not;
  - the live D3D12 depth buffer is never read (it measured zeros), and the pre-clear snapshot is
    rebuilt on a format change and parked rather than released under an in-flight list;
  - a fence event is registered only by WaitFence, which reads the value again after every wake,
    and WaitForPreviousJob, whose event is its own: a timed wait on a shared auto-reset event
    leaves a registration behind that ends the next wait on it early;
  - on OpenGL the CPU sees GL's first signal land before the queue waits on it, the queue waits
    once, before any list runs, and a rebuild drains our queue and forgets the old context's names;
  - on D3D11 nothing is tallied, and no streak kept, while no present will settle it (NoBridge and
    Stage included), an MSAA back buffer goes out raw with the tallies cleared, and crossLocal is
    made in the format it is read as (host64 too);
  - every route (host64 too) sizes the network raster by EffectiveScale(), the Scale under the cap
    NoteJobCost puts on after evaluations long enough to risk a display-driver reset.

    python tools/transport_check.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FACTORY = ROOT / "core/transport/Transports.inc"
# Everything neural.cpp's unity build pulls in, plus framecheck and the 64-bit host.
SCOPE = [ROOT / "core" / d for d in ("addon", "transport", "temporal", "shared", "ui", "diagnostics/framecheck", "x86bridge/host64.cpp")]
NEURAL = ROOT / "core/addon/neural.cpp"
D3D12 = ROOT / "core/transport/d3d12/D3D12Transport.inc"
GUIDES = ROOT / "core/transport/d3d12/D3D12Guides.inc"
RUNTIMES = ROOT / "core/addon/runtimes.inc"
PROBES = ROOT / "core/addon/probes.inc"
MOTION = ROOT / "core/temporal/motion_sources.inc"  # included inside RecordNetwork


def body(text, name):
    """A function's definition, not its declaration, to the closing brace at column 0."""
    start = re.search(rf"\n[^\n;]*\b{name}\([^;{{]*\)\s*{{", text).start()
    return text[start:text.index("\n}\n", start)]


def lines_of(text, name):
    b = body(text, name)
    first = text[:text.index(b)].count("\n") + 1
    return range(first, first + b.count("\n") + 1)


runtimes = RUNTIMES.read_text(encoding="utf-8")
waits = [lines_of(runtimes, n) for n in ("WaitFence", "WaitForPreviousJob")]
bad = []
files = []
sized = 0
for base in SCOPE:
    files += [base] if base.is_file() else sorted(p for p in base.rglob("*") if p.suffix in (".cpp", ".h", ".inc"))
for f in files:
    lines = f.read_text(encoding="utf-8").splitlines()
    where = lambda n: f"{f.relative_to(ROOT)}:{n}: {lines[n - 1].strip()}"
    for n, line in enumerate(lines, 1):
        before = "\n".join(lines[max(0, n - 5):n - 1])
        if "device_api::" in line and f != FACTORY:
            bad.append("device_api read outside the transport factory: " + where(n))
        # RetireUnsubmitted is the one notify with no execute: its list failed to Close and never runs.
        if "NotifyRuntimes(" in line and "void NotifyRuntimes(" not in line and not any(
                s in "\n".join(lines[max(0, n - 3):n - 1])
                for s in ("ExecuteCommandLists(", "flush_immediate_command_list()", "void RetireUnsubmitted(")):
            bad.append("runtime notified before its list was executed: " + where(n))
        if "CopyResource(" in line and "g.composed.Get())" in line and not (
                "g.composed.Get(), D3D12_RESOURCE_STATE_" in before and "D3D12_RESOURCE_STATE_COPY_SOURCE)" in before):
            bad.append("copy out of composed without its barrier into COPY_SOURCE: " + where(n))
        if "SetEventOnCompletion(" in line and "diagnostics" not in f.parts and not (
                f == RUNTIMES and any(n in w for w in waits)):
            bad.append("fence event registered outside WaitFence and WaitForPreviousJob: " + where(n))
        if "EnsureResources(" in line and ("transport" in f.parts or f.name == "host64.cpp"):
            sized += 1
            if "EffectiveScale()" not in line:
                bad.append("network raster sized from the raw Scale, past the TDR cap: " + where(n))
if sized < 5:
    bad.append(f"{sized} EnsureResources calls on the routes, fewer than the five routes")

route = D3D12.read_text(encoding="utf-8").splitlines()
first = next(n for n, line in enumerate(route) if "RecordNetwork(" in line)
last = max(n for n, line in enumerate(route) if "flush_immediate_command_list()" in line)
for n in range(first, last):
    if "return;" in route[n] and "cmd_list == nullptr" not in route[n - 1] + route[n]:
        bad.append(f"D3D12 route returns between RecordNetwork and its tail at line {n + 1}: {route[n].strip()}")

neural = NEURAL.read_text(encoding="utf-8")
drain = body(PROBES.read_text(encoding="utf-8"), "DrainReadbacks")
fences, mapped, parked = (drain.count(s) for s in ("Signal(f.Get(), 1)", "GetCompletedValue() >= 1 &&", "g.parked.insert("))
if not fences or not fences == mapped == parked:
    bad.append(f"DrainReadbacks: {fences} fences, {mapped} maps behind them, {parked} parks; each readback needs all three")
if "depthBest" in body(neural, "RecordNetwork") + MOTION.read_text(encoding="utf-8"):
    bad.append("RecordNetwork reads the live D3D12 depth buffer, which measured zeros")
snapshot = body(GUIDES.read_text(encoding="utf-8"), "SnapshotBeforeClear")
if "Format != DepthAliasFormat(" not in snapshot or "depthSnapshot.Reset()" in snapshot:
    bad.append("SnapshotBeforeClear: the snapshot must be rebuilt on a format change and parked, not reset")

gl = "\n".join(p.read_text(encoding="utf-8") for p in sorted((ROOT / "core/transport/opengl").glob("*.inc")))
present, ensure = body(gl, "Present"), body(gl, "Ensure")
steps = ("BlitIn(r)", "backFence->GetCompletedValue()", "workQueue->Wait(", "RecordNetwork(", "ExecuteCommandLists(")
at = [present.find(s) for s in steps]
if -1 in at or at != sorted(at) or gl.count("workQueue->Wait(") != 1:
    bad.append("OpenGL Present: " + " < ".join(steps) + ", with exactly one queue Wait")
if len(re.findall(r"WaitForWorkQueue\(", present)) != len(re.findall(r"if \(!r\.semaphores\)\s*WaitForWorkQueue\(", present)):
    bad.append("OpenGL Present: the queue is waited for on the CPU only without the fences")
if not 0 <= ensure.find("WaitForWorkQueue(g.completion)") < ensure.find("BuildCrossing(r, r.in"):
    bad.append("OpenGL Ensure: the crossing is rebuilt without draining our queue first")
if not all(s in ensure for s in ("r.resolveFbo = ", "r.semaphoresProven = ", "r.haveResult = false")):
    bad.append("OpenGL Ensure: a new context keeps the old one's resolve names, fences or result")
if body(gl, "ImportFences").count("std::max(r.to") != 2:
    bad.append("OpenGL ImportFences: a new context's fence values may step below the old one's")

d3d11 = ROOT / "core/transport/d3d11"
bridge = body((d3d11 / "D3D11Transport.inc").read_text(encoding="utf-8"), "BridgePresent")
observe = body((d3d11 / "D3D11Guides.inc").read_text(encoding="utf-8"), "ObserveD3D11")
gate = observe[:max(0, observe.find("d3d11guides::ObserveD3D11("))]
if not all(s in gate for s in ("g.settings.enabled", "g.noBridge", "g_depthTally.clear()", "coldFrames = 0")):
    bad.append("D3D11 ObserveD3D11: targets are tallied, or a cold start kept, while no present will settle them")
early = bridge[:bridge.find("EnsureResources(")]
msaa = early[early.find("samples > 1"):] if "samples > 1" in early else ""
if not all(s in msaa for s in ("g_depthTally.clear()", "return;")):
    bad.append("D3D11 BridgePresent: an MSAA back buffer must go out raw, tallies cleared, before anything is built")
if not all(s in bridge for s in ("crossFmt = ColourReadFormat(fmt)", "Format != crossFmt", "CreateTexture(w, h, crossFmt,")):
    bad.append("D3D11 BridgePresent: crossLocal must be made, and remade, in the format it is read as")
if "CreateTexture(w,h,ColourReadFormat(fmt),g.bridge.crossLocal" not in (ROOT / "core/x86bridge/host64.cpp").read_text(encoding="utf-8"):
    bad.append("host64 Neural: crossLocal must be made in the format it is read as")

if bad:
    print("FAIL")
    print("\n".join(bad))
    sys.exit(1)
print(f"PASS only the transport factory reads device_api; D3D12 states and lifetimes hold ({len(files)} files)")
