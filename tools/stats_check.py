"""The stats line counts what each present did, apart, and the panel's skip rate is a real rate.

Each route logged its own "frame N processed (M skipped)", a pass the engine refused was counted
as skipped, and the panel read the skip rate as skipped / (frame + skipped) where frame already
counts the skipped presents. Now JobGate ticks one set of running totals once a present on every
route (core/addon/stats.inc), and a line a second says what that second did (FormatStats,
core/shared/frame_stats.h). Both are compiled here with g++, against a clock this script moves:
  - a window formats exactly, and a window with no job retired says "job max none";
  - the holds WaitForPreviousJob notes read as their mean, longest, deadlines run out and spins
    left behind, with a line once to say what the hold is, and no field when nothing held;
  - no line inside the first second; one at the second, logged only with Diagnostics bit 2 and kept
    for the panel either way, covering exactly that window's counts;
  - a hook finding g.lock held counts one bind wait; one that finds it free counts none;
  - a refused pass says why (not ready, four in flight, or neither), and the line adds up the
    watchdog's fires over every module, a count a staging rebuild zeroed included, and ends on each
    module's last job and how many it retired;
  - the temporal line reads the engine's bytes back, ORs a window's evaluations (a skip that
    dropped one pass's history does not flip it), is logged when it changes and only then, and says
    nothing for a window with no evaluation.
And read from the text:
  - JobGate ticks first, and nothing else does; a refused pass counts as refused, not as skipped;
    an evaluation counts where a pass was accepted; a held heap counts inside skipped;
  - both panels read processed as the presents less the skipped ones, and the 64-bit one shows the
    line under Debug;
  - WaitForPreviousJob is off with D3D12Wait=0 (read at load, never saved) and notes each hold
    once, after its spin;
  - Diagnostics bit 1 is the old hotkeys, and the game's bind and clear hooks lock through
    LockForHook;
  - the temporal state is noted once per evaluation, SmoothOutput records the passes it smoothed,
    each motion branch names its source, and the 64-bit panel shows it and the line.

    python tools/stats_check.py

It needs a MinGW g++ (windows.h, std::thread): the one CXX names, else g++ on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8-sig")


HARNESS = r"""
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
static unsigned long long fakeNow = 5000;
#define GetTickCount64() fakeNow
struct {
    std::mutex lock;
    struct { uint64_t skipped = 0; } status;
    struct {
        int diagnostics = 0;
        std::atomic<int> temporalMode { 2 }, fixedSeed { 1 };
        std::atomic<float> outputSmooth { 0.8f }, outputSmoothLimit { 10.0f };
    } settings;
    std::atomic<unsigned long long> worstJobMs { 0 };
    UINT activePasses = 0;
    HMODULE runtimes[3] {};
} g;
struct State { static constexpr UINT kMaxPasses = 3; };
// The runtime is a byte array here, its fields at small offsets, one module for every pass.
enum class GuideSource { None, Game, Effect, Snapshot, Estimated, Unusable, Flow };
namespace rt { struct Build { size_t kHistoryOn, kTemporal, kInlineActive, kUseDepth, kReady, kJobId, kJobCounter, kWatchdogFires; };
               const Build kFake { 1, 2, 3, 4, 5, 8, 12, 16 }, *B = &kFake; }
alignas(4) uint8_t module[32] {};
template <class T> T &At(HMODULE h, size_t rva) { return *reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(h) + rva); }
HMODULE RuntimeFor(UINT) { return reinterpret_cast<HMODULE>(module); }
int Outstanding(HMODULE m) { return static_cast<int>(At<UINT>(m, 8) - At<UINT>(m, 12)); }
bool mochizuki = false;
bool MzSelected() { return mochizuki; }
std::string logged;
int logs = 0;
void Log(const char *fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    logged = b;
    ++logs;
}
#include "stats.inc"
int fails = 0;
void Expect(bool ok, const char *what) {
    if (!ok) { ++fails; std::printf("FAIL %s\n", what); }
}
int main() {
    FrameCounts a;
    a.presents = 61; a.evaluated = 58; a.skipped = 3; a.heapHeld = 1; a.refused = 0; a.bindWaits = 2;
    Expect(FormatStats(a - FrameCounts {}, 1.0, 18, 0.0) ==
               "stats: 1.00 s | presents 61 eval 58 skip 3 (heap 1) refused 0 | bind waits 2 | job max 18 ms | timeouts 0",
           "a window formats as the line, with no hold field when nothing held");
    Expect(FormatStats(FrameCounts {}, 2.5, 0, 0.0).find("job max none") != std::string::npos,
           "a window with no job retired says so");

    // Three holds, one that ran out at 500 ms and one whose spin left the counter behind.
    g.settings.diagnostics = 0;
    NoteHold(3.0, false, false);
    NoteHold(9.5, true, false);
    NoteHold(0.5, false, true);
    Expect(logs == 1 && logged.rfind("hold: ", 0) == 0, "the first hold says once what it is");
    NoteHold(0.0, false, false);
    Expect(logs == 1, "and only once");
    Expect(FormatStats(g_stats.now - FrameCounts {}, 1.0, 20, g_stats.holdMaxMs).find(
               " | job max 20 ms | timeouts 0 | hold 3.2/9.5 ms, deadline 1, spin 1") != std::string::npos,
           "the holds are their mean, their longest, the ones that ran out and the spins left behind");
    g_stats.now = g_stats.last = {};
    g_stats.holdMaxMs = 0.0;
    logs = 0;

    g_stats.at = fakeNow;  // as if the add-on had just loaded
    g.settings.diagnostics = 1;
    for (int i = 0; i < 59; ++i) {
        fakeNow += 16;
        TickStats();
        if (i % 3 == 0) ++g_stats.now.evaluated;
    }
    Expect(g_stats.line.empty() && logs == 0, "no line inside the first second");
    g.status.skipped = 4;
    g_stats.now.refused = 1;
    g.worstJobMs = 31;
    fakeNow = 6000;
    TickStats();
    Expect(g_stats.line == "stats: 1.00 s | presents 60 eval 20 skip 4 (heap 0) refused 1 | bind waits 0 | job max 31 ms | timeouts 0",
           "the first second's line counts that second");
    Expect(logs == 0, "Diagnostics=1 keeps the line out of the log");
    Expect(g.worstJobMs == 0, "the job maximum starts again each window");
    g.settings.diagnostics = 3;
    fakeNow = 7500;
    TickStats();
    Expect(logs == 1 && logged == g_stats.line && logged.rfind("stats: 1.50 s | presents 1 eval 0 skip 0", 0) == 0,
           "the next window is its own, and bit 2 logs it");

    // Two evaluations of one pass in a window, history handed on one of them (a skip dropped it for
    // the other), smoothed there, temporal on, depth withheld: the line ORs the window, and is
    // logged once, then not again while nothing changes.
    module[1] = 1; module[2] = 1; module[3] = 1; module[4] = 0;
    g.activePasses = 1;
    g.settings.diagnostics = 0;
    const int before = logs;
    NoteTemporal(GuideSource::Estimated);
    g_stats.smoothed |= 1u;
    module[1] = 0;
    NoteTemporal(GuideSource::Estimated);
    g_stats.now.evaluated += 2;
    fakeNow = 9000;
    TickStats();
    Expect(logs == before + 1 && logged == "temporal: byte 1 (Temporal=2), engine same-frame, history handed 1/1, "
                                           "smoothed 1/1 at 0.80 under 10/255, seed pinned, motion estimated, depth not handed",
           "the temporal line ORs the window's evaluations");
    module[1] = 1;
    NoteTemporal(GuideSource::Estimated);
    g_stats.smoothed |= 1u;
    ++g_stats.now.evaluated;
    fakeNow = 10000;
    TickStats();
    Expect(logs == before + 1 && g_stats.temporalLine == logged, "the same temporal state is not logged again");
    module[1] = 0;
    NoteTemporal(GuideSource::Game);
    ++g_stats.now.evaluated;
    fakeNow = 11000;
    TickStats();
    Expect(logs == before + 2 && logged.find("history handed 0/1, smoothed 0/1") != std::string::npos &&
               logged.find("motion the game's") != std::string::npos,
           "a new motion source, and a window with no history handed, is a new line");
    fakeNow = 12000;
    TickStats();
    Expect(logs == before + 2, "a window with no evaluation says nothing about the engine");
    TemporalState off;
    off.passes = 2; off.handed = 7; off.motion = "none";
    Expect(FormatTemporal(off) == "temporal: byte 0 (Temporal=0), engine NOT same-frame, history handed 2/2, "
                                  "not smoothed (OutputSmooth=0), seed free, motion none, depth not handed",
           "a pass bit past the passes run is not counted, and smoothing off says so");

    // A refused pass says why, as far as the runtime's own state does.
    const HMODULE r = reinterpret_cast<HMODULE>(module);
    module[5] = 0;
    Expect(std::string(RefusalReason(r)) == "the engine is not ready", "a refusal before the engine was ready");
    module[5] = 1;
    At<UINT>(r, 8) = 816;
    At<UINT>(r, 12) = 812;
    Expect(std::string(RefusalReason(r)) == "four jobs already in flight: job 816, 812 retired",
           "a refusal at four jobs in flight");
    At<UINT>(r, 12) = 816;
    Expect(std::string(RefusalReason(r)) == "no reason the runtime's state shows: job 816, 816 retired",
           "a refusal the runtime's state does not explain");

    // The watchdog's fires on every loaded module, and each module's work, close the line; a count
    // that went down is a module that rebuilt its staging, and all of it is new.
    g.runtimes[0] = r;
    At<UINT>(r, 16) = 2;
    fakeNow = 13000;
    TickStats();
    Expect(g_stats.line.find(" | timeouts 2") != std::string::npos &&
               g_stats.line.ends_with(" | module 1 job 816 retired 816"),
           "the line counts the fires and ends on each module's work");
    At<UINT>(r, 16) = 1;
    fakeNow = 14000;
    TickStats();
    Expect(g_stats.line.find(" | timeouts 1 |") != std::string::npos, "a count a staging rebuild zeroed is new fires");
    g.runtimes[0] = nullptr;

    { auto free = LockForHook(); }
    Expect(g_stats.bindWaits == 0, "a free lock is no wait");
    g.lock.lock();
    std::thread hook([] { auto held = LockForHook(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    g.lock.unlock();
    hook.join();
    Expect(g_stats.bindWaits == 1, "a lock the present holds is one wait");
    if (!fails) std::printf("stats.inc and FormatStats run as specified\n");
    return fails;
}
"""

bad = []


def body(text, signature):
    m = re.search(re.escape(signature) + r"[^;{]*\)\s*\{", text)
    return text[m.start():text.index("\n}\n", m.end())] if m else ""


neural, runtimes = read("core/addon/neural.cpp"), read("core/addon/runtimes.inc")
stats, panel, host = read("core/addon/stats.inc"), read("core/addon/panel64.inc"), read("core/x86bridge/host64.cpp")
gate = body(runtimes, "bool JobGate(")
# The control line: a check that found nothing to read passes on nothing.
if not gate or not body(neural, "bool RecordNetwork("):
    bad.append("could not find JobGate in runtimes.inc or RecordNetwork in neural.cpp")
elif not re.match(r"bool JobGate\(\)\s*\{\s*TickStats\(\);", gate):
    bad.append("runtimes.inc: JobGate does not tick the stats first")
calls = sum(len(re.findall(r"(?<!void )TickStats\(\)", p.read_text(encoding="utf-8", errors="replace")))
            for p in (ROOT / "core").rglob("*") if p.suffix in (".cpp", ".h", ".inc"))
if calls != 1:
    bad.append(f"TickStats is called {calls} times; JobGate is the one place every route passes once a present")
record = body(neural, "bool RecordNetwork(")
if "++g.status.skipped" in record or "if (++g_stats.now.refused % 600 == 1)" not in record:
    bad.append("neural.cpp: a refused pass is counted as skipped, not as refused")
if not re.search(r"if \(accepted != 0\)\s*\{[^}]*\+\+g_stats\.now\.evaluated;", record):
    bad.append("neural.cpp: an evaluation is not counted where a pass was accepted")
if "++g.status.skipped, ++g_stats.now.heapHeld;" not in body(runtimes, "bool HeapStillRead("):
    bad.append("runtimes.inc: a frame HeapStillRead held is not counted as skipped and as heap")
if "st.processed = g.status.frame - std::min(g.status.frame, g.status.skipped);" not in panel:
    bad.append("panel64.inc: processed still counts the skipped presents")
if "s.processed=g.status.frame-std::min(g.status.frame,g.status.skipped);s.skipped=g.status.skipped;" not in host:
    bad.append("host64.cpp: the 32-bit panel's processed still counts the skipped presents")
if not re.search(r"RouteCaps\(st\);[^\n]*\n\s*for \(const std::string \*line : \{ &g_stats\.line, "
                 r"&g_stats\.temporalLine \}\)\s*if \(!line->empty\(\)\)\s*st\.routeDiagnostics\.push_back\(\*line\);", panel):
    bad.append("panel64.inc: the stats and temporal lines are not under Debug")
if "if (g.settings.diagnostics & 1)" not in body(neural, "void OnPresent(") or \
        'g.settings.diagnostics = static_cast<int>(num(L"Diagnostics", 0.0f));' not in neural:
    bad.append("neural.cpp: Diagnostics is not read as bits, with bit 1 the hotkeys")
if not re.search(r"if \(g\.settings\.diagnostics & 2\)\s*Log\(", body(stats, "void TickStats(")):
    bad.append("stats.inc: the stats line is logged without Diagnostics bit 2")
for rel in ("core/transport/d3d12/D3D12Guides.inc", "core/transport/d3d11/D3D11Transport.inc"):
    text = read(rel)
    if re.search(r"std::lock_guard \w+\(g\.lock\)", text) or "LockForHook()" not in text:
        bad.append(f"{rel}: a hook on the game's binds or clears takes g.lock without counting the wait")

with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp) / "stats.cpp", Path(tmp) / "stats.exe"
    src.write_text(HARNESS, encoding="utf-8")
    cxx = os.environ.get("CXX", "g++")
    try:
        built = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1", "-static",
                                "-I" + str(ROOT / "core/addon"), str(src), "-o", str(exe)],
                               capture_output=True, text=True)
    except FileNotFoundError:
        built = subprocess.CompletedProcess(cxx, 1, "", f"no compiler at {cxx!r}; set CXX to a g++")
    out = ""
    if built.returncode != 0:
        bad.append("stats.inc did not compile on its own:\n" + built.stderr[-2000:])
    else:
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        out = ran.stdout.strip()
        if ran.returncode != 0:
            bad.append(out or ran.stderr.strip())

if len(re.findall(r"(?<!void )NoteTemporal\(", neural)) != 1 or not re.search(
        r"if \(accepted != 0\)\s*\{[^}]*NoteTemporal\(haveMotion \? motionFrom : GuideSource::None\);", record):
    bad.append("neural.cpp: the temporal state is not noted once, after an evaluation")
smooth = body(read("core/temporal/smooth.inc"), "void SmoothOutput(").rstrip()
if not smooth.endswith("g_stats.smoothed |= 1u << slot;  // the temporal line (stats.inc)"):
    bad.append("smooth.inc: a pass smoothed is not recorded after its dispatch")
motion = read("core/temporal/motion_sources.inc")
if not ("GuideSource motionFrom = GuideSource::Estimated;" in motion
        and "motionFrom = g.guideMotion.external ? GuideSource::Effect : GuideSource::Game;" in motion
        and "haveMotion = FlowMotion(cmd, colourSrc, colourFmt), motionFrom = GuideSource::Flow;" in motion):
    bad.append("motion_sources.inc: a branch does not say which source it is")
if "static_cast<unsigned long long>(g_stats.now.refused), RefusalReason(r));" not in record:
    bad.append("neural.cpp: a refused pass does not say why")
change = re.search(r"if \(change\)\s*\{[^}]*history starts again[^}]*\}", body(runtimes, "bool BringUpEngines("))
if not change or "g.loggedPassDetail = false;" not in change.group(0):
    bad.append("runtimes.inc: a new pass count does not print each pass's job ids again")
if "st.motionSource = g_stats.motion;" not in panel:
    bad.append("panel64.inc: the motion source is not the last evaluation's")

hold = body(runtimes, "void WaitForPreviousJob(")
if not re.match(r"void WaitForPreviousJob\(\)\s*\{\s*//[^\n]*\n\s*if \(!g\.settings\.d3d12Wait\.load\(\) \|\|", hold) \
        or hold.count("NoteHold(") != 1 or hold.index("NoteHold(") < hold.index("SwitchToThread();"):
    bad.append("runtimes.inc: the hold is not off with D3D12Wait=0, or is not noted once after its spin")
if 'g.settings.d3d12Wait.store(flag(L"D3D12Wait", true));' not in body(neural, "void LoadSettings(") \
        or "D3D12Wait" in body(neural, "void ForEachSetting("):
    bad.append("neural.cpp: D3D12Wait is not read at load with 1 as its default, or is written back")

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print("PASS the stats line: " + out + "; one tick a present, refused apart from skipped, both panels' "
      "processed without the skips, Diagnostics as bits, every bind hook counted; the temporal line once "
      "an evaluation, from the runtime's own bytes, on a change only")
