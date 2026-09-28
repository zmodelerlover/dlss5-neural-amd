"""An amd-nr.ini from before v0.7.0 moves to Temporal=2 once, and never again.

MigrateTemporalDefault (core/addon/ini_migrate.inc) is compiled here with g++ against a stub Log and
run on files this script writes, so the rule is run, not read:
  - Temporal=0 and no FixedSeed key (every ini before v0.7.0): Temporal=2, and FixedSeed=1,
    OutputSmooth=0.8 and OutputSmoothLimit=10 written, every other line as it was;
  - run again on that file: nothing changes, and nothing is logged;
  - Temporal=1 or 2 with no FixedSeed key, or no Temporal key at all: left alone;
  - Temporal=0 with a FixedSeed key (the person's choice after v0.7.0): left alone.
And read from the text: EnsureNeuralIni calls it on an ini that exists, before anything reads it.

    python tools/ini_migrate_check.py

It needs a MinGW g++ (windows.h): the one CXX names, else g++ on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
inc = (ROOT / "core/addon/ini_migrate.inc").read_text(encoding="utf-8")
neural = (ROOT / "core/addon/neural.cpp").read_text(encoding="utf-8")
bad = []

m = re.search(r"\nvoid MigrateTemporalDefault\([^)]*\)\s*\{", inc)
body = inc[m.start():inc.index("\n}\n", m.start()) + 3] if m else ""
if not body:
    bad.append("ini_migrate.inc: no MigrateTemporalDefault")

ensure = neural[neural.find("bool EnsureNeuralIni()"):]
ensure = ensure[:ensure.find("\n}\n")]
if not re.search(r"if \(std::filesystem::exists\(ini, ec\)\)\s*\{\s*MigrateTemporalDefault\(ini\);\s*return false;", ensure):
    bad.append("EnsureNeuralIni: an existing ini is not migrated before it returns")

HARNESS = r"""
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
int logged = 0;
void Log(const char *, ...) { ++logged; }
""" + body + r"""
int main(int argc, char **argv) {
    MigrateTemporalDefault(std::filesystem::path(argv[1]));
    std::printf("%d", logged);
    return 0;
}
"""

OLD = "[amd-nr]\r\nStartOn=1\r\nTemporal=0\r\nScale=0.7\r\n"
CASES = [
    # (name, file before, expected Temporal, FixedSeed written, logged)
    ("before v0.7.0, Temporal=0", OLD, "2", True, 1),
    ("before v0.7.0, Temporal=1", OLD.replace("Temporal=0", "Temporal=1"), "1", False, 0),
    ("before v0.7.0, Temporal=2", OLD.replace("Temporal=0", "Temporal=2"), "2", False, 0),
    ("no Temporal key", OLD.replace("Temporal=0\r\n", ""), None, False, 0),
    ("Temporal=0 chosen after v0.7.0", OLD + "FixedSeed=0\r\n", "0", False, 0),
]


def value(text, key):
    found = re.search(rf"^{key}=(.*?)\r?$", text, re.M)
    return found.group(1) if found else None


if body:
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "migrate.cpp", Path(tmp) / "migrate.exe"
        src.write_text(HARNESS, encoding="utf-8")
        cxx = os.environ.get("CXX", "g++")
        try:
            built = subprocess.run([cxx, "-std=c++20", "-O1", "-static", str(src), "-o", str(exe)],
                                   capture_output=True, text=True)
        except FileNotFoundError:
            built = subprocess.CompletedProcess(cxx, 1, "", f"no compiler at {cxx!r}; set CXX to a MinGW g++")
        if built.returncode != 0:
            bad.append("MigrateTemporalDefault did not compile on its own:\n" + built.stderr[-1500:])
        else:
            for name, before, temporal, seeded, logs in CASES:
                ini = Path(tmp) / "amd-nr.ini"
                ini.write_bytes(before.encode("ascii"))
                ran = subprocess.run([str(exe), str(ini)], capture_output=True, text=True, timeout=30)
                after = ini.read_bytes().decode("latin1")
                if value(after, "Temporal") != temporal:
                    bad.append(f"{name}: Temporal reads {value(after, 'Temporal')!r}, not {temporal!r}")
                if seeded != (value(after, "FixedSeed") == "1" and value(after, "OutputSmooth") == "0.8"
                              and value(after, "OutputSmoothLimit") == "10"):
                    bad.append(f"{name}: the three keys are {'not ' if seeded else ''}written")
                if ran.stdout.strip() != str(logs):
                    bad.append(f"{name}: logged {ran.stdout.strip()} line(s), not {logs}")
                for line in ("StartOn=1", "Scale=0.7"):
                    if line not in after:
                        bad.append(f"{name}: {line} did not survive")
                if logs:
                    again = subprocess.run([str(exe), str(ini)], capture_output=True, text=True, timeout=30)
                    if ini.read_bytes().decode("latin1") != after or again.stdout.strip() != "0":
                        bad.append(f"{name}: a second run changed the file or logged again")

if bad:
    print("FAIL")
    for b in bad:
        print("  " + b)
    sys.exit(1)
print("PASS an ini from before v0.7.0 at Temporal=0 moves to 2 once, with the three keys written; "
      "a second run, Temporal 1 or 2, no Temporal key and a later Temporal=0 are left alone")
