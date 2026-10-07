"""Check that the built add-on imports no graphics library at all.

Every graphics DLL the add-on uses is resolved by hand at first use: d3d12.dll and d3dcompiler_47.dll
in `LoadGraphicsApi` (neural.cpp), opengl32.dll by the OpenGL loader, dxgi.dll in
`core/shared/dxgi_entry.h`. An import is resolved when ReShade loads the add-on, in the middle of
ReShade's start-up, and twice that has broken a game that had not loaded the library yet: NFS 2015
over d3d12.dll, Dragon Age: Inquisition over dxgi.dll (issue #22, DXGI_ERROR_INVALID_CALL at start,
no log). Nothing in the build notices: one bare call links the import library and every other gate
stays green.

    python tools/import_table_check.py build/amd-nr.addon64

What it proves: the import table names only the system libraries below (case aside), kernel32.dll
among them as the control row -- a parser that read nothing would otherwise pass -- and the
delay-load table is empty, since a delay-loaded graphics DLL is the same import arriving later.
The PE reading is opengl_import_check.py's.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from opengl_import_check import imports  # noqa: E402

ALLOWED = {"kernel32.dll", "user32.dll", "shell32.dll", "ole32.dll", "bcrypt.dll"}


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    static, delayed = imports(Path(argv[1]).read_bytes())
    bad = []

    def check(ok, said):
        print(("  ok   " if ok else "  FAIL ") + said)
        if not ok:
            bad.append(said)

    print(f"{argv[1]}\n")
    check("kernel32.dll" in static, f"the import table reads: {', '.join(static) or '(nothing)'}")
    extra = sorted(set(static) - ALLOWED)
    check(not extra, "nothing but " + ", ".join(sorted(ALLOWED)) + " is imported"
          + ("" if not extra else f" (also: {', '.join(extra)} -- resolve it at first use instead)"))
    check(not delayed, "nothing is delay-loaded" + ("" if not delayed else f" ({', '.join(delayed)})"))
    print("\n" + ("PASS" if not bad else f"FAIL: {len(bad)} check(s)"))
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
