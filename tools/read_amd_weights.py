"""Read the DLSS-NR-on-AMD weights file, and check its tensors against NVIDIA's DLL.

    python tools/read_amd_weights.py <dlssnr_on_amd_weights.bin> [--nvidia <nvngx_dlssnr.dll>]
    python tools/read_amd_weights.py --check

The format is simple and fully accounted for, which is the point of this file: the parse is
validated by two independent totals, so a wrong reading cannot pass quietly.

    magic   "DLSSNRW1"                       8 bytes
    count   uint32                           number of tensors
    base    uint32                           file offset where the blob data starts
    index   count x { u8 namelen, name, u64 blob_offset, u64 size }
    data    at `base`, blob_offset is relative to it

On the v0.3.0 weights shipped beside the runtime: count 153, base 5673, and both checks hold --
the index ends exactly at `base`, and base + sum(sizes) == the file size, 147,689,451 bytes.

WHY THIS MATTERS, measured 22/09/2026 (docs/spike-rocm/README.md):

    All 153 tensor names appear verbatim inside NVIDIA's nvngx_dlssnr.dll, and a 25-tensor sample
    of the blobs is BYTE-IDENTICAL inside it.

So the AMD port is not a reimplementation that happens to resemble DLSS-NR: it runs NVIDIA's own
network, with NVIDIA's own weights, lifted out of the DLL. That closes a question `docs/nvidia-parity.md`
had to leave open -- any difference in the image between the two machines is NOT the network. It is
the composition around it, the four-vs-five conditioning inputs, and the fp8 kernels' arithmetic.

`--nvidia` re-runs that comparison. It is a plain substring search over a 166 MB file per tensor, so
it samples 25 of them rather than all 144 searchable ones (the other 9 are 2-byte scalars, too short
for a match to mean anything). Point it at your own copy of the DLL; no binaries ship with this
project.
"""
import struct
import sys
from pathlib import Path

MAGIC = b"DLSSNRW1"


def parse(blob):
    """(entries, base) where entries is [(name, blob_offset, size)]. Raises if the file disagrees
    with itself -- the two totals below are what make a misparse loud instead of silent."""
    if blob[:8] != MAGIC:
        raise ValueError("not a DLSSNRW1 weights file (magic %r)" % blob[:8])
    count, base = struct.unpack_from("<II", blob, 8)
    pos, entries = 16, []
    for _ in range(count):
        nlen = blob[pos]
        pos += 1
        name = blob[pos:pos + nlen].decode("ascii", "replace")
        pos += nlen
        off, size = struct.unpack_from("<QQ", blob, pos)
        pos += 16
        entries.append((name, off, size))
    if pos != base:
        raise ValueError("index ends at %d but data_base is %d -- the index layout is wrong" % (pos, base))
    total = base + sum(e[2] for e in entries)
    if total != len(blob):
        raise ValueError("base + sum(sizes) = %d but the file is %d bytes" % (total, len(blob)))
    return entries, base


def main(argv):
    if len(argv) == 2 and argv[1] == "--check":
        # Self-test on a synthetic file, so the parser is exercised without shipping weights.
        names = [b"a.layer", b"bb.layer"]
        idx = b"".join(bytes([len(n)]) + n for n in names)
        # Two entries, sizes 4 and 6; offsets are relative to base.
        idx = (bytes([len(names[0])]) + names[0] + struct.pack("<QQ", 0, 4)
               + bytes([len(names[1])]) + names[1] + struct.pack("<QQ", 4, 6))
        base = 16 + len(idx)
        blob = MAGIC + struct.pack("<II", 2, base) + idx + b"\0" * 10
        ents, b = parse(blob)
        assert b == base and [e[0] for e in ents] == ["a.layer", "bb.layer"], ents
        bad = bytearray(blob)
        bad[8:12] = struct.pack("<I", 3)          # a count the index cannot satisfy
        try:
            parse(bytes(bad))
        except Exception:
            print("PASS: parses, and rejects a file that disagrees with itself")
            return 0
        raise AssertionError("the corrupt-count case was accepted -- the check has no teeth")

    if not 2 <= len(argv) <= 4:
        print(__doc__)
        return 2

    blob = Path(argv[1]).read_bytes()
    entries, base = parse(blob)
    print("%d tensors, data_base %d, file %d bytes -- index and totals both agree"
          % (len(entries), base, len(blob)))
    big = [e for e in entries if e[2] >= 256]
    print("largest:")
    for name, off, size in sorted(entries, key=lambda e: -e[2])[:5]:
        print("  %-34s %10d" % (name, size))

    if "--nvidia" in argv:
        dll = Path(argv[argv.index("--nvidia") + 1]).read_bytes()
        missing = [n for n, _, _ in entries if n.encode() not in dll]
        print("\nnames present verbatim in the NVIDIA DLL: %d / %d"
              % (len(entries) - len(missing), len(entries)))
        if missing:
            print("  absent:", missing[:8])
        sample = big[::max(1, len(big) // 25)][:25]
        hit = 0
        for name, off, size in sample:
            at = dll.find(blob[base + off:base + off + size])
            hit += at >= 0
            print("  %-34s %9d  %s" % (name, size, hex(at) if at >= 0 else "NOT FOUND"))
        print("\n%d/%d sampled blobs are byte-identical inside the NVIDIA DLL" % (hit, len(sample)))
        print("(%d tensors are >=256 bytes and searchable; the other %d are 2-byte scalars)"
              % (len(big), len(entries) - len(big)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
