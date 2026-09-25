"""Only the transport factory may ask which graphics API it is on, and the hand-offs between the
routes and the runtime that no offline run can see.

Every per-API decision the add-on makes goes through the port in core/transport/FrameTransport.hpp; a
`device_api::` anywhere else is the add-on branching on the API again, which is the coupling the
port was built to remove.

The rest is read from the text, because framecheck never copies `composed` to a back buffer and
never submits through ReShade, so its InfoQueue sees none of it:
  - on every route a list is executed before the runtime is told about it, the order the runtime's
    own detour keeps (tools/runtime-patches.json, 0x8873);
  - on every route a copy out of `composed` follows its barrier into COPY_SOURCE;
  - on D3D12 a failed RecordNetwork still runs the tail (back buffer to present, completion Signal),
    except the `cmd_list == nullptr` exit, which a graphics queue never takes;
  - a readback is mapped only once its fence has landed, and parked, not freed, when it has not;
  - the live D3D12 depth buffer is never read (it measured zeros), and the pre-clear snapshot is
    rebuilt on a format change and parked rather than released under an in-flight list.

    python tools/transport_check.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FACTORY = ROOT / "core/transport/Transports.inc"
SCOPE = [ROOT / "core/addon", ROOT / "core/transport", ROOT / "core/diagnostics/framecheck", ROOT / "core/x86bridge/host64.cpp"]
NEURAL = ROOT / "core/addon/neural.cpp"
D3D12 = ROOT / "core/transport/d3d12/D3D12Transport.inc"
GUIDES = ROOT / "core/transport/d3d12/D3D12Guides.inc"


def body(text, name):
    """A function's definition, not its declaration, to the closing brace at column 0."""
    start = re.search(rf"\n[^\n;]*\b{name}\([^;{{]*\)\s*{{", text).start()
    return text[start:text.index("\n}\n", start)]


bad = []
files = []
for base in SCOPE:
    files += [base] if base.is_file() else sorted(p for p in base.rglob("*") if p.suffix in (".cpp", ".h", ".inc"))
for f in files:
    lines = f.read_text(encoding="utf-8").splitlines()
    where = lambda n: f"{f.relative_to(ROOT)}:{n}: {lines[n - 1].strip()}"
    for n, line in enumerate(lines, 1):
        before = "\n".join(lines[max(0, n - 5):n - 1])
        if "device_api::" in line and f != FACTORY:
            bad.append("device_api read outside the transport factory: " + where(n))
        if "NotifyRuntimes(" in line and "void NotifyRuntimes(" not in line and not any(
                s in "\n".join(lines[max(0, n - 3):n - 1]) for s in ("ExecuteCommandLists(", "flush_immediate_command_list()")):
            bad.append("runtime notified before its list was executed: " + where(n))
        if "CopyResource(" in line and "g.composed.Get())" in line and not (
                "g.composed.Get(), D3D12_RESOURCE_STATE_" in before and "D3D12_RESOURCE_STATE_COPY_SOURCE)" in before):
            bad.append("copy out of composed without its barrier into COPY_SOURCE: " + where(n))

route = D3D12.read_text(encoding="utf-8").splitlines()
first = next(n for n, line in enumerate(route) if "RecordNetwork(" in line)
last = max(n for n, line in enumerate(route) if "flush_immediate_command_list()" in line)
for n in range(first, last):
    if "return;" in route[n] and "cmd_list == nullptr" not in route[n - 1] + route[n]:
        bad.append(f"D3D12 route returns between RecordNetwork and its tail at line {n + 1}: {route[n].strip()}")

neural = NEURAL.read_text(encoding="utf-8")
drain = body(neural, "DrainReadbacks")
fences, mapped, parked = (drain.count(s) for s in ("Signal(f.Get(), 1)", "GetCompletedValue() >= 1 &&", "g.parked.insert("))
if not fences or not fences == mapped == parked:
    bad.append(f"DrainReadbacks: {fences} fences, {mapped} maps behind them, {parked} parks; each readback needs all three")
if "depthBest" in body(neural, "RecordNetwork"):
    bad.append("RecordNetwork reads the live D3D12 depth buffer, which measured zeros")
snapshot = body(GUIDES.read_text(encoding="utf-8"), "SnapshotBeforeClear")
if "Format != DepthAliasFormat(" not in snapshot or "depthSnapshot.Reset()" in snapshot:
    bad.append("SnapshotBeforeClear: the snapshot must be rebuilt on a format change and parked, not reset")

if bad:
    print("FAIL")
    print("\n".join(bad))
    sys.exit(1)
print(f"PASS only the transport factory reads device_api; D3D12 states and lifetimes hold ({len(files)} files)")
