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
  - a frame shows its composition only when CompositionIsFresh(runNetwork) says so, with no term of
    the route's own (host64 included), and otherwise the game's own frame, never an older result;
  - on D3D11 nothing is tallied, and no streak kept, while no present will settle it (NoBridge and
    Stage included), an MSAA back buffer goes out raw with the tallies cleared, and crossLocal is
    made in the format it is read as (host64 too);
  - every route (host64 too) sizes the network raster by EffectiveScale(), the Scale under the cap
    NoteJobCost puts on after evaluations long enough to risk a display-driver reset;
  - RecordNetwork asks HeapStillRead before it writes a descriptor, keyed on the last list that read
    the heap and not on g.completion, which a skipped frame moves on;
  - every present checks DeviceLost before a route runs (host64 before JobGate); each route
    compares what it latched (the D3D12 device and queue, the D3D11 and Vulkan devices) and lets a
    stranger's present go raw; D3D11 checks the game's device for removal; the D3D12 pre-clear copy
    is taken on our device only; destroy_device is subscribed unconditionally and those three routes
    stand down on it;
  - one swapchain drives the pipeline (PrimaryRoute; frontend32's g.active): another window's present
    goes out raw before the depth settle, the alt-tab and minimised tests and the history, its init
    or teardown leaves the primary's gate, size and resources alone, and its effect runtime is not
    taken; a resize keeps the latch, and the init of a primary a destroy let go (a Vulkan rebuild, an
    OpenGL restore) takes it back, dropping what was built for a window taken meanwhile;
  - the panel's capability bits (Feed.fx, Read from the game, depth) are the primary route's own
    Caps(), never a constant, and PrimaryRoute logs what each route reaches when it takes over.

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
if len(re.findall(r"WaitForWorkQueue\(", present)) != len(re.findall(r"if \(!r\.semaphores && !WaitForWorkQueue\(", present)):
    bad.append("OpenGL Present: the queue is waited for on the CPU only without the fences")
if not 0 <= ensure.find("WaitForWorkQueue(g.completion)") < ensure.find("BuildCrossing(r, r.in"):
    bad.append("OpenGL Ensure: the crossing is rebuilt without draining our queue first")
if not all(s in ensure for s in ("r.resolveFbo = ", "r.semaphoresProven = ")):
    bad.append("OpenGL Ensure: a new context keeps the old one's resolve names or fences")
# One rule for a frame the network did not answer, on every route: the composition only when
# CompositionIsFresh says it is this frame's, otherwise the game's own frame. No route adds a term of
# its own (host64 once also asked for runNetwork, which dropped the grade), and OpenGL does not
# repeat its last result. Read per statement, comments and strings out, so a term on a continuation
# line or on the `fresh`/`show` a write-back site tests is caught too.
for f in [ROOT / "core" / p for p in ("transport/d3d12/D3D12Transport.inc", "transport/d3d11/D3D11Transport.inc",
                                      "transport/vulkan/vk_route.inc", "transport/opengl/gl_frame.inc", "x86bridge/host64.cpp")]:
    src = re.sub(r'"[^"\n]*"|//[^\n]*', "", f.read_text(encoding="utf-8"))
    gates = [s.replace("CompositionIsFresh(runNetwork)", "") for s in re.split(r"[;{}]", src)
             if re.search(r"CompositionIsFresh\(|\b(fresh|show)\b", s)]
    if "CompositionIsFresh(runNetwork)" not in src or any(w in s for s in gates for w in ("runNetwork", "activePasses", "CompositionIsFresh(")):
        bad.append(f"{f.relative_to(ROOT)}: a skipped frame is shown by a rule other than CompositionIsFresh(runNetwork) alone")
if "const bool show = fresh && !g.noBackBuffer.load();" not in present or "haveResult" in gl:
    bad.append("OpenGL Present: a frame that is not fresh repeats the last result instead of the host's own")
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
host = (ROOT / "core/x86bridge/host64.cpp").read_text(encoding="utf-8")
if "CreateTexture(w,h,ColourReadFormat(fmt),g.bridge.crossLocal" not in host:
    bad.append("host64 Neural: crossLocal must be made in the format it is read as")

record = body(neural, "RecordNetwork")
heap = body(runtimes, "HeapStillRead")
if not 0 <= record.find("HeapStillRead(runNetwork)") < record.find("CreateShaderResourceView("):
    bad.append("RecordNetwork writes a descriptor before HeapStillRead says no list still reads the heap")
if not all(s in heap for s in ("reader = g.completion;", "GetCompletedValue() >= reader", "DropStaleHistory(false)")):
    bad.append("HeapStillRead: keyed on the last list that read the heap, dropping stale history on a skip")
present = body(neural, "OnPresent")
if "DeviceLost()" not in present[:present.find("transport->Present(")]:
    bad.append("OnPresent: a removed device is not checked before the route runs")
# One swapchain drives the pipeline; another window's present leaves before anything shared, and its
# arrival or teardown leaves the primary's gate, size and resources alone. The 32-bit frontend too.
shared = ("SettleD3D12Depth()", "ToggleRequested()", "disableOnAltTab", "IsIconic(", "ResetTemporal(")
if not 0 <= present.find("PrimaryRoute(dev, queue, sc)") < min(present.find(s) for s in shared):
    bad.append("OnPresent: another swapchain's present reaches shared state before PrimaryRoute")
primary = body(FACTORY.read_text(encoding="utf-8"), "PrimaryRoute")
if not all(s in primary for s in ("g.primarySwapchain.store(sc)", "sc == g.primarySwapchain.load()")):
    bad.append("PrimaryRoute: the first swapchain a route gets is not latched and compared")
life = (ROOT / "core/addon/lifecycle.inc").read_text(encoding="utf-8")
gone, init = body(life, "OnDestroySwapchain"), body(life, "OnInitSwapchain")
if not (0 <= gone.find("OtherSwapchain(sc)") < gone.find("g.goneSwapchain.store(sc)") < gone.find("DrainAndRelease()")
        and "!resize && g.primarySwapchain.load() == sc" in gone
        and 0 <= init.rfind("OtherSwapchain(sc)") < init.find("g.outWidth = g.outHeight = 0")):
    bad.append("lifecycle.inc: another swapchain's teardown or init touches the primary's, or a resize lets it go")
retake = init[init.find("if (sc == g.releasedPrimary)"):init.rfind("OtherSwapchain(sc)")]
if not ("g.releasedPrimary = sc;" in gone[gone.find("!resize && g.primarySwapchain"):gone.find("Log(")]
        and all(s in retake for s in ("DrainAndRelease()", "g.status.loggedOtherSwapchain = false", "g.primarySwapchain.store(sc)"))
        and "ReleaseSwapchainSized()" in body(life, "DrainAndRelease")):
    bad.append("lifecycle.inc: the primary's init after its destroy (Vulkan rebuild, GL restore) does not take the latch back")
adopt = body(life, "OnInitEffects")
if not 0 <= adopt.find("primary->get_hwnd() != runtime->get_hwnd()") < adopt.find("g.effects = runtime"):
    bad.append("OnInitEffects: another window's effect runtime is taken for the primary's frame")
f32 = body((ROOT / "core/x86bridge/frontend32.cpp").read_text(encoding="utf-8"), "OnPresent")
shared = ("probe.Present(", "ClearFrameTallies", "g.disableAltTab", "IsIconic(")
if not 0 <= f32.find("else if(g.active!=sc)") < min(f32.find(s) for s in shared):
    bad.append("frontend32 OnPresent: another swapchain's present reaches shared state before it leaves")
start = host.find("Result Neural()")
if not start < host.find("DeviceLost()", start) < host.find("JobGate()", start):
    bad.append("host64 Neural: a removed device is not checked before JobGate")
latched = (("D3D12", D3D12, ("Foreign(g.device.Get() != device12 || g.queue.Get() != queue12",)),
           ("D3D11", d3d11 / "D3D11Transport.inc",
            ("gameDevice = native;", "!= gameDevice", "game11->GetDeviceRemovedReason()")),
           ("Vulkan", ROOT / "core/transport/vulkan/VulkanTransport.inc",
            ("native != g_route.device", "g_route.device = nullptr;")))
for name, path, marks in latched:
    text = path.read_text(encoding="utf-8")
    if not all(s in text for s in marks + ("void OnDestroyDevice(device* dev) override", "DeviceGone(")):
        bad.append(f"{name}: does not compare what it latched on each present and stand down on destroy_device")
if "g.device.Get() != reinterpret_cast<ID3D12Device*>(dev->get_native())" not in snapshot:
    bad.append("SnapshotBeforeClear: a clear on another device would be copied across devices")
lines = neural.splitlines()
at = [i for i, line in enumerate(lines) if "addon_event::destroy_device>(OnDestroyDevice)" in line]
if len(at) != 1 or lines[at[0] - 1].strip().startswith("if (g.events"):
    bad.append("neural.cpp: destroy_device is registered once and unconditionally")
if "RouteCaps(st);" not in body(neural, "ReadPanelStatus") or re.search(r"\bst\.has\w+ = (true|false)", neural):
    bad.append("ReadPanelStatus: a capability bit is a constant, not the primary route's Caps()")
if "RouteReach(*transport)" not in body(FACTORY.read_text(encoding="utf-8"), "PrimaryRoute"):
    bad.append("PrimaryRoute: a route taking over does not say what it reaches")

if bad:
    print("FAIL")
    print("\n".join(bad))
    sys.exit(1)
print(f"PASS only the transport factory reads device_api; D3D12 states and lifetimes, and every route's device and swapchain identity, hold ({len(files)} files)")
