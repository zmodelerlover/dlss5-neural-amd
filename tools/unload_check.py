"""Nothing the add-on leaves behind at unload or exit points into it, or runs after the driver left.

NFS unloads the add-on mid-process at device destroy and exits later. A vectored handler whose
handle was dropped, or a fault filter that was never put back, then points into unmapped code for
the rest of shutdown. And a global that owns COM objects releases them from a CRT destructor,
under the loader lock, after the runtime, HIP and the driver have already detached -- so those are
never destroyed; the built objects are asked too, since a map or struct holding a ComPtr slips
past any source pattern. The resize drain is destroy_swapchain; Events without bit 8 used to drop
it and bring back the refused resize.

    python tools/unload_check.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
neural = (ROOT / "core/addon/neural.cpp").read_text(encoding="utf-8")
gl = (ROOT / "core/transport/opengl/OpenGLTransport.inc").read_text(encoding="utf-8")

bad = []
if not re.search(r"g_probe = AddVectoredExceptionHandler\(", neural) or \
        "RemoveVectoredExceptionHandler(g_probe)" not in neural:
    bad.append("neural.cpp: the null-jump probe's handle is kept and removed at DETACH")
if "SetUnhandledExceptionFilter(g_previousFilter)" not in gl or "!= ReportFault" not in gl or \
        "!filterInstalled" not in gl:
    bad.append("OpenGLTransport.inc: an unload puts back the previous fault filter, only if it set one"
               " and ours is current")

# Every global that owns COM objects, declared as a reference to a leaked object.
owners = [("addon/neural.cpp", "g"), ("transport/opengl/gl_crossing.inc", "g_route"),
          ("transport/vulkan/vk_crossing.inc", "g_route"), ("temporal/optical_flow.inc", "g_flow"),
          ("temporal/smooth.inc", "g_filters"), ("temporal/motion_feed.inc", "g_motionFeed"),
          ("addon/neural.cpp", "g_depthTally"), ("addon/neural.cpp", "g_motionTally"),
          ("addon/neural.cpp", "g_d12DepthTally")]
for rel, name in owners:
    if not re.search(rf"^\S.*&{name} = \*new ", (ROOT / "core" / rel).read_text(encoding="utf-8"), re.M):
        bad.append(f"core/{rel}: {name} is not a never-destroyed reference")
# And no new one by value, in the sources the add-on is built from.
for part in ("addon", "transport", "temporal", "shared"):
    for path in sorted((ROOT / "core" / part).rglob("*")):
        if path.suffix not in (".cpp", ".inc", ".h", ".hpp"):
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if re.match(r"(static )?ComPtr<[^>]+> g_?\w*;", line):
                bad.append(f"{path.relative_to(ROOT).as_posix()}:{n}: global ComPtr destroyed at exit")
# The last word is the compiler's: every global the CRT destroys at exit has a ??__F symbol. Only
# the transports (no COM inside) and the panel's exported-path string may have one.
checked = 0
for obj in ("build/neural.obj", "build-x86bridge/obj-x64/host64.obj"):
    if (ROOT / obj).exists():
        checked += 1
        for name in sorted(set(re.findall(rb"\?\?__F(\w+)@", (ROOT / obj).read_bytes()))):
            if name not in (b"transport", b"exported"):
                bad.append(f"{obj}: {name.decode()} is destroyed at exit")

# The resize drain: its registration must not sit under an Events bit.
lines = neural.splitlines()
at = [i for i, line in enumerate(lines) if "addon_event::destroy_swapchain>" in line]
if len(at) != 1 or lines[at[0] - 1].strip().startswith("if (g.events"):
    bad.append("neural.cpp: destroy_swapchain is registered once and unconditionally")

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print(f"PASS nothing dangles at unload, no COM-holding global is destroyed at exit ({checked} "
      "built objects asked), the resize drain is on")
