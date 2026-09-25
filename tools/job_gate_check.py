"""Every route runs the engine same-frame, and nothing decides as if it might not.

In the engine's async mode kJobCounter never moves: its one non-zero store sits behind the worker's
sample of kInlineActive (runtime_offsets.h). A job recorded async therefore reads busy until the
500 ms reset, ~2 evaluations a second, while history copied the frame's input and the output smooth
blended it. So async is retired, and these hold it retired:
  - LoadSettings reads `Inline` only to say it ignores 0; nothing in neural.cpp stores the flag;
  - no history, smooth or pass-control decision reads the menu flag (core/temporal/, RecordNetwork,
    BringUpEngines);
  - RecordNetwork reads the module's own latched kInlineActive after each record, stands down
    (unavailable, break) when it is not 1, and does so before the job is counted as ours;
  - the 64-bit panel has no Timing: only the 32-bit bridge draws it, as its pipelining switch.

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


bad = []
record, bring, draw = (body(neural, "bool RecordNetwork("), body(neural, "bool BringUpEngines("),
                       body(perf, "void DrawPerformance("))
temporal = sorted((ROOT / "core/temporal").glob("*.inc"))
# The control line: a check that found nothing to read passes on nothing.
if not (record and bring and draw and temporal):
    bad.append("could not find RecordNetwork, BringUpEngines, DrawPerformance or core/temporal")

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

if bad:
    print("FAIL\n  " + "\n  ".join(bad))
    sys.exit(1)
print("PASS same frame only: Inline=0 ignored, no decision on the menu flag, a latched async stands "
      "down, Timing only on the 32-bit bridge")
