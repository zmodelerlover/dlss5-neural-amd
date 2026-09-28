"""The stats line counts what each present did, apart, and the panel's skip rate is a real rate.

Each route logged its own "frame N processed (M skipped)", a pass the engine refused was counted
as skipped, and the panel read the skip rate as skipped / (frame + skipped) where frame already
counts the skipped presents. Now JobGate ticks one set of running totals once a present on every
route (core/addon/stats.inc), and a line a second says what that second did (FormatStats,
core/shared/frame_stats.h). Both are compiled here with g++, against a clock this script moves:
  - a window formats exactly, and a window with no job retired says "job max none";
  - no line inside the first second; one at the second, logged only with Diagnostics bit 2 and kept
    for the panel either way, covering exactly that window's counts;
  - a hook finding g.lock held counts one bind wait; one that finds it free counts none.
And read from the text:
  - JobGate ticks first, and nothing else does; a refused pass counts as refused, not as skipped;
    an evaluation counts where a pass was accepted; a held heap counts inside skipped;
  - both panels read processed as the presents less the skipped ones, and the 64-bit one shows the
    line under Debug;
  - Diagnostics bit 1 is the old hotkeys, and the game's bind and clear hooks lock through
    LockForHook.

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
static unsigned long long fakeNow = 5000;
#define GetTickCount64() fakeNow
struct {
    std::mutex lock;
    struct { uint64_t skipped = 0; } status;
    struct { int diagnostics = 0; } settings;
    std::atomic<unsigned long long> worstJobMs { 0 };
} g;
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
    Expect(FormatStats(a - FrameCounts {}, 1.0, 18) ==
               "stats: 1.00 s | presents 61 eval 58 skip 3 (heap 1) refused 0 | bind waits 2 | job max 18 ms",
           "a window formats as the line");
    Expect(FormatStats(FrameCounts {}, 2.5, 0).find("job max none") != std::string::npos,
           "a window with no job retired says so");

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
    Expect(g_stats.line == "stats: 1.00 s | presents 60 eval 20 skip 4 (heap 0) refused 1 | bind waits 0 | job max 31 ms",
           "the first second's line counts that second");
    Expect(logs == 0, "Diagnostics=1 keeps the line out of the log");
    Expect(g.worstJobMs == 0, "the job maximum starts again each window");
    g.settings.diagnostics = 3;
    fakeNow = 7500;
    TickStats();
    Expect(logs == 1 && logged == g_stats.line && logged.rfind("stats: 1.50 s | presents 1 eval 0 skip 0", 0) == 0,
           "the next window is its own, and bit 2 logs it");

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
if not re.search(r"RouteCaps\(st\);[^\n]*\n\s*if \(!g_stats\.line\.empty\(\)\)\s*st\.routeDiagnostics\.push_back\(g_stats\.line\);", panel):
    bad.append("panel64.inc: the stats line is not under Debug")
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

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print("PASS the stats line: " + out + "; one tick a present, refused apart from skipped, both panels' "
      "processed without the skips, Diagnostics as bits, every bind hook counted")
