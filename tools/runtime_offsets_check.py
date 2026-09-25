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
  * Its refusal at four jobs in flight subtracts kJobCounter from kJobId, the same two fields the
    add-on's own busy test (runtimes.inc, Outstanding) reads.
  * The watchdog, where it lets a job go, writes kWatchdogJobA and then counts into kWatchdogFires,
    the count NoteWatchdog steps the scale down on, and at the lowest stands the add-on down on;
    and the ini reader takes InlineWaitMs with a default of 200, clamps it to 50..5000 and stores
    it first to kWaitBudgetMax, the value InitEngine logs as the budget in force.
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
SOURCES = ROOT / "core"


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


def rip_target(data, delta, rva, length, imm=1):
    """Where a rip-relative instruction at `rva` points. `length` is the whole instruction, `imm`
    the bytes of immediate after its displacement."""
    disp = struct.unpack_from("<i", data, rva - delta + length - 4 - imm)[0]
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
    # The control row: this pointed at src/ for a while after the tree moved to core/, found
    # nothing, and passed.
    scanned = sum(1 for p in SOURCES.rglob("*") if p.suffix.lower() in (".cpp", ".h", ".inc"))
    check(scanned > 0, f"sources to read ({scanned} under {SOURCES.relative_to(ROOT).as_posix()}/)")
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

    # -- The jobs in flight, decoded where the record entry refuses one --------------------------
    # runtimes.inc's Outstanding is kJobId - kJobCounter. The runtime's own refusal is the same
    # subtraction: eax = kJobId + 1 (the last `mov r11d, [rip+..]` before it), `sub eax,
    # [kJobCounter]`, `cmp eax, 4` (0x15383). If a new build moves either field, busy means nothing.
    window = raw[record - delta:record - delta + 0x4000]
    sub = re.search(rb"\x2b\x05.{4}\x83\xf8\x04", window, re.S)
    load = [m.start() for m in re.finditer(rb"\x44\x8b\x1d", window[:sub.start()])][-1:] if sub else []
    check(bool(sub and load), f"record entry {record:#x}: decoded its in-flight test and job id load")
    if sub and load:
        for want, rva in (("kJobCounter", rip_target(raw, delta, record + sub.start(), 6, 0)),
                          ("kJobId", rip_target(raw, delta, record + load[0], 7, 0))):
            check(data_off.get(want) == rva, f"in-flight test reads {rva:#x}, which the header calls "
                                             f"{want} ({data_off.get(want, 0):#x})")

    # -- The watchdog's count, and the budget it counts against ----------------------------------
    # Its release stores the job id to kWatchdogJobA (`mov [rip+..], r13d`, 0x1b281) and bumps the
    # counter a few instructions on (`inc dword [rip+..]`, 0x1b297). A wrong kWatchdogFires would
    # read a field that never moves, and the stand-down on a run of fires would never come.
    text = raw[text_raw:text_raw + text_vsize]
    fires = [rip_target(raw, delta, text_va + inc, 6, 0)
             for m in re.finditer(rb"\x44\x89\x2d", text)
             if rip_target(raw, delta, text_va + m.start(), 7, 0) == data_off.get("kWatchdogJobA")
             for inc in [text.find(b"\xff\x05", m.start(), m.start() + 0x20)] if inc >= 0]
    check(fires == [data_off.get("kWatchdogFires")],
          f"watchdog release counts into {', '.join(f'{v:#x}' for v in fires) or 'nothing found'}, "
          f"which the header calls kWatchdogFires ({data_off.get('kWatchdogFires', 0):#x})")
    # The ini reader: `lea rdx, "InlineWaitMs"`, `mov r8d, 200`, the call, the clamp to 50 and 5000
    # (`mov ecx, 50` / `mov eax, 5000`), and the first `mov [rip+..], eax` is the budget's ceiling.
    rdata_va, _, rdata_raw = secs[".rdata"]
    key = raw.find(b"InlineWaitMs\0")
    key_rva = key - rdata_raw + rdata_va if key >= 0 else -1
    reader = [m.start() for m in re.finditer(rb"\x48\x8d\x15", text)
              if rip_target(raw, delta, text_va + m.start(), 7, 0) == key_rva]
    window = text[reader[0]:reader[0] + 0x40] if len(reader) == 1 else b""
    default = window.find(b"\x41\xb8")
    store = window.find(b"\x89\x05")
    budget = rip_target(raw, delta, text_va + reader[0] + store, 6, 0) if window and store >= 0 else -1
    check(default >= 0 and window[default + 2:default + 6] == struct.pack("<I", 200)
          and b"\xb9\x32\x00\x00\x00" in window and b"\xb8\x88\x13\x00\x00" in window
          and budget == data_off.get("kWaitBudgetMax"),
          f"InlineWaitMs is read with a default of 200, clamped to 50..5000 and stored to {budget:#x}, "
          f"which the header calls kWaitBudgetMax ({data_off.get('kWaitBudgetMax', 0):#x})")

    # -- And the DLL went through patch_runtime.py -----------------------------------------------
    spec = ROOT / "tools/runtime-patches.json"
    for change in json.loads(spec.read_text())["changes"]:
        off, after = int(change["offset"], 16), bytes.fromhex(change["after"])
        check(raw[off:off + len(after)] == after, f"patch at {change['offset']} applied")

    print("\n" + ("PASS" if not bad else f"FAIL: {len(bad)} check(s)"))
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
