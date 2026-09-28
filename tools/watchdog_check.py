"""A network that keeps outlasting InlineWaitMs steps the scale down, and only at the lowest stands
the add-on down; InlineWaitMs is only ever read.

The runtime's watchdog lets the game's queue go when a job outlasts InlineWaitMs and counts each
time it does (rt::B->kWatchdogFires, one count per module). NoteWatchdog (core/addon/runtimes.inc)
takes the scale one step down after WatchdogStandDown retired evaluations in a row that each
tripped it, and stands the add-on down only when that happens at the lowest scale. NoteJobCost
(neural.cpp) steps on three jobs past 250 ms, and at the lowest counts only one the watchdog also
stopped. Both step through StepScaleDown. The three bodies are compiled here with g++ against
counters this script moves, each evaluation noted the way JobGate notes it, so the rules are run,
not read:
  - at the lowest scale, seven in a row and then a clean one never stand down, and neither does a
    count a staging recreate zeroed; a fire on any module counts, and so does a count that went
    down but not to 0; the eighth in a row stands down with a reason; WatchdogStandDown=0 never;
  - a network that is only slow (196 ms: past the budget, under 250 ms) at Scale 2.0 takes one
    step per eight in a row all the way to 0.25, and stands down only after eight more there;
  - a resize, or a step either note took, starts the streak again, and a streak that completes on
    the evaluation of NoteJobCost's third long job takes one step, not two;
  - at the lowest scale, long readings the watchdog did not stop (a loading screen) never stand
    down, and three it did stop do.
And read from the text:
  - JobGate notes the watchdog where it retires a job, before NoteJobCost, and hands it whether the
    watchdog fired, so every route has both; it runs nothing on the frame either stood it down;
  - WatchdogStandDown is 8 both constructed and as LoadSettings' fallback;
  - InitEngine logs the InlineWaitMs in force (kWaitBudgetMax) and warns over 200; nothing writes
    that field, and EnsureEngineIni leaves an existing dlssnr_on_amd.ini alone.

    python tools/watchdog_check.py

It needs a MinGW g++ (windows.h): the one CXX names, else g++ on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
neural = "\n".join((ROOT / f).read_text(encoding="utf-8")
                   for f in ("core/addon/neural.cpp", "core/addon/runtime_files.inc"))
runtimes = (ROOT / "core/addon/runtimes.inc").read_text(encoding="utf-8")


def body(text, name):
    """A function's definition, not its declaration, to the closing brace at column 0."""
    m = re.search(rf"\n[^\n;]*\b{name}\([^;{{]*\)\s*{{", text)
    return text[m.start():text.index("\n}\n", m.start()) + 3] if m else ""


HARNESS = r"""
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
// What the three notes read from the add-on. A module is a counter of fires at offset 0.
struct State { static constexpr UINT kMaxPasses = 3; };
namespace rt { struct Build { size_t kWatchdogFires; }; inline constexpr Build kStub { 0 }; inline const Build *B = &kStub; }
template <class T> T &At(HMODULE h, size_t rva) {
    return *reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(h) + rva);
}
struct {
    HMODULE runtimes[State::kMaxPasses] {};
    struct {
        std::atomic<int> watchdogStandDown { 8 };
        std::atomic<float> scale { 0.25f };
    } settings;
    struct { bool unavailable = false; const char *reason = ""; } status;
    std::atomic<float> scaleCap { 0.0f };
    std::atomic<UINT64> worstJobMs { 0 };
    UINT longJobs = 0, netWidth = 480, netHeight = 270;
} g;
float EffectiveScale() {  // ui::EffectiveScale
    const float cap = g.scaleCap.load(), scale = g.settings.scale.load();
    return cap > 0.0f && cap < scale ? cap : scale;
}
void Log(const char *, ...) {}
STEP_SCALE_DOWN
NOTE_WATCHDOG
NOTE_JOB_COST
UINT fires[State::kMaxPasses] {};
int step = 0;
// One evaluation of `ms` retired, after the modules' counts were set, and noted as JobGate notes
// it; true if the add-on is up or down as `down` says.
bool Up(const char *what, bool down, UINT64 ms = 10) {
    ++step;
    const bool fired = NoteWatchdog();
    NoteJobCost(ms, fired);
    if (g.status.unavailable == down)
        return true;
    std::printf("FAIL step %d (%s): %s\n", step, what, down ? "did not stand down" : "stood down");
    return false;
}
bool Cap(const char *what, float want) {
    if (g.scaleCap.load() == want)
        return true;
    std::printf("FAIL step %d (%s): cap %.2f, not %.2f\n", step, what, g.scaleCap.load(), want);
    return false;
}
void Fresh(float scale) {
    g.status.unavailable = false;
    g.settings.scale = scale;
    g.scaleCap = 0.0f;
    g.longJobs = 0;
}
int main() {
    g.runtimes[0] = reinterpret_cast<HMODULE>(&fires[0]);
    g.runtimes[1] = reinterpret_cast<HMODULE>(&fires[1]);
    // At the lowest scale (the constructed Scale here), where eight in a row are a stand-down.
    for (int i = 0; i < 7; ++i)
        if (fires[i % 2] += 1, !Up("seven fires in a row, on either module", false))
            return 1;
    if (!Up("a clean evaluation after seven", false))
        return 1;
    for (int i = 0; i < 7; ++i)
        if (fires[0] += 2, !Up("seven more, after the clean one", false))
            return 1;
    fires[0] = 0;  // the module recreated its staging, and nothing fired since
    if (!Up("a count zeroed by a staging recreate", false))
        return 1;
    for (int i = 0; i < 7; ++i)
        if (fires[0] += 1, !Up("seven after the recreate", false))
            return 1;
    fires[0] = 2;  // recreated again, and it fired twice since
    if (!Up("the eighth in a row, a count that went down but not to 0", true) || !*g.status.reason)
        return 1;
    Fresh(0.25f);
    g.settings.watchdogStandDown = 0;
    for (int i = 0; i < 20; ++i)
        if (fires[1] += 1, !Up("WatchdogStandDown=0", false))
            return 1;
    g.settings.watchdogStandDown = 8;

    // Only slow: 196 ms outlasts the budget every time and never reaches NoteJobCost's 250 ms.
    // Each eight in a row are one step; only eight more at the lowest stand it down.
    Fresh(2.0f);
    for (float want = 1.75f; want >= 0.25f; want -= 0.25f)
        for (int i = 0; i < 8; ++i)
            if (fires[0] += 1, !Up("a slow network above the lowest", false, 196) ||
                               !Cap("a slow network above the lowest",
                                    i == 7 ? want : want == 1.75f ? 0.0f : want + 0.25f))
                return 1;
    for (int i = 0; i < 7; ++i)
        if (fires[0] += 1, !Up("seven slow at the lowest", false, 196))
            return 1;
    g.netWidth = 640;  // a resize: the streak so far was on another raster
    for (int i = 0; i < 7; ++i)
        if (fires[0] += 1, !Up("seven slow after a resize at the lowest", false, 196))
            return 1;
    if (fires[0] += 1, !Up("the eighth slow after the resize, at the lowest", true, 196))
        return 1;

    // Five slow, then three past 250 ms: the eighth fire and the third long job land together and
    // take one step between them. The streak then starts again on the new scale.
    Fresh(1.0f);
    for (int i = 0; i < 8; ++i)
        if (fires[1] += 1, !Up("five slow, three long", false, i < 5 ? 196 : 300) ||
                           !Cap("five slow, three long", i < 7 ? 0.0f : 0.75f))
            return 1;
    for (int i = 0; i < 8; ++i)
        if (fires[1] += 1, !Up("eight slow after the step", false, 196) ||
                           !Cap("eight slow after the step", i < 7 ? 0.75f : 0.5f))
            return 1;
    // Three long jobs the watchdog never stopped (an InlineWaitMs over 300): NoteJobCost steps.
    for (int i = 0; i < 3; ++i)
        if (!Up("three long, unwatched, above the lowest", false, 300) ||
            !Cap("three long, unwatched, above the lowest", i < 2 ? 0.5f : 0.25f))
            return 1;
    // At the lowest, long readings the watchdog did not stop are present gaps, not jobs.
    for (int i = 0; i < 6; ++i)
        if (!Up("long present gaps at the lowest", false, 400))
            return 1;
    for (int i = 0; i < 3; ++i)
        if (fires[1] += 1, !Up("three long jobs the watchdog stopped, at the lowest", i == 2, 400))
            return 1;
    std::printf("PASS the watchdog steps the scale on eight fires in a row and stands down only at "
                "the lowest, over %d evaluations\n", step);
    return 0;
}
"""

bad = []
note, gate, down = body(runtimes, "NoteWatchdog"), body(runtimes, "JobGate"), body(runtimes, "StepScaleDown")
cost, init, ini = body(neural, "NoteJobCost"), body(neural, "InitEngine"), body(neural, "EnsureEngineIni")
# The control line: a check that found nothing to read passes on nothing.
if not (note and gate and down and cost and init and ini):
    bad.append("could not find NoteWatchdog, JobGate, StepScaleDown, NoteJobCost, InitEngine or EnsureEngineIni")
else:
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "watchdog.cpp", Path(tmp) / "watchdog.exe"
        harness = HARNESS.replace("STEP_SCALE_DOWN", down).replace("NOTE_WATCHDOG", note)
        src.write_text(harness.replace("NOTE_JOB_COST", cost), encoding="utf-8")
        cxx = os.environ.get("CXX", "g++")
        try:
            built = subprocess.run([cxx, "-std=c++20", "-O1", "-static", str(src), "-o", str(exe)],
                                   capture_output=True, text=True)
        except FileNotFoundError:
            built = subprocess.CompletedProcess(cxx, 1, "", f"no compiler at {cxx!r}; set CXX to a MinGW g++")
        if built.returncode != 0:
            bad.append("StepScaleDown, NoteWatchdog and NoteJobCost did not compile on their own:\n"
                       + built.stderr[-2000:])
        else:
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            out = (ran.stdout or ran.stderr).strip()
            if ran.returncode != 0 or "PASS" not in out:
                bad.append("the watchdog and job-cost notes: " + out)
            else:
                print(out)

    retire = re.search(r"if \(!pending && g\.jobRunning\)\s*\{([^}]*)\}", gate)
    if not retire or not re.search(r"const bool fired = NoteWatchdog\(\);\s*NoteJobCost\([^;]*, fired\);",
                                   retire.group(1)):
        bad.append("JobGate: the watchdog is not noted where a job retires, before NoteJobCost and "
                   "handed to it")
    if "if (!pending)\n        return !g.status.unavailable;" not in gate:
        bad.append("JobGate: runs the network on the frame a note has just stood the add-on down")
    if ("std::atomic<int> watchdogStandDown { 8 };" not in neural
            or 'num(L"WatchdogStandDown", 8.0f)' not in neural):
        bad.append("neural.cpp: WatchdogStandDown is not 8 both constructed and as LoadSettings' fallback")
    if "StepScaleDown(" not in note or "StepScaleDown(" not in cost or "g.scaleCap.store(" in cost:
        bad.append("NoteWatchdog and NoteJobCost do not both step the scale through StepScaleDown")
    if "At<int>(h, rt::B->kWaitBudgetMax)" not in init or "budget > 200" not in init:
        bad.append("InitEngine: InlineWaitMs in force (kWaitBudgetMax) is not logged, or not warned over 200")
    for path in sorted((ROOT / "core").rglob("*")):
        if path.suffix in (".cpp", ".h", ".inc"):
            text = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"rt::B->kWaitBudgetMax\)\s*=[^=]", text) or re.search(r"WritePrivateProfile\w*\([^;]*InlineWaitMs", text):
                bad.append(f"{path.relative_to(ROOT).as_posix()}: writes InlineWaitMs, which is the person's")
    if not 0 <= ini.find("if (std::filesystem::exists(ini, ec))\n        return;") < ini.find("std::ofstream"):
        bad.append("EnsureEngineIni: an existing dlssnr_on_amd.ini is no longer left alone")

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print("PASS the watchdog steps the scale down after WatchdogStandDown fires in a row on every route and "
      "stands down only at the lowest, where only a job it stopped counts, and InlineWaitMs is logged, "
      "never written")
