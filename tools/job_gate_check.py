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
  - one copy a call, since the 32-bit helper's frame has 5 s and a drain with two copies can outlast
    it: the latch is compiled here with g++ against stubs and run, and 1 -> 3 goes through 2 over
    two presents (a first bring-up at 3 too), a lower count and a higher one again load nothing, and
    a copy that fails (pass 2's, or pass 3's with pass 2 up) holds the count below it for 50
    presents with no second try, says once what was asked, what runs and which pass did not load,
    and, the count not having moved, takes nothing and keeps the history;
  - no route decides the count itself: each hands WantedPasses to BringUpEngines once and stores
    only its answer in g.loadedPasses, and both panels are told PassesAvailable, which is what their
    note on a pass that could not load compares Passes with.

    python tools/job_gate_check.py

The latch needs a C++20 g++: the one CXX names, else g++ on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile
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
    if "inlineMode.load()" in text.replace("rt::B->kInlineMode) = g.settings.inlineMode.load()", ""):
        bad.append(f"{name}: decides on the menu's inlineMode")

latch = record.find("At<uint8_t>(r, rt::B->kInlineActive) != 1")
if latch < 0 or not record.find("rt::B->kRecordFn") < latch < record.find("g.lastJob = jobAfter") \
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
        s in outstanding for s in ("rt::B->kJobId", "rt::B->kJobCounter")):
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
loads = re.search(r"if \(g\.runtimes\[slot\] == nullptr\)\s*\{\s*if \(!LoadExtraRuntime\(slot\)\)\s*\{([^}]*)\}\s*break;", latch)
taken = re.search(r"if \(change && next != live\)\s*\{[^}]*ResetTemporal\(\"Passes changed\"\);[^}]*\}\s*live = next;", latch)
if not (all(s in latch for s in ("PassesAvailable()", "timeouts >= 3 && !fresh", "timeouts = 0;",
                                 "asked = wanted;", "wanted = live;"))
        and stuck and all(s in stuck.group(1) for s in ("++timeouts", "wanted = live;", "return true;"))
        and loads and "g_passLimit = slot;" in loads.group(1)
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
for path, wiring in (("core/addon/panel64.inc", "st.passesAvailable = PassesAvailable();"),
                     ("core/x86bridge/host64.cpp", "s.loadedPasses=PassesAvailable();"),
                     ("core/x86bridge/panel32.cpp", "s.passesAvailable=w.loadedPasses;"),
                     ("core/ui/sections/performance.cpp", "s.passes) > status.passesAvailable")):
    if wiring not in code((ROOT / path).read_text(encoding="utf-8")):
        bad.append(f"{path}: the panel is not told what the loaded copies can run ({wiring})")

# The latch itself, run: g_passLimit through BringUpEngines, against stubs that record every load.
HARNESS = r"""
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using UINT = unsigned;
using UINT64 = unsigned long long;
using HMODULE = void *;
struct State { static constexpr UINT kMaxPasses = 3; };
struct {
    struct { std::atomic<bool> serialPasses { true }; } settings;
    HMODULE runtime = nullptr, runtimes[State::kMaxPasses] {};
    bool loggedPassDetail = false;
} g;
int modules[State::kMaxPasses], loads = 0, resets = 0;
UINT refuse = 0;  // the slot whose copy does not come up, 0 for none
std::vector<std::string> said;
bool MzSelected() { return false; }
bool MzInit() { return false; }
bool InitPipeline() { return true; }
bool InitEngine() { g.runtime = g.runtimes[0] = &modules[0]; return true; }
bool LoadExtraRuntime(UINT slot) {
    ++loads;
    if (slot == refuse) return false;
    g.runtimes[slot] = &modules[slot];
    return true;
}
bool RuntimeBusy() { return false; }
bool DeviceLost() { return false; }
UINT64 GetTickCount64() { return 0; }
void Sleep(UINT) {}
void ResetTemporal(const char *) { ++resets; }
void Log(const char *fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    said.push_back(line);
}
PASS_LATCH
int fails = 0;
void Expect(bool ok, const char *what) { if (!ok) ++fails, std::printf("FAIL %s\n", what); }
// One present asking for `asked`: the count it runs, with at most one copy loaded on the way.
UINT Present(UINT asked) {
    const int before = loads;
    UINT wanted = asked;
    Expect(BringUpEngines(wanted), "the latch refused a count");
    Expect(loads - before <= 1, "one present loaded more than one copy");
    return wanted;
}
int main(int argc, char **argv) {
    const int scenario = argc > 1 ? std::atoi(argv[1]) : 0;
    if (scenario == 0) {
        Expect(Present(1) == 1, "the first bring-up at 1");
        const UINT a = Present(3), b = Present(3), c = Present(3);
        Expect(a == 2 && b == 3 && c == 3 && loads == 2, "1 -> 3 goes through 2, a copy a present");
        Expect(Present(1) == 1 && Present(3) == 3 && loads == 2,
               "a lower count and a higher one again load nothing");
    } else if (scenario == 1) {
        const UINT a = Present(3), b = Present(3);
        Expect(a == 2 && b == 3 && loads == 2, "a first bring-up at 3 loads a copy a present too");
    } else {
        // Pass 2's copy fails (scenario 2), or pass 3's once pass 2 is up (scenario 3).
        refuse = scenario == 2 ? 1 : 2;
        const UINT held = refuse;
        Present(1);
        if (scenario == 3)
            Expect(Present(3) == 2 && resets == 1, "1 -> 2 on the way to a copy that fails");
        said.clear();
        const int before = resets;
        for (int i = 0; i < 50; ++i)
            Expect(Present(3) == held, "a count past a copy that failed is not held below it");
        Expect(loads == static_cast<int>(held), "a copy that failed is tried again");
        char line[160];
        std::snprintf(line, sizeof line, "passes: asked for 3, running %u. Pass %u's copy", held,
                      held + 1);
        Expect(said.size() == 1 && said[0].rfind(line, 0) == 0,
               "the failure is not said once, as asked for, running and the pass that failed");
        Expect(resets == before, "a count that did not move is taken, with the history dropped");
    }
    if (!fails) std::printf("ok\n");
    return fails;
}
"""
start = runtimes.find("UINT g_passLimit")
if start < 0 or not bring:
    bad.append("could not find g_passLimit or BringUpEngines in runtimes.inc")
else:
    source = runtimes[start:runtimes.index("\n}\n", runtimes.index("bool BringUpEngines(")) + 3]
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "latch.cpp", Path(tmp) / "latch.exe"
        src.write_text(HARNESS.replace("PASS_LATCH", source), encoding="utf-8")
        cxx = os.environ.get("CXX", "g++")
        try:
            built = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1", "-static",
                                    str(src), "-o", str(exe)], capture_output=True, text=True)
        except FileNotFoundError:
            built = subprocess.CompletedProcess(cxx, 1, "", f"no compiler at {cxx!r}; set CXX to a g++")
        if built.returncode != 0:
            bad.append("BringUpEngines did not compile on its own:\n" + built.stderr[-2000:])
        else:
            for scenario in ("0", "1", "2", "3"):
                ran = subprocess.run([str(exe), scenario], capture_output=True, text=True, timeout=30)
                if ran.returncode != 0:
                    bad.append(f"pass latch, scenario {scenario}: " + (ran.stdout.strip() or ran.stderr.strip()))

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print("PASS same frame only: Inline=0 ignored, no decision on the menu flag, a latched async stands "
      "down, Timing only on the 32-bit bridge, one JobGate on each of the five routes, busy from the "
      "runtime's own count, no timed let-go of a runtime job, a pause clears the job latch, every "
      "failed Close retires its job, one pass-count latch that drains, loads missing copies live, "
      "latches a failed one and resets history")
