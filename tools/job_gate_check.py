"""Every route runs the engine same-frame, and nothing decides as if it might not.

In the engine's async mode kJobCounter never moves: its one non-zero store sits behind the worker's
sample of kInlineActive (runtime_offsets.h). A job recorded async therefore read busy until the
old 500 ms reset, ~2 evaluations a second, while history copied the frame's input and the output
smooth blended it; with no reset it would read busy for ever. So async is retired, and these hold
it retired:
  - LoadSettings reads `Inline` only to say it ignores 0; the settings table holds the flag at 1
    (default 1, range 1..1) for the ini, both panels and the bridge's wire, and nothing in neural.cpp
    stores it by hand;
  - no history, smooth or pass-control decision reads the menu flag (core/temporal/, RecordNetwork,
    BringUpEngines);
  - RecordNetwork reads the module's own latched kInlineActive after each record, stands down
    (unavailable, break) when it is not 1, and does so before the job is counted as ours;
  - the 64-bit panel has no Timing: only the 32-bit bridge draws it, as its pipelining switch.

And the run/skip decision is made in one place. Five copies had drifted (Vulkan and OpenGL recorded
on top of a late job and never timed one), so each route (D3D12, D3D11, Vulkan, OpenGL, the 32-bit
bridge's host) sets its runNetwork from JobGate (core/addon/runtimes.inc) exactly once, in code and
not in a comment, and reads neither the job counter, the skip window nor the job cost itself. A
pause (the 64-bit effect off, resumed through the minimised window's restore; the 32-bit host's
reset after one) clears the jobRunning latch, or JobGate would time the whole pause as a network
job, one toward the three that cap the scale; so does a swapchain teardown, ReleaseSwapchainSized,
which the helper's DROP runs too, now that a rebuild raises no reset.

Busy is the runtime's own count, never the add-on's memory of it. A job given up on at 500 ms was
still in the runtime's in-order queue, so recording on top only stacked work behind it. Hence:
  - RuntimeBusy asks Outstanding, which reads kJobId and kJobCounter, and nothing keeps lastJobs;
  - no ResetJobs anywhere, and JobGate has no timer that lets a job the runtime has not retired
    go; only our own queue fence keeps its 500 ms, since a list behind it stacks no runtime job;
  - every Close-failure branch on the four bridge routes calls RetireUnsubmitted, which still tells
    the runtime, so the job it recorded retires instead of reading busy for ever.

And the pass count has one latch, BringUpEngines (runtimes.inc), which every route calls before
JobGate; the 32-bit host's SET_STATE only stores the request. So:
  - a change, up or down, waits, bounded at 2 s, until no module has a job in flight, then starts
    history again; three drains in a row that ran out keep the count until the next change, and
    nothing in it says a restart applies it;
  - a missing copy loads only there, after that drain: LoadExtraRuntime is called once in the core,
    in BringUpEngines, and a copy that fails latches g_passLimit, so it is not tried again and the
    count never exceeds PassesAvailable;
  - no route decides the count itself: each hands WantedPasses to BringUpEngines once and stores
    only its answer in g.loadedPasses, and both panels are told PassesAvailable, which is what their
    note on a pass that could not load compares Passes with.

    python tools/job_gate_check.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
neural = (ROOT / "core/addon/neural.cpp").read_text(encoding="utf-8")
perf = (ROOT / "core/ui/sections/performance.cpp").read_text(encoding="utf-8")


def body(text, signature):
    """The definition of `signature` (not a declaration) up to its closing brace at column 0."""
    m = re.search(rf"^{re.escape(signature)}[^;{{]*\)\s*\n\{{", text, re.M)
    return text[m.start():text.index("\n}\n", m.start())] if m else ""


def code(text):
    """`text` without its comments: a call named in a comment is not a call made."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


bad = []
runtimes = (ROOT / "core/addon/runtimes.inc").read_text(encoding="utf-8")
record, bring, draw = (body(neural, "bool RecordNetwork("), body(runtimes, "bool BringUpEngines("),
                       body(perf, "void DrawPerformance("))
temporal = sorted((ROOT / "core/temporal").glob("*.inc"))
# The control line: a check that found nothing to read passes on nothing.
if not (record and bring and draw and temporal):
    bad.append("could not find RecordNetwork, BringUpEngines, DrawPerformance or core/temporal")

if 'X(uint32_t, inlineMode, "Inline", 1, 1, 1)' not in (ROOT / "core/x86bridge/settings_fields.inc").read_text(encoding="utf-8"):
    bad.append("settings_fields.inc: Inline is not held at 1 (default 1, range 1..1)")
if "inlineMode.store(" in neural:
    bad.append("neural.cpp stores inlineMode; the ini's Inline=0 must be ignored, not taken")
if not re.search(r'if \(!flag\(L"Inline", true\)\) Log\(', neural):
    bad.append("LoadSettings no longer logs an Inline=0 it ignores")
for path in temporal:
    if "inlineMode" in path.read_text(encoding="utf-8"):
        bad.append(f"{path.relative_to(ROOT).as_posix()}: decides on the menu's inlineMode")
for name, text in (("RecordNetwork", record), ("BringUpEngines", bring)):
    if "inlineMode.load()" in text.replace("rt::kInlineMode) = g.settings.inlineMode.load()", ""):
        bad.append(f"{name}: decides on the menu's inlineMode")

latch = record.find("At<uint8_t>(r, rt::kInlineActive) != 1")
if latch < 0 or not record.find("rt::kRecordFn") < latch < record.find("g.lastJob = jobAfter") \
        or latch > record.find("++accepted"):
    bad.append("RecordNetwork: kInlineActive is not tested after the record and before the job counts")
else:
    then = record[latch:record.index("break;", latch)]
    if "g.status.unavailable = true;" not in then or "g.status.reason" not in then:
        bad.append("RecordNetwork: a latched async does not stand down with a reason")

if not re.search(r"if \(status\.helperProcess\)\s*\n\s*Timing\(s\);", draw) or draw.count("Timing(") != 1:
    bad.append("performance.cpp: Timing is drawn outside the 32-bit bridge's helperProcess")

gate = code(body(runtimes, "bool JobGate("))
if not all(s in gate for s in ("WaitForPreviousJob();", "RuntimeBusy()", "NoteJobCost(", "DeviceLost()")):
    bad.append("runtimes.inc: JobGate must wait, test the job, note its cost and check the device")
# Only our own queue fence may time out; the runtime's count stands alone, with no timer on it.
fence = re.search(r"const bool pending = RuntimeBusy\(\) \|\|\s*(\(g\.fence->GetCompletedValue\(\)[^;]*\));",
                  gate)
if not fence or "RuntimeBusy" in fence.group(1) or re.search(r"\b500\b", gate.replace(fence.group(1), "")):
    bad.append("runtimes.inc: JobGate lets a job the runtime has not retired go on a timer")
outstanding = code(body(runtimes, "int Outstanding("))
if "Outstanding(m)" not in code(body(runtimes, "bool RuntimeBusy(")) or not all(
        s in outstanding for s in ("rt::kJobId", "rt::kJobCounter")):
    bad.append("runtimes.inc: RuntimeBusy is not kJobId - kJobCounter read from each module")
for path in sorted((ROOT / "core").rglob("*")):
    if path.suffix in (".cpp", ".h", ".inc"):
        for word in ("lastJobs", "ResetJobs("):
            if word in code(path.read_text(encoding="utf-8", errors="replace")):
                bad.append(f"{path.relative_to(ROOT).as_posix()}: {word} is back")
transport = ROOT / "core/transport"
routes = {"D3D12": transport / "d3d12", "D3D11": transport / "d3d11",
          "Vulkan": transport / "vulkan", "OpenGL": transport / "opengl",
          "x86 host": ROOT / "core/x86bridge/host64.cpp"}
for name, path in routes.items():
    files = [path] if path.is_file() else sorted(path.glob("*.inc"))
    text = code("".join(p.read_text(encoding="utf-8") for p in files))
    sets = len(re.findall(r"const bool runNetwork = (?:!transportOnly && )?JobGate\(\);", text))
    if not files or sets != 1 or text.count("JobGate(") != 1:
        bad.append(f"{name}: sets runNetwork from JobGate() {sets} times and calls it "
                   f"{text.count('JobGate(')} times, not once each")
    for own in ("RuntimeBusy(", "NoteJobCost(", "WaitForPreviousJob(", "lastJobAt"):
        if own in text:
            bad.append(f"{name}: reads {own} itself instead of leaving the decision to JobGate")
    if name != "D3D12":  # the four bridges close their own list; D3D12 records on the game's
        fails = re.findall(r"->Close\(\); FAILED\(hr\)\)\s*\{(.*?)\breturn\b", text, re.S)
        if not fails or any("RetireUnsubmitted(" not in f for f in fails):
            bad.append(f"{name}: a Close failure drops its recorded job without RetireUnsubmitted")
    # The pass count is BringUpEngines' answer, asked for once a present and never second-guessed.
    stores = re.findall(r"g\.loadedPasses\s*=(?!=)\s*([^;]*);", text)
    if (text.count("WantedPasses()") != 1 or text.count("BringUpEngines(wanted)") != 1
            or stores != ["wanted"] or "settings.passes.load()" in text):
        bad.append(f"{name}: decides the pass count itself instead of taking BringUpEngines' answer")
# A pause runs no JobGate, so the latch would time the whole pause as one job; three cap the scale.
host = code((ROOT / "core/x86bridge/host64.cpp").read_text(encoding="utf-8"))
restore = re.search(r"if \(g\.status\.windowHidden\)\s*\{\s*g\.status\.windowHidden = false;([^}]*)\}",
                    code(neural))
if (not re.search(r"\|\| g\.status\.failed(?: \|\| DeviceLost\(\))?\)\s*\{\s*g\.status\.windowHidden = true;\s*return;",
                  code(neural))
        or not restore or "g.jobRunning = false;" not in restore.group(1)
        or not re.search(r"if\(f\.resetHistory\)\{ResetTemporal\(\"[^\"]+\"\);g\.jobRunning=false;\}", host)):
    bad.append("the effect switched off (64-bit) or a reset (32-bit host) keeps jobRunning set")
if "g.jobRunning = false;" not in body(code(neural), "void ReleaseSwapchainSized("):
    bad.append("ReleaseSwapchainSized keeps jobRunning set: the first job after a rebuild times the gap")

latch = code(bring)
# A change drains first; one that ran out keeps the count (and three in a row wait for the next
# change); copies load only past the drain; history starts again once the new count is taken.
drain = latch.find("GetTickCount64() + 2000;")
stuck = re.search(r"if \(RuntimeBusy\(\)\)\s*\{(.*?)\n        \}", latch, re.S)
loads = re.search(r"if \(g\.runtimes\[slot\] == nullptr && !LoadExtraRuntime\(slot\)\)\s*\{([^}]*)\}", latch)
taken = re.search(r"if \(change\)\s*\{[^}]*ResetTemporal\(\"Passes changed\"\);[^}]*\}\s*live = next;", latch)
if not (all(s in latch for s in ("PassesAvailable()", "timeouts >= 3 && !fresh", "timeouts = 0;",
                                 "asked = wanted;", "wanted = live;"))
        and stuck and all(s in stuck.group(1) for s in ("++timeouts", "wanted = live;", "return true;"))
        and loads and all(s in loads.group(1) for s in ("g_passLimit = slot;", "break;"))
        and taken and 0 <= drain < stuck.start() < loads.start() < taken.start()):
    bad.append("runtimes.inc: BringUpEngines does not drain for 2 s, load missing copies after the "
               "drain, latch a failed one and reset history on a Passes change")
if "g_passLimit" not in code(body(runtimes, "UINT PassesAvailable(")):
    bad.append("runtimes.inc: PassesAvailable is not capped by a copy that failed to load")
if "restart" in latch or "restart" in code(body(perf, "void Passes(")):
    bad.append("a Passes change is said to wait for a restart; it applies live")
core_code = "".join(code(p.read_text(encoding="utf-8", errors="replace"))
                    for p in sorted((ROOT / "core").rglob("*")) if p.suffix in (".cpp", ".h", ".inc"))
if len(re.findall(r"(?<!bool )LoadExtraRuntime\(", core_code)) != 1:
    bad.append("a runtime copy loads outside BringUpEngines' drained change")
for path, wiring in (("core/addon/neural.cpp", "st.passesAvailable = PassesAvailable();"),
                     ("core/x86bridge/host64.cpp", "s.loadedPasses=PassesAvailable();"),
                     ("core/x86bridge/panel32.cpp", "s.passesAvailable=w.loadedPasses;"),
                     ("core/ui/sections/performance.cpp", "s.passes) > status.passesAvailable")):
    if wiring not in code((ROOT / path).read_text(encoding="utf-8")):
        bad.append(f"{path}: the panel is not told what the loaded copies can run ({wiring})")

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print("PASS same frame only: Inline=0 ignored, no decision on the menu flag, a latched async stands "
      "down, Timing only on the 32-bit bridge, one JobGate on each of the five routes, busy from the "
      "runtime's own count, no timed let-go of a runtime job, a pause clears the job latch, every "
      "failed Close retires its job, one pass-count latch that drains, loads missing copies live, "
      "latches a failed one and resets history")
