"""Only the transport factory may ask which graphics API it is on.

Every per-API decision the add-on makes goes through the port in core/transport/FrameTransport.hpp; a
`device_api::` anywhere else is the add-on branching on the API again, which is the coupling the
port was built to remove.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FACTORY = ROOT / "core/transport/Transports.inc"
SCOPE = [ROOT / "core/addon", ROOT / "core/transport", ROOT / "core/diagnostics/framecheck", ROOT / "core/x86bridge/host64.cpp"]

bad = []
for base in SCOPE:
    files = [base] if base.is_file() else sorted(p for p in base.rglob("*") if p.suffix in (".cpp", ".h", ".inc"))
    for f in files:
        if f == FACTORY:
            continue
        for n, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
            if "device_api::" in line:
                bad.append(f"{f.relative_to(ROOT)}:{n}: {line.strip()}")

if bad:
    print("FAIL device_api read outside the transport factory:")
    print("\n".join(bad))
    sys.exit(1)
print("PASS only the transport factory reads device_api")
