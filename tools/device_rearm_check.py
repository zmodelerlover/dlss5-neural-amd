"""A game that destroys its device gets the effect back on the new one wherever the network runs on
a D3D12 device of our own; the D3D12 route, and our own device removed, still stand down.

PCSX2 and RPCS3 destroy their device when they switch game or renderer, and every route used to
stand down with "restart the game to re-enable" (DeviceGone). Where the network runs on our own
device (D3D11, Vulkan, OpenGL) nothing of it depended on the game's, so only what crossed to the
game's goes. GameDeviceGone (core/transport/Transports.inc) is compiled here with g++ against stubs
and run, so the rule is run, not read:
  - our queue drained: everything sized to the swapchain is dropped, the primary's latch let go,
    the history reset with the reason the route gave, logged once, the add-on left up, and true;
  - the add-on already down, our own device removed, or a wait on our queue that gave up: false,
    and nothing released, let go or reset -- what the GPU may still read is kept.
And read from the text:
  - D3D11 and Vulkan answer destroy_device through GameDeviceGone with a reason, never DeviceGone,
    and forget what they held of the game's device: D3D11 only once GameDeviceGone said yes, Vulkan
    the handle and its semaphores first, since ReShade has already unregistered the device;
    BridgeStep1 opens a new game device beside the work device it already has, and Vulkan imports
    its fences into every VkDevice and identifies the GPU once;
  - a removed D3D11 game device sends its frames out as drawn and does not stand down;
  - OpenGL has no destroy_device of its own and rebuilds on a new context with a history reset;
  - D3D12 stands down with DeviceGone and says to restart;
  - OnPresent stands the add-on down on our own removed device before any route runs;
  - the 32-bit frontend starts a new helper on the new device instead of failing.

    python tools/device_rearm_check.py

It needs a C++20 g++: the one CXX names, else g++ on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRANSPORT = ROOT / "core/transport"


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def code(text):
    """`text` without its comments: a call named in a comment is not a call made."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def body(text, signature, indent=""):
    """The definition of `signature`, not a declaration, to the brace closing at `indent`."""
    m = re.search(re.escape(signature) + r"[^;{]*\)[^;{]*\{", text)
    return text[m.start():text.index("\n" + indent + "}", m.end())] if m else ""


HARNESS = r"""
#include <atomic>
#include <cstdio>
// What GameDeviceGone reads and calls, recorded.
struct {
    struct { bool unavailable = false, failed = false; } status;
    std::atomic<void *> primarySwapchain { nullptr };
    unsigned long long completion = 7;
} g;
bool lost = false, drains = true;
int released = 0, logs = 0;
const char *reset = nullptr;
// Both stand the add-on down when they say no, as DeviceLost and WaitFence do.
bool DeviceLost() { if (lost) g.status.unavailable = true; return lost; }
bool WaitForWorkQueue(unsigned long long) { if (!drains) g.status.unavailable = true; return drains; }
void ReleaseSwapchainSized() { ++released; }
void ResetTemporal(const char *why) { reset = why; }
void Log(const char *, ...) { ++logs; }
GAME_DEVICE_GONE
int fails = 0, primary;
bool Run(bool unavailable, bool failed, bool ourLost, bool drained) {
    g.status.unavailable = unavailable, g.status.failed = failed, lost = ourLost, drains = drained;
    g.primarySwapchain.store(&primary);
    released = logs = 0, reset = nullptr;
    return GameDeviceGone("the game destroyed its test device");
}
void Expect(bool ok, const char *what) { if (!ok) ++fails, std::printf("FAIL %s\n", what); }
int main() {
    const bool up = Run(false, false, false, true);
    Expect(up && released == 1 && g.primarySwapchain.load() == nullptr && logs == 1 &&
               reset != nullptr && *reset != 0 && !g.status.unavailable,
           "a drained re-arm drops the swapchain's things and the latch, resets history with the "
           "route's reason, logs once and leaves the add-on up");
    const char *why[] = { "already down", "stopped", "our device removed", "the wait gave up" };
    const bool cases[][4] = { { true, false, false, true }, { false, true, false, true },
                              { false, false, true, true }, { false, false, false, false } };
    for (int i = 0; i < 4; ++i) {
        const bool again = Run(cases[i][0], cases[i][1], cases[i][2], cases[i][3]);
        Expect(!again && released == 0 && g.primarySwapchain.load() == &primary &&
                   reset == nullptr && (g.status.unavailable || g.status.failed), why[i]);
    }
    if (!fails)
        std::printf("GameDeviceGone: re-arms on a drained queue, keeps everything otherwise\n");
    return fails;
}
"""

bad = []
factory = code(read("core/transport/Transports.inc"))
gone = body(factory, "bool GameDeviceGone(")
if not gone:
    bad.append("could not find GameDeviceGone in Transports.inc")

d3d11 = read("core/transport/d3d11/D3D11Transport.inc")
d11 = code(d3d11)
destroy11 = body(d11, "void OnDestroyDevice(", "    ")
rearm = destroy11.find("!GameDeviceGone(")
after = destroy11[destroy11.find("return;", rearm):] if rearm >= 0 else ""
if not (rearm >= 0 and "!= gameDevice" in destroy11[:rearm]
        and all(s in after for s in ("gameDevice = nullptr;", "g.bridge.game11.Reset();",
                                     "g.bridge.game11ctx.Reset();", "g.bridge.crossOn11.Reset();",
                                     "g.bridge.backOn11.Reset();", "g.guideDepthCs.Reset();"))):
    bad.append("D3D11 OnDestroyDevice: does not re-arm through GameDeviceGone and only then forget "
               "the game's device, context, fences and depth shader")
step1, opened = body(d11, "void BridgeStep1("), body(d11, "bool OpenBridge(")
if not ("if (g.bridge.failed || gameDevice != nullptr)" in step1
        and 0 <= step1.find("if (OpenBridge(native))") < step1.find("gameDevice = native;")
        and "g.bridge.workDevice == nullptr && !CreateWorkDevice(" in opened):
    bad.append("BridgeStep1: a new game device is not opened beside the work device already up")
removed = re.search(r"if \(FAILED\(g\.bridge\.game11->GetDeviceRemovedReason\(\)\)\) \{(.*?)\n        \}",
                    d11, re.S)
if not removed or "return;" not in removed.group(1) or any(
        s in removed.group(1) for s in ("unavailable", "DeviceGone(", "failed")):
    bad.append("D3D11 Present: a removed game device stands the add-on down instead of going out raw")

vk = code(read("core/transport/vulkan/VulkanTransport.inc"))
destroyvk = body(vk, "void OnDestroyDevice(", "    ")
if not (0 <= destroyvk.find("g_route.device = nullptr;")
        < destroyvk.find("g_route.semToVk = g_route.semToD3D = nullptr;")
        < destroyvk.find("GameDeviceGone(")):
    bad.append("Vulkan OnDestroyDevice: does not forget the VkDevice and its semaphores before "
               "re-arming through GameDeviceGone")
route = code(read("core/transport/vulkan/vk_route.inc"))
ensure = body(route, "bool Ensure(")
imports = ensure.find("if (r.semToVk == nullptr &&")
if not (0 <= ensure.find("if (g.bridge.workDevice == nullptr)") < imports
        and "ImportFence(" not in ensure[:imports] and "r.gpu == nullptr && !FindPhysicalDevice(r)" in ensure):
    bad.append("vk_route.inc Ensure: the fences are not imported into every VkDevice, or the GPU is "
               "identified again on each (one more VkInstance)")

for name, text in (("D3D11", d11), ("Vulkan", vk)):
    if re.search(r"(?<!Game)DeviceGone\(", text) or "restart the game" in text:
        bad.append(f"{name}: still stands down, or says to restart, when the game's device goes")
calls = re.findall(r"GameDeviceGone\(([^)]*)\)", d11 + vk)
if len(calls) != 2 or any(not re.fullmatch(r'"[^"]+"', c) for c in calls):
    bad.append("D3D11 and Vulkan do not each hand GameDeviceGone the reason the history is reset with")

gl = "".join(code((TRANSPORT / "opengl" / f).read_text(encoding="utf-8"))
             for f in ("OpenGLTransport.inc", "gl_route.inc", "gl_frame.inc"))
change = re.search(r"if \(current != r\.context\)\s*\{(.*?)\n    \}", gl, re.S)
if ("OnDestroyDevice" in gl or "DeviceGone(" in gl or not change
        or not all(s in change.group(1) for s in ("ResetTemporal(\"", "r.context = current;", "r.ready = false;"))):
    bad.append("OpenGL: stands down on destroy_device, or does not rebuild on a new context with a "
               "history reset")

d3d12 = code(read("core/transport/d3d12/D3D12Transport.inc"))
destroy12 = body(d3d12, "void OnDestroyDevice(", "    ")
if not re.search(r'(?<!Game)DeviceGone\("[^"]*restart the game[^"]*"\)', destroy12) or "GameDeviceGone" in d3d12:
    bad.append("D3D12 OnDestroyDevice: no longer stands down (its runtime is on the game's device)")
if not all(s in body(factory, "void DeviceGone(") for s in ("g.status.unavailable = true;", "g.status.reason = reason;")):
    bad.append("DeviceGone: does not stand the add-on down with its reason")

neural = code(read("core/addon/neural.cpp"))
present = body(neural, "void OnPresent(")
off = re.search(r"if \(!g\.settings\.enabled\.load\(\) \|\| g\.status\.unavailable \|\| g\.status\.failed \|\| "
                r"DeviceLost\(\)\)\s*\{[^}]*return;", present)
lost = body(code(read("core/addon/runtimes.inc")), "bool DeviceLost(")
if not (off and 0 <= off.start() < present.find("transport->Present(")
        and "g.status.unavailable = true;" in lost and "restart the game" in lost):
    bad.append("OnPresent: our own removed device does not stand the add-on down before the route runs")

f32 = code(read("core/x86bridge/frontend32.cpp"))
leave = f32[f32.index("void OnDestroy("):f32.index("void OnPresent(")]
if "g.failed=true" in leave or "StopHost();" not in leave or not re.search(r"if\(!g\.game11\)\{", f32) \
        or "if(!StartHost())" not in f32:
    bad.append("frontend32: a destroyed device fails the 32-bit bridge instead of starting a new helper "
               "on the new one")

out = ""
if gone:
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "rearm.cpp", Path(tmp) / "rearm.exe"
        src.write_text(HARNESS.replace("GAME_DEVICE_GONE", gone + "\n}\n"), encoding="utf-8")
        cxx = os.environ.get("CXX", "g++")
        try:
            built = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1", "-static",
                                    str(src), "-o", str(exe)], capture_output=True, text=True)
        except FileNotFoundError:
            built = subprocess.CompletedProcess(cxx, 1, "", f"no compiler at {cxx!r}; set CXX to a g++")
        if built.returncode != 0:
            bad.append("GameDeviceGone did not compile on its own:\n" + built.stderr[-2000:])
        else:
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            out = ran.stdout.strip()
            if ran.returncode != 0:
                bad.append(out or ran.stderr.strip())

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print(f"PASS the device re-arm: {out}; D3D11, Vulkan and OpenGL come back on the game's new device, "
      "D3D12 and our own removed device stand down, the history goes with a reason, and the 32-bit "
      "bridge starts a new helper")
