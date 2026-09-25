"""A fence wait ends, and nothing the GPU may still read is released when it ends early.

WaitFence (core/addon/runtimes.inc) waited for ever unless the device died. It now gives up after
FenceWaitCapMs (10 s by default, 0 waits for ever) and stands the add-on down. Its own body is
compiled here with g++ against a fence that never signals, so the bound is run, not read:
  - with the cap at 0 a fence that lands late is still waited for; with no event to wait with,
    the add-on stands down at once;
  - a never-signalled fence, with a stale wake already on the event, gives up at the cap and
    stands the add-on down; one already reached returns at once; after that give-up a pending
    fence is only looked at, never waited out for another whole cap.
And read from the text:
  - LoadSettings takes FenceWaitCapMs with 10000 as its fallback, as the constructed value is,
    and reads 1 to 999 as 1000;
  - ReleaseSwapchainSized and the OpenGL rebuild return before releasing anything when the wait on
    our queue fails, unless the device is gone (then nothing is running);
  - the depth-bind hook takes g.lock only for its eight log lines, not on every bind the game makes.

    python tools/fence_wait_check.py

It needs a MinGW g++ (windows.h, std::thread): the one CXX names, else g++ on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
neural = (ROOT / "core/addon/neural.cpp").read_text(encoding="utf-8")
runtimes = (ROOT / "core/addon/runtimes.inc").read_text(encoding="utf-8")
gl = (ROOT / "core/transport/opengl/gl_route.inc").read_text(encoding="utf-8")


def body(text, name):
    """A function's definition, not its declaration, to the closing brace at column 0."""
    m = re.search(rf"\n[^\n;]*\b{name}\([^;{{]*\)\s*{{", text)
    return text[m.start():text.index("\n}\n", m.start()) + 3] if m else ""


HARNESS = r"""
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
// What WaitFence reads from the add-on, and a fence whose event is never signalled by it.
struct ID3D12Fence {
    std::atomic<UINT64> done { 0 };
    UINT64 GetCompletedValue() { return done.load(); }
    HRESULT SetEventOnCompletion(UINT64, HANDLE) { return S_OK; }
};
struct {
    struct { std::atomic<int> fenceWaitCapMs { 10000 }; } settings;
    struct { bool unavailable = false; const char *reason = ""; } status;
} g;
void Log(const char *, ...) {}
bool DeviceLost() { return false; }
WAIT_FENCE
int main() {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ID3D12Fence fence;
    // These two first: a give-up latches for the rest of the process.
    g.settings.fenceWaitCapMs = 0;
    std::thread late([&] { Sleep(500); fence.done = 1; SetEvent(ev); });
    const bool landed = WaitFence(&fence, 1, ev, "a slow queue");
    late.join();
    if (!landed || g.status.unavailable) {
        std::printf("FAIL FenceWaitCapMs=0 gave up on a fence that landed\n");
        return 1;
    }
    if (WaitFence(&fence, 2, nullptr, "a queue with no event") || !g.status.unavailable || !*g.status.reason) {
        std::printf("FAIL a wait with no event returned false without standing down\n");
        return 1;
    }
    g.status.unavailable = false;
    g.settings.fenceWaitCapMs = 300;
    SetEvent(ev);  // a registration another wait left behind
    const ULONGLONG t0 = GetTickCount64();
    const bool stuck = WaitFence(&fence, 2, ev, "a queue that never finishes");
    const ULONGLONG took = GetTickCount64() - t0;
    if (stuck || took < 290 || took > 1500 || !g.status.unavailable) {
        std::printf("FAIL never-signalled: returned %d after %llu ms, stood down %d\n", stuck, took,
                    g.status.unavailable);
        return 1;
    }
    if (!WaitFence(&fence, 1, ev, "a finished queue")) {
        std::printf("FAIL a reached fence was not reported reached\n");
        return 1;
    }
    const ULONGLONG t1 = GetTickCount64();
    if (WaitFence(&fence, 2, ev, "the same queue at the next resize") || GetTickCount64() - t1 > 100) {
        std::printf("FAIL after a give-up a pending fence was waited out again\n");
        return 1;
    }
    std::printf("PASS gave up at %llu ms of a 300 ms cap\n", took);
    return 0;
}
"""

bad = []
wait = body(runtimes, "WaitFence")
release, ensure, bind = body(neural, "ReleaseSwapchainSized"), body(gl, "Ensure"), body(neural, "OnBindDepthStencil")
# The control line: a check that found nothing to read passes on nothing.
if not (wait and release and ensure and bind):
    bad.append("could not find WaitFence, ReleaseSwapchainSized, the OpenGL Ensure or OnBindDepthStencil")
else:
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "fence_wait.cpp", Path(tmp) / "fence_wait.exe"
        src.write_text(HARNESS.replace("WAIT_FENCE", wait), encoding="utf-8")
        cxx = os.environ.get("CXX", "g++")
        try:
            built = subprocess.run([cxx, "-std=c++20", "-O1", "-static", str(src), "-o", str(exe)],
                                   capture_output=True, text=True)
        except FileNotFoundError:
            built = subprocess.CompletedProcess(cxx, 1, "", f"no compiler at {cxx!r}; set CXX to a MinGW g++")
        if built.returncode != 0:
            bad.append("WaitFence did not compile on its own:\n" + built.stderr[-2000:])
        else:
            try:
                ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
                out = ran.stdout.strip() if ran.returncode == 0 else (ran.stdout or ran.stderr).strip()
            except subprocess.TimeoutExpired:
                out = "a never-signalled fence was still waited for after 30 s"
            if "PASS" not in out:
                bad.append("WaitFence: " + out)
            else:
                print(out)

    if ('capMs = static_cast<int>(num(L"FenceWaitCapMs", 10000.0f));' not in neural
            or "fenceWaitCapMs.store(capMs <= 0 ? 0 : std::max(capMs, 1000));" not in neural
            or "std::atomic<int> fenceWaitCapMs { 10000 };" not in neural):
        bad.append("neural.cpp: FenceWaitCapMs is not 10000 both constructed and as LoadSettings' fallback, "
                   "with 1000 as its floor")
    first = min(i for i in (release.find(s) for s in (".Reset()", "Destroy()", "->ReleaseSwapchainSized()")) if i >= 0)
    if not 0 <= release.find("if (!WaitForWorkQueue(g.completion) && !DeviceLost())\n        return;") < first:
        bad.append("ReleaseSwapchainSized: releases what our queue may still read after its wait gave up")
    guarded = ensure.find("if (!WaitForWorkQueue(g.completion))\n        return false;")
    if not 0 <= guarded < min(ensure.find("BuildCrossing(r, r.in"), ensure.find("crossLocal.Reset()")):
        bad.append("OpenGL Ensure: rebuilds the crossing after the wait on our queue gave up")
    test = bind.find("logged < 8 && logged++ < 8")
    if test < 0 or "lock_guard" in bind[:test] or "static std::atomic<UINT> logged" not in bind:
        bad.append("OnBindDepthStencil: g.lock is taken on every bind, not only for the eight log lines")

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print("PASS the fence wait gives up at FenceWaitCapMs and stands down, nothing is released after it "
      "does, and a depth bind takes g.lock only to log")
