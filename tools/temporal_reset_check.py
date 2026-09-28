"""The temporal history is dropped by one rule on every route, and only through ResetTemporal.

Each route used to keep its own list. The 64-bit panel dropped it on Scale, depth direction and
History; the 32-bit helper on History alone (Factory Defaults on a guide list of its own); an ini
reload nowhere; switching the effect back on nowhere; and every swapchain rebuild on the 32-bit
route, which PCSX2 and Xenosaga 2 do every few frames. Now (core/temporal/history.inc):
  - every drop is ResetTemporal(reason): one log line when the reason changes, with its count, and
    no historyValid.store( anywhere else in core/;
  - one settings list, SettingsInvalidateHistory (core/shared/history_keys.h), used by the 64-bit
    panel and the helper's ApplySettings, and LoadSettings drops it on every ini read;
  - the raster rebuilt (EnsureResources) drops it and a swapchain rebuild alone does not, on either
    route: not ReleaseSwapchainSized, not the helper's Drop or Build;
  - off (hotkey, panel, alt-tab) is a pause, so switching back on resumes through the restore that
    drops it; a new OpenGL context drops it;
  - after the pass loop, the bits of the slots that did not run are cleared.
Guide takeovers are held by guide_switch_check.py, the pass-count drain by job_gate_check.py and
the 32-bit frontend's resets by test-x86bridge.py.

    python tools/temporal_reset_check.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
bad = []


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8-sig")


def code(text):
    """`text` without its comments: a call named in a comment is not a call made."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def body(text, signature):
    """The definition of `signature`, not a declaration, up to the brace closing at its indent."""
    m = re.search(re.escape(signature) + r"[^;{]*\)\s*\{", text)
    indent = signature[:len(signature) - len(signature.lstrip())]
    return text[m.start():text.index("\n" + indent + "}", m.end())] if m else ""


for path in sorted((ROOT / "core").rglob("*")):
    if path.suffix not in (".cpp", ".h", ".hpp", ".inc"):
        continue
    text = code(path.read_text(encoding="utf-8-sig", errors="replace"))
    stores = text.count("historyValid.store(")
    if path.name == "history.inc":
        reset = body(text, "void ResetTemporal(")
        if stores != 1 or "historyValid.store(0)" not in reset:
            bad.append("history.inc: the history is stored to outside ResetTemporal")
        if not all(s in reset for s in ("++times[why]", "if (why != last)", "Log(")):
            bad.append("history.inc: ResetTemporal does not log once per reason change with a count")
    elif stores:
        bad.append(f"{path.relative_to(ROOT)}: stores historyValid itself instead of ResetTemporal")

keys = read("core/shared/history_keys.h")
for field in ("scale", "depthInverted", "useHistory", "temporalMode", "useMotion", "useDepth",
              "useGameGuides", "motionScale", "flowGate", "flowRatio"):
    if f"a.{field} != b.{field}" not in keys:
        bad.append(f"history_keys.h: {field} is not in SettingsInvalidateHistory")

neural = code(read("core/addon/neural.cpp"))
host = code(read("core/x86bridge/host64.cpp"))
panel = body(code(read("core/addon/panel64.inc")), "void ApplyPanelSettings(")
if not re.search(r"if \(SettingsInvalidateHistory\(before, after\)\)\s*ResetTemporal\(", panel):
    bad.append("panel64.inc: the 64-bit panel does not drop history by the shared list")
apply = body(host, "    bool ApplySettings(")
if ("SettingsInvalidateHistory(ExportSettings(),s)" not in apply
        or "if(historyChanged)ResetTemporal(" not in apply):
    bad.append("host64.cpp: the 32-bit helper does not drop history by the shared list")
if not re.search(r"ResetTemporal\(\"[^\"]*\"\);\s*$", body(neural, "void LoadSettings(")):
    bad.append("neural.cpp: an ini read does not drop the history (OpticalFlow is in no struct)")

ensure = body(neural, "bool EnsureResources(")
if not re.search(r"if \(netChanged\)\s*\{[^}]*\}\s*ResetTemporal\(", ensure):
    bad.append("neural.cpp: a rebuilt raster does not drop the history")
for name, text in (("ReleaseSwapchainSized", body(neural, "void ReleaseSwapchainSized(")),
                   ("host Drop", body(host, "    void Drop(")),
                   ("host Resources", body(host, "    void Resources("))):
    if "ResetTemporal(" in text or "historyValid" in text:
        bad.append(f"{name}: a swapchain rebuild alone drops the history")

present = body(neural, "void OnPresent(")
off = re.search(r"DeviceLost\(\)\)\s*\{\s*g\.status\.windowHidden = true;\s*return;", present)
alt = re.search(r"g\.settings\.enabled\.store\(false\);\s*g\.status\.windowHidden = true;", present)
back = re.search(r"if \(g\.status\.windowHidden\)\s*\{([^}]*)\}", present)
if not (off and alt and back and "ResetTemporal(" in back.group(1)):
    bad.append("neural.cpp: switching the effect back on does not resume through the restore's drop")

record = body(neural, "bool RecordNetwork(")
if not re.search(r"g\.activePasses = accepted;\s*g\.historyValid\.fetch_and\(\(1u << accepted\) - 1\);",
                 record):
    bad.append("neural.cpp: a slot that did not run this frame keeps its history bit")

gl = code(read("core/transport/opengl/gl_route.inc"))
if not re.search(r"if \(r\.context != nullptr\)\s*ResetTemporal\(", gl):
    bad.append("gl_route.inc: a new OpenGL context keeps the old one's history")

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print("PASS temporal resets: one ResetTemporal with a reason and a count, one settings list for both "
      "panels and an ini read, the raster and not a swapchain rebuild, switch-on resumes clean, a new "
      "GL context, and the bits of passes that sat out")
