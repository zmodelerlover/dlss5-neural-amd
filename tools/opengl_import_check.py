"""Check that nothing drags opengl32.dll into the add-on's import table.

The OpenGL route resolves `opengl32.dll` by hand -- `GetModuleHandleW`, then `LoadLibraryW` only if
the process does not already have it -- and calls every entry point through a function pointer. That
is not a style choice. A static import loads OpenGL into every D3D11 and D3D12 game the add-on is
placed in, and `neural.cpp` records what one unwanted import already did to NFS 2015's
`ResizeBuffers`. One `#pragma comment(lib, "opengl32")` undoes it silently: the build stays green,
every test passes, and the damage only shows up in somebody else's game.

    python tools/opengl_import_check.py                             # sources only, no toolchain needed
    python tools/opengl_import_check.py build/amd-nr.addon64  # and the built import table

What it proves:

  * **No source calls a `gl`/`wgl` entry point by name.** Every one must go through the loader's
    function pointers, because a bare call is a linked import. Comments and string literals are
    stripped first, so the loader's own `FromModule("glGetError", ...)` and the route's
    explanatory comments do not trip it.
  * **No `#pragma comment(lib, ...)` names opengl32**, in any source under `src/`.
  * **The build's link line does not name `opengl32.lib`.** One list of libraries serves every
    target in `build.ps1`, so adding it for `glinfo` or `glprobe` would put it in the add-on too.
  * Given a binary: **`opengl32.dll` appears in neither its import table nor its delay-load table**,
    while `kernel32.dll` does appear in the first. That last one is a control row: a parser that
    found nothing at all would otherwise pass this check by failing to read the file. It is
    deliberately not a graphics DLL -- the add-on resolves D3D12 by hand as well, so the table is
    shorter than the link line suggests.
"""

import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ROOT / "src"
BUILD = ROOT / "build.ps1"
SUFFIXES = (".cpp", ".h", ".hpp", ".inc", ".c", ".cc")

CALL = re.compile(r"(?<![A-Za-z0-9_])((?:w?gl)[A-Z][A-Za-z0-9_]*)\s*\(")
PRAGMA_LIB = re.compile(r'#\s*pragma\s+comment\s*\(\s*lib\s*,\s*"([^"]*)"', re.I)


def code_only(text):
    """`text` with comments and string/char literals blanked, newlines kept so lines still count.

    Written out rather than regexed because the route's comments contain the very call syntax this
    is looking for -- `-> glImportMemoryWin32HandleEXT(...)` -- and a URL inside a string contains
    the opening of a line comment.
    """
    out = []
    i, n = 0, len(text)
    quotes = '"' + "'"
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif two == "/*":
            while i < n and text[i:i + 2] != "*/":
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
        elif c in quotes:
            out.append(" ")
            i += 1
            while i < n and text[i] != c:
                if text[i] == "\\":
                    out.append(" ")
                    i += 1
                if i < n:
                    out.append("\n" if text[i] == "\n" else " ")
                    i += 1
            out.append(" ")
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def sources():
    for path in sorted(SOURCES.rglob("*")):
        if path.suffix.lower() in SUFFIXES:
            yield path


def bare_calls():
    """(file, line, name) for every `gl`/`wgl` entry point called by name in real code."""
    out = []
    for path in sources():
        text = code_only(path.read_text(encoding="utf-8", errors="replace"))
        for number, line in enumerate(text.splitlines(), 1):
            for name in CALL.findall(line):
                out.append((path.relative_to(ROOT).as_posix(), number, name))
    return out


def pragma_libs():
    """(file, line, library) for every `#pragma comment(lib, ...)` naming opengl32."""
    out = []
    for path in sources():
        for number, line in enumerate(
                path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for lib in PRAGMA_LIB.findall(line):
                if "opengl32" in lib.lower():
                    out.append((path.relative_to(ROOT).as_posix(), number, lib))
    return out


def build_line_libs():
    """The libraries `build.ps1` links, read off its link line."""
    text = BUILD.read_text(encoding="utf-8", errors="replace")
    return sorted(set(m.lower() for m in re.findall(r"([A-Za-z0-9_]+\.lib)", text)))


def imports(data):
    """(imported DLL names, delay-loaded DLL names) out of a PE's data directories."""
    e = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e:e + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    count = struct.unpack_from("<H", data, e + 6)[0]
    opt = e + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic not in (0x10B, 0x20B):
        raise ValueError(f"unknown optional header magic {magic:#x}")
    directories = opt + (0x60 if magic == 0x10B else 0x70)

    secs = []
    table = opt + struct.unpack_from("<H", data, e + 20)[0]
    for i in range(count):
        off = table + i * 40
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, off + 8)
        secs.append((va, max(vsize, rawsize), raw))

    def at(rva):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return raw + (rva - va)
        raise ValueError(f"rva {rva:#x} is in no section")

    def name(rva):
        off = at(rva)
        return data[off:data.index(b"\0", off)].decode("latin1").lower()

    def walk(index, stride, name_field):
        rva, size = struct.unpack_from("<II", data, directories + index * 8)
        if rva == 0 or size == 0:
            return []
        out, off = [], at(rva)
        while True:
            entry = data[off:off + stride]
            if len(entry) < stride or not any(entry):
                return out
            field = struct.unpack_from("<I", data, off + name_field)[0]
            if field == 0:
                return out
            out.append(name(field))
            off += stride

    # Directory 1 is the import table, 13 the delay-load one. A delay-loaded opengl32 would not
    # load OpenGL at process start, but it is still a link-time dependency this route does not
    # have, and finding one would mean somebody linked the import library after all.
    return walk(1, 20, 12), walk(13, 32, 4)


def main(argv):
    if len(argv) > 2:
        print(__doc__)
        return 2

    bad = []

    def check(ok, said):
        print(("  ok   " if ok else "  FAIL ") + said)
        if not ok:
            bad.append(said)

    print("every source under src/, and build.ps1\n")

    calls = bare_calls()
    check(not calls, f"no source calls a gl/wgl entry point by name ({len(calls)} found)")
    for path, number, called in calls:
        print(f"         {path}:{number} calls {called} -- resolve it through the loader instead")

    pragmas = pragma_libs()
    check(not pragmas, f"no #pragma comment(lib) names opengl32 ({len(pragmas)} found)")
    for path, number, lib in pragmas:
        print(f"         {path}:{number} links {lib}")

    libs = build_line_libs()
    check("opengl32.lib" not in libs, f"build.ps1 links {' '.join(libs)}, and not opengl32.lib")

    if len(argv) == 1:
        print("\n" + ("PASS (sources only; pass the add-on to check its import table too)"
                      if not bad else f"FAIL: {len(bad)} check(s)"))
        return 0 if not bad else 1

    binary = Path(argv[1])
    print(f"\n{binary}\n")
    static, delayed = imports(binary.read_bytes())

    check("kernel32.dll" in static, f"the import table reads: {', '.join(static) or '(nothing)'}")
    check("opengl32.dll" not in static, "opengl32.dll is not imported")
    check("opengl32.dll" not in delayed,
          "opengl32.dll is not delay-loaded either"
          + ("" if not delayed else f" (delay-loaded: {', '.join(delayed)})"))

    print("\n" + ("PASS" if not bad else f"FAIL: {len(bad)} check(s)"))
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
