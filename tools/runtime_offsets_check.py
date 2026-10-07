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
  * The ini reader takes Quality with a default of "fast", compares it with "fast" and stores the
    answer to kQuality, the byte the add-on writes its own Quality into; a build that names 0 there
    has no Quality key in it at all.
  * The packet the add-on hands Record (`struct Packet`, neural.cpp) holds every float Record reads
    off it. 0.6.0 reads FSR's pre-exposure at +0x60, one past the 0x60 bytes the packet had; with a
    binary, the record entry's `movss xmm, [packet+0x60]` reads are counted, and the packet must
    reach past them.
  * The file is a build `kBuilds` names, by its hash, and it has been through `patch_runtime.py`.
    Without a binary: every build names every address (a field left out compiles as zero) but the
    ones `struct Build` declares optional, and each is the patched_sha256 of a runtime-patches.json
    build.
  * The runtime names d3d12.dll in a table the add-on rewrites. The private-D3D12 copy renames it
    in the import and delay-import tables; v0.4.0 moved it from the first to the second while the
    add-on read only the first, and a runtime it cannot rename loads the system d3d12.dll -- the
    resize crash that copy exists to prevent, back with nothing failing on the way.

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
BUILDS = ROOT / "core/addon/runtime_builds.inc"  # kBuilds, included by HEADER
SOURCES = ROOT / "core"
NEURAL = ROOT / "core/addon/neural.cpp"  # struct Packet and its static_assert


def packet_layout():
    """(sizeof(Packet), offsetof(Packet, preExposure) or -1), off the static_assert under it."""
    text = NEURAL.read_text(encoding="utf-8")
    size = re.search(r"sizeof\(Packet\)\s*==\s*(0x[0-9a-fA-F]+)", text)
    pre = re.search(r"offsetof\(Packet,\s*preExposure\)\s*==\s*(0x[0-9a-fA-F]+)", text)
    return (int(size.group(1), 16) if size else 0), (int(pre.group(1), 16) if pre else -1)


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


def import_names(data):
    """(table, dll) per descriptor of the import table (directory 1) and the delay-import table
    (directory 13, RVA form only) -- the two RuntimeCopyUsingPrivateD3D12 rewrites."""
    e = struct.unpack_from("<I", data, 0x3C)[0]
    opt = e + 24
    dirs = opt + (112 if struct.unpack_from("<H", data, opt)[0] == 0x20B else 96)
    count = struct.unpack_from("<I", data, dirs - 4)[0]
    secs = sections(data)

    def at(rva):
        for _, va, vsize, raw in secs:
            if va <= rva < va + vsize:
                return raw + rva - va
        return None

    out = []
    for table, index, size, field in (("import", 1, 20, 12), ("delay-import", 13, 32, 4)):
        desc = at(struct.unpack_from("<I", data, dirs + index * 8)[0]) if index < count else None
        while desc is not None and struct.unpack_from("<I", data, desc + field)[0] != 0:
            if table == "import" or struct.unpack_from("<I", data, desc)[0] & 1:
                name = at(struct.unpack_from("<I", data, desc + field)[0])
                if name is not None:
                    out.append((table, data[name:data.index(b"\0", name)].decode("latin1")))
            desc += size
    return out


def header_builds():
    """The address fields `struct Build` declares, the ones among them it declares optional (the
    word in the comment on the declaration's line: a build without the key names 0), and every
    entry of `kBuilds`: its version, size, SHA-256 and {name: rva}, the rvas split into data and
    entry points by the `Fn` suffix."""
    text = HEADER.read_text(encoding="utf-8") + "\n" + BUILDS.read_text(encoding="utf-8")
    struct_body = re.search(r"struct Build\s*\{(.*?)\n\};", text, re.S).group(1)
    decls = re.findall(r"^size_t ([^;]+);(.*)$", struct_body, re.M)
    fields = [n for decl, _ in decls for n in re.findall(r"k\w+", decl) if n != "kSize"]
    optional = {n for decl, comment in decls if re.search(r"//.*\boptional\b", comment)
                for n in re.findall(r"k\w+", decl)}
    table = re.search(r"kBuilds\[\]\s*=\s*\{(.*?)\n\};", text, re.S).group(1)
    builds = []
    for entry in re.split(r"\n    \{", table)[1:]:
        values = dict(re.findall(r"\.(k\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+|\"[^\"]*\")", entry))
        data, code = {}, {}
        for name, value in values.items():
            if name.startswith(("kVersion", "kSize", "kSha256")):
                continue
            (code if name.endswith("Fn") else data)[name] = int(value, 16)
        builds.append({"version": values["kVersion"].strip('"'), "size": int(values["kSize"]),
                       "sha256": values["kSha256"].strip('"'), "data": data, "code": code})
    if not fields or not builds:
        raise ValueError(f"no struct Build or kBuilds in {HEADER}")
    return fields, optional, builds


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
        if path.suffix.lower() not in (".cpp", ".h", ".hpp", ".inc", ".c", ".cc") or path in (HEADER, BUILDS):
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

    # A field a build leaves out of its designated initializer compiles, as zero: an address at the
    # module's base. Every build names every field, and each is a build runtime-patches.json makes.
    # An optional field may be 0, for a build without it; with the binary, a check below proves it.
    fields, optional, builds = header_builds()
    spec = json.loads((ROOT / "tools/runtime-patches.json").read_text(encoding="utf-8"))
    patched = {b["patched_sha256"]: b for b in spec["builds"]}
    for build in builds:
        named = {**build["data"], **build["code"]}
        missing = [f for f in fields if not named.get(f) and f not in optional]
        lacks = [f for f in fields if not named.get(f) and f in optional]
        check(not missing, f"build {build['version']} names all {len(fields)} addresses"
                           + (f" (optional, 0 on this build: {', '.join(lacks)})" if lacks else "")
                           + ("" if not missing else "  -- missing or zero: " + ", ".join(missing)))
        check(build["sha256"] in patched, f"build {build['version']}'s kSha256 is the patched_sha256 of a "
                                          "runtime-patches.json build")
    check(len({b["sha256"] for b in builds}) == len(builds), f"{len(builds)} builds, no two with one hash")

    # Record reads pre-exposure at +0x60 on 0.6.0 (UsePreExposure=1 by default); a packet that ends
    # there hands it stack bytes, which the OptiScaler fork's 0x60-byte packet turned into green noise.
    packet_size, pre_exposure = packet_layout()
    check(pre_exposure == 0x60 and packet_size >= 0x64,
          f"struct Packet is {packet_size:#x} bytes with preExposure at {pre_exposure:#x} "
          "(Record reads +0x60 since 0.6.0)")

    if len(argv) == 1:
        print("\n" + ("PASS (sources only; pass the runtime to check it too)" if not bad
                      else f"FAIL: {len(bad)} check(s)"))
        return 0 if not bad else 1

    dll = Path(argv[1])
    raw = dll.read_bytes()
    print(f"\n{dll}\n")

    # -- The file is a build the header names, and these are its offsets -------------------------
    got = hashlib.sha256(raw).hexdigest()
    build = next((b for b in builds if b["sha256"] == got), None)
    check(build is not None, f"sha256 {got[:16]}... is a build the header names ("
                             + ", ".join(f"{b['version']} {b['sha256'][:16]}..." for b in builds) + ")")
    if build is None:
        print("\nthis is not a build these offsets belong to; nothing below would mean anything")
        return 1
    check(len(raw) == build["size"], f"size {len(raw)} == kSize {build['size']} of {build['version']}")
    print(f"\n  build {build['version']}\n")

    # A 0 left here is an optional field this build lacks: there is nothing of it to find in .data.
    data_off = {n: v for n, v in build["data"].items() if v or n not in optional}
    code_off = build["code"]
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

    # -- The packet reaches every float Record reads at +0x60 -------------------------------------
    # The record entry keeps the packet in the register it moves rcx into (`mov rsi, rcx` on 0.6.0);
    # `movss xmmN, [that+0x60]` is the pre-exposure read, twice on 0.6.0 (0x1a916, 0x1aef8).
    entry = raw[record - delta:record - delta + 0x80]
    keep = re.search(rb"\x48\x89([\xc8-\xcf])", entry)
    reads = 0
    if keep:
        rm = keep.group(1)[0] - 0xc8
        modrm = bytes(0x40 | (n << 3) | rm for n in range(8))
        reads = len(re.findall(rb"\xf3\x0f\x10[" + re.escape(modrm) + rb"]\x60",
                               raw[record - delta:record - delta + 0x4000]))
    check(bool(keep), f"record entry {record:#x}: found the register it keeps the packet in")
    check(reads == 0 or packet_size >= 0x64,
          f"Record reads a float at packet+0x60 {reads} time(s); the packet is {packet_size:#x} bytes")

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

    # -- The engine's latched mode, and the seed the pre-block is handed ---------------------------
    # The record entry starts its watchdog only when the latched byte reads 1 (`cmp byte [rip+..],
    # 1`, 0x1783d on v0.4.1), the byte after kInlineMode; and zeroes the frame counter right after
    # the warm-up job (`mov dword [rip+..], 0`, 0x172c6), just ahead of `mov byte [kHistoryOn], 0`.
    rec = raw[record - delta:record - delta + 0x4000]
    gates = {rip_target(raw, delta, record + m.start(), 7)
             for m in re.finditer(rb"\x80\x3d.{4}\x01", rec, re.S)}
    check(data_off.get("kInlineActive") in gates and
          data_off.get("kInlineActive") == data_off.get("kInlineMode", -2) + 1,
          f"record entry tests {data_off.get('kInlineActive', 0):#x} for 1 before its watchdog, the "
          f"byte after kInlineMode, which the header calls kInlineActive")
    seeds = [rip_target(raw, delta, record + m.start(), 10, 4)
             for m in re.finditer(rb"\xc7\x05.{4}\x00\x00\x00\x00", rec, re.S)
             for c in [rec.find(b"\xc6\x05", m.start() + 10, m.start() + 0x20)]
             if c >= 0 and rec[c + 6] == 0
             and rip_target(raw, delta, record + c, 7) == data_off.get("kHistoryOn")]
    check(seeds == [data_off.get("kFrameCounter")],
          f"record entry zeroes {', '.join(f'{v:#x}' for v in seeds) or 'nothing found'} after the "
          f"warm-up, which the header calls kFrameCounter ({data_off.get('kFrameCounter', 0):#x})")
    # The worker compares the two engine fields: `mov eax, [rsi+counter]` / `cmp eax, [rsi+check]`.
    counter = data_off.get("kFrameCounter", 0) - data_off.get("kEngineObject", 0)
    selfcheck = data_off.get("kSelfCheckFrame", 0) - data_off.get("kEngineObject", 0)
    # Through any base register: rsi up to v0.4.2, r10 on 0.5.0 (`41 8b 42 4c 41 3b 82 ..`).
    pattern = (rb"(?:\x41)?\x8b[\x40-\x47]" + re.escape(bytes([counter]))
               + rb"(?:\x41)?\x3b[\x80-\x87]" + re.escape(struct.pack("<I", selfcheck))
               if 0 <= counter < 0x80 and selfcheck > 0 else b"")
    check(bool(pattern) and re.search(pattern, raw[text_raw:text_raw + text_vsize]) is not None,
          f"the worker compares engine+{counter:#x} with engine+{selfcheck:#x}, which the header "
          f"calls kFrameCounter and kSelfCheckFrame")

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

    # CpuWait, which the add-on pins to 0 so the notify entry never blocks the presenting thread:
    # `lea rdx, "CpuWait"` in the same reader, and the first `mov [rip+..], eax` after it.
    key = raw.find(b"CpuWait\0")
    key_rva = key - rdata_raw + rdata_va if key >= 0 else -1
    reader = [m.start() for m in re.finditer(rb"\x48\x8d\x15", text)
              if rip_target(raw, delta, text_va + m.start(), 7, 0) == key_rva]
    window = text[reader[0]:reader[0] + 0x40] if len(reader) == 1 else b""
    store = window.find(b"\x89\x05")
    cpu_wait = rip_target(raw, delta, text_va + reader[0] + store, 6, 0) if window and store >= 0 else -1
    check(cpu_wait == data_off.get("kCpuWait"),
          f"CpuWait is stored to {cpu_wait:#x}, which the header calls kCpuWait "
          f"({data_off.get('kCpuWait', 0):#x})")

    # Quality, which the add-on writes from its own setting. The reader: `lea rdx, "Quality"`, `lea
    # r8, "fast"` (the default), `lea r9, [rbp+buf]`, `call [GetPrivateProfileStringA]`, `lea rdx,
    # "fast"`, `lea rcx, [rbp+buf]`, `call _stricmp`, `test eax, eax`, `sete byte [rip+..]`: 1 for
    # fast in any case, or no key, and 0 for anything else. The same nine instructions on v0.4.2
    # (0x89ac) and 0.5.0 (0x8af4); the overlay's own `lea rdx, "Quality"`, its write-back, is not
    # followed by them. A build that names 0 must have no Quality key anywhere in the file.
    quality = data_off.get("kQuality", 0)
    key = raw.find(b"Quality\0")
    if not quality:
        check(key < 0, "the runtime has no Quality key, as its kQuality of 0 says"
                       + ("" if key < 0 else f"  -- it has one, at file offset {key:#x}"))
    else:
        key_rva = key - rdata_raw + rdata_va if key >= 0 else -1
        shape = re.compile(rb"\x48\x8d\x15.{4}\x4c\x8d\x05.{4}\x4c\x8d\x4d(.)\xff\x15.{4}"
                           rb"\x48\x8d\x15.{4}\x48\x8d\x4d\1\xe8.{4}\x85\xc0\x0f\x94\x05.{4}", re.S)

        def string_at(rva):
            off = rva - rdata_va + rdata_raw
            return raw[off:raw.find(b"\0", off)] if rdata_raw <= off < len(raw) else b""

        stores = [rip_target(raw, delta, at + 42, 7, 0)
                  for m in re.finditer(rb"\x48\x8d\x15", text)
                  for at in [text_va + m.start()]
                  if rip_target(raw, delta, at, 7, 0) == key_rva and shape.match(text, m.start())
                  and string_at(rip_target(raw, delta, at + 7, 7, 0)) == b"fast"
                  and string_at(rip_target(raw, delta, at + 24, 7, 0)) == b"fast"]
        check(stores == [quality],
              f"Quality is read with a default of \"fast\", compared with \"fast\" and stored to "
              f"{', '.join(f'{v:#x}' for v in stores) or 'nothing found'}, which the header calls "
              f"kQuality ({quality:#x})")

    # -- And the DLL went through patch_runtime.py -----------------------------------------------
    for change in patched[got]["changes"]:
        off, after = int(change["offset"], 16), bytes.fromhex(change["after"])
        check(raw[off:off + len(after)] == after, f"patch at {change['offset']} applied")

    # -- And d3d12.dll is named where the add-on can rename it -----------------------------------
    tables = sorted({t for t, name in import_names(raw) if name.lower() == "d3d12.dll"})
    check(bool(tables), "d3d12.dll is named where the private copy renames it ("
                        + (", ".join(tables) or "in neither table") + ")")

    print("\n" + ("PASS" if not bad else f"FAIL: {len(bad)} check(s)"))
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
