"""Check the add-on's hardcoded runtime offsets against the runtime binary itself.

Every address in `core/addon/runtime_offsets.h` is a raw write into someone else's DLL, and a wrong
one does not fail politely -- it writes into read-only memory, or calls into the middle of an
unrelated function. The only thing standing between a bad port and that used to be reading the
disassembly carefully. This is that reading, written down so it runs.

    python tools/runtime_offsets_check.py <dlssnr_amd_pass1.dll>
    python tools/runtime_offsets_check.py            # sources only, no binary needed -- what CI runs

What it proves, in the order of how much each would have caught:

  * **No source file holds a runtime offset of its own.** The offsets were once copied into four
    files; the move to v0.3.0 updated two, and the two that kept v0.2.17's job counter crashed the
    32-bit bridge on its first frame and left the same crash unfired in the Vulkan route. Both
    passed a build and a full test suite. Only the header may name an address now.
  * The record entry's first three tests -- the gate every evaluation goes through -- resolve to
    three addresses the header names. Those are Enabled, the native-failure byte and the ready
    byte, decoded out of the instruction stream rather than assumed.
  * Every data offset lands in `.data` and every entry point in `.text`. `.rdata` is where a stale
    data offset goes to fault, so this is not a formality.
  * The file is the build `kRuntimeSha256` names, and it has been through `patch_runtime.py`.

No binaries ship with this project: the DLL is yours.
"""

import hashlib
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "core/addon/runtime_offsets.h"
SOURCES = ROOT / "src"


def sections(data):
    """(name, virtual address, virtual size, raw pointer) per PE section."""
    e = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e:e + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    count = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    table = e + 24 + opt
    out = []
    for i in range(count):
        off = table + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("latin1")
        vsize, va, _, raw = struct.unpack_from("<IIII", data, off + 8)
        out.append((name, va, vsize, raw))
    return out


def header_offsets():
    """{name: rva} from the header, split into data and entry points by the `Fn` suffix."""
    text = HEADER.read_text(encoding="utf-8")
    found = dict(re.findall(r"(k\w+)\s*=\s*0x([0-9a-fA-F]+)", text))
    if not found:
        raise ValueError(f"no offsets in {HEADER}")
    data, code = {}, {}
    for name, value in found.items():
        (code if name.endswith("Fn") else data)[name] = int(value, 16)
    return data, code


def stray_literals():
    """Any source file that still writes an address itself instead of naming one.

    This is the check the crash asked for. The two forms that matter are the ones that reach the
    runtime: `At<T>(module, 0x...)` and `reinterpret_cast<...>(reinterpret_cast<uintptr_t>(module)
    + 0x...)`. A number anywhere else in these files is not an offset and is left alone.
    """
    at = re.compile(r"At<[^>]+>\([^,]+,\s*(0x[0-9a-fA-F]+)\s*\)")
    plus = re.compile(r"reinterpret_cast<uintptr_t>\([^)]+\)\s*\+\s*(0x[0-9a-fA-F]+)")
    out = []
    for path in sorted(SOURCES.rglob("*")):
        if path.suffix.lower() not in (".cpp", ".h", ".hpp", ".inc", ".c", ".cc") or path == HEADER:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for m in at.findall(line) + plus.findall(line):
                out.append((path.relative_to(ROOT).as_posix(), number, m))
    return out


def rip_target(data, delta, rva, length):
    """Where a rip-relative instruction at `rva` points. `length` is the whole instruction."""
    disp = struct.unpack_from("<i", data, rva - delta + length - 5)[0]
    return rva + length + disp


def main(argv):
    if len(argv) > 2:
        print(__doc__)
        return 2

    bad = []
    def check(ok, said):
        print(("  ok   " if ok else "  FAIL ") + said)
        if not ok:
            bad.append(said)

    # -- The one that would have caught the crash ------------------------------------------------
    # First, and on its own when no binary is given: it needs nothing but the tree, which is what
    # lets CI run it. No binary ships with this project, so everything below it cannot run there --
    # and this is the check that matters most, because the crash it catches survived a clean build,
    # a green suite and two code reviews.
    print(f"{HEADER.relative_to(ROOT).as_posix()} and every source under core/\n")
    stray = stray_literals()
    check(not stray, f"no source file writes an offset of its own ({len(stray)} found)")
    for path, number, value in stray:
        print(f"         {path}:{number} writes {value} -- name it in runtime_offsets.h instead")

    if len(argv) == 1:
        print("\n" + ("PASS (sources only; pass the runtime to check it too)" if not bad
                      else f"FAIL: {len(bad)} check(s)"))
        return 0 if not bad else 1

    dll = Path(argv[1])
    raw = dll.read_bytes()
    print(f"\n{dll}\n")

    # -- The file is the build these offsets belong to -------------------------------------------
    source = (ROOT / "core/addon/neural.cpp").read_text(encoding="utf-8", errors="replace")
    digest = re.search(r"kRuntimeSha256\[32\]\s*=\s*\{(.*?)\}", source, re.S)
    size = re.search(r"kRuntimeSize\s*=\s*(\d+)", source)
    want_sha = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", digest.group(1))).hex()
    want_size = int(size.group(1))

    check(len(raw) == want_size, f"size {len(raw)} == kRuntimeSize {want_size}")
    got = hashlib.sha256(raw).hexdigest()
    check(got == want_sha, f"sha256 {got[:16]}... == kRuntimeSha256 {want_sha[:16]}...")
    if got != want_sha or len(raw) != want_size:
        print("\nthis is not the build these offsets belong to; nothing below would mean anything")
        return 1

    data_off, code_off = header_offsets()
    secs = {name: (va, vsize, rawp) for name, va, vsize, rawp in sections(raw)}
    text_va, text_vsize, text_raw = secs[".text"]
    data_va, data_vsize, _ = secs[".data"]
    delta = text_va - text_raw

    astray = sorted((n, v) for n, v in data_off.items() if not data_va <= v < data_va + data_vsize)
    check(not astray, f"{len(data_off)} data offsets inside .data "
                      f"[{data_va:#x}..{data_va + data_vsize:#x})"
          + ("" if not astray else "  -- outside: " + ", ".join(f"{n}={v:#x}" for n, v in astray)))

    off_text = sorted((n, v) for n, v in code_off.items() if not text_va <= v < text_va + text_vsize)
    check(not off_text, f"{len(code_off)} entry points inside .text "
                        f"[{text_va:#x}..{text_va + text_vsize:#x})"
          + ("" if not off_text else "  -- outside: " + ", ".join(f"{n}={v:#x}" for n, v in off_text)))

    # -- The gate, decoded out of the record entry -----------------------------------------------
    record = code_off["kRecordFn"]
    gate, at = [], record
    for opcode, length in ((b"\x80\x3d", 7), (b"\xf6\x05", 7), (b"\xf6\x05", 7)):
        found = raw.find(opcode, at - delta, at - delta + 0x80)
        if found < 0:
            break
        at = found + delta
        gate.append(rip_target(raw, delta, at, length))
        at += length
    check(len(gate) == 3, f"record entry {record:#x}: decoded {len(gate)} of 3 opening tests")
    for want, rva in zip(("kEnabled", "kNativeFailure", "kReady"), gate):
        check(data_off.get(want) == rva,
              f"record entry gates on {rva:#x}, which the header calls {want} "
              f"({data_off.get(want, 0):#x})")

    # -- And the DLL went through patch_runtime.py -----------------------------------------------
    spec = ROOT / "tools/runtime-patches.json"
    for change in json.loads(spec.read_text())["changes"]:
        off, after = int(change["offset"], 16), bytes.fromhex(change["after"])
        check(raw[off:off + len(after)] == after, f"patch at {change['offset']} applied")

    print("\n" + ("PASS" if not bad else f"FAIL: {len(bad)} check(s)"))
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
