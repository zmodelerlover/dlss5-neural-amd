"""Hold every source file of our own to 500 lines, and never let a grandfathered one grow.

The add-on grew into one 7706-line translation unit, and a 1508-line copy of its panel for the
32-bit route. Nothing stopped either: a file only ever gets one more function. This is the stop.

    python tools/line_limit_check.py

Files that were already over the limit when this check arrived are listed, with their length at
the time, in `tools/line_limit_allow.json`. That list is a ratchet: an entry may only go down. A
listed file that grows fails, and so does one that has come under the limit or no longer exists,
so the entry has to be deleted rather than left behind to excuse the next file with that name.

Scanned: tracked files under src/, tools/, docs/spike-rocm/ and shaders/, plus the scripts at the root.
3rdparty/ is vendored and binaries are not code.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALLOW = ROOT / "tools/line_limit_allow.json"
LIMIT = 500
SCANNED = ("src/", "tools/", "docs/spike-rocm/", "shaders/")
SKIPPED_SUFFIXES = {".md", ".json", ".txt", ".csv"}


def tracked():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True)
    for name in out.stdout.splitlines():
        root_script = "/" not in name and name.endswith(".ps1")
        if not (name.startswith(SCANNED) or root_script):
            continue
        if Path(name).suffix.lower() in SKIPPED_SUFFIXES:
            continue
        data = (ROOT / name).read_bytes()
        if b"\0" in data:
            continue  # a binary, such as the carved .co kernels under docs/spike-rocm/
        yield name, data.count(b"\n") + (0 if data.endswith(b"\n") or not data else 1)


def main():
    allow = json.loads(ALLOW.read_text(encoding="utf-8"))
    lengths = dict(tracked())
    failures = []
    for name, lines in sorted(lengths.items()):
        if name in allow:
            if lines > allow[name]:
                failures.append(f"{name}: {lines} lines, grew past the {allow[name]} it is "
                                f"allowed in {ALLOW.name}; it may only shrink")
        elif lines > LIMIT:
            failures.append(f"{name}: {lines} lines, over the limit of {LIMIT}")
    for name, recorded in sorted(allow.items()):
        if name not in lengths:
            failures.append(f"{name}: listed in {ALLOW.name} but no longer exists; delete the entry")
        elif lengths[name] <= LIMIT:
            failures.append(f"{name}: {lengths[name]} lines, now within the limit of {LIMIT}; "
                            f"delete its entry from {ALLOW.name}")
    for f in failures:
        print(f"FAIL {f}")
    if failures:
        return 1
    print(f"OK: {len(lengths)} files within {LIMIT} lines, {len(allow)} grandfathered and not grown")
    return 0


if __name__ == "__main__":
    sys.exit(main())
