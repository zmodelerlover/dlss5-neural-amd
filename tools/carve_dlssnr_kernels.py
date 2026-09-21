"""Pull the CUDA kernels out of nvngx_dlssnr.dll, so a question about what NVIDIA's code does can
be answered by reading it instead of by guessing.

    python tools/carve_dlssnr_kernels.py <nvngx_dlssnr.dll | carved_*.fatbin> <outdir>
    python tools/carve_dlssnr_kernels.py --check

The DLL embeds 15 CUDA fatbinaries. Each fatbin entry is a cubin (or PTX) compressed with zstd --
that is why a plain strings/grep over the DLL finds no kernel names and why the first look at this
concluded there was nothing readable in there. Decompressed, the kernels are named, and
`cg2r_post_process_kernel` is the one that consumes the fourteen DLSSNR.Style floats.

Needs `zstandard` (pip install zstandard). Disassembling the result needs nvdisasm, which is not a
dependency of this script -- it only produces the .cubin files.

Reading the parameter layout out of a cubin afterwards:

    nvdisasm -c cg2r_post_process_kernel.cubin
    # EIATTR_PARAM_CBANK gives the constant-bank base; struct byte N is c[0x0][base + N].
"""
import struct
import sys
from pathlib import Path

FATBIN_MAGIC = 0xBA55ED50
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
ELF_MAGIC = b"\x7fELF"


def entries(blob, base=0):
    """Walk one fatbinary's entries. Yields (arch, kind, payload) with payload decompressed."""
    magic, _ver, hsz, fatsz = struct.unpack_from("<IHHQ", blob, base)
    if magic != FATBIN_MAGIC:
        raise ValueError(f"not a fatbin at {base:#x}")
    off, end = base + hsz, base + hsz + fatsz
    while off < end:
        kind, _u, ehsz = struct.unpack_from("<HHI", blob, off)
        size, csize = struct.unpack_from("<QI", blob, off + 8)
        arch = struct.unpack_from("<I", blob, off + 28)[0]
        dsize = struct.unpack_from("<Q", blob, off + 56)[0]
        payload = blob[off + ehsz: off + ehsz + size]
        if payload[:4] == ZSTD_MAGIC:
            import zstandard
            # csize, not size: size is padded, and feeding the padding to zstd is an error.
            payload = zstandard.ZstdDecompressor().decompress(
                payload[:csize], max_output_size=max(dsize, 1) * 4)
        yield arch, kind, payload
        off += ehsz + size


def kernel_names(cubin):
    """The .text.<name> sections of a cubin, which is where the kernel names live."""
    shoff, = struct.unpack_from("<Q", cubin, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", cubin, 0x3A)
    stroff, = struct.unpack_from("<Q", cubin, shoff + shstrndx * shentsize + 0x18)
    out = []
    for i in range(shnum):
        nameoff, = struct.unpack_from("<I", cubin, shoff + i * shentsize)
        end = cubin.index(b"\0", stroff + nameoff)
        nm = cubin[stroff + nameoff:end].decode("ascii", "replace")
        if nm.startswith(".text."):
            out.append(nm[6:])
    return out


def carve(path: Path, outdir: Path):
    blob = path.read_bytes()
    outdir.mkdir(parents=True, exist_ok=True)
    # A .fatbin carved out already starts with the magic; a DLL has them scattered through it.
    starts, at = [], blob.find(struct.pack("<I", FATBIN_MAGIC))
    while at != -1:
        starts.append(at)
        at = blob.find(struct.pack("<I", FATBIN_MAGIC), at + 4)
    if not starts:
        raise SystemExit(f"{path}: no fatbinary magic found.")

    total = 0
    for s in starts:
        try:
            got = list(entries(blob, s))
        except Exception as e:                      # a magic that was not really a header
            print(f"  {s:#010x}: skipped ({e})")
            continue
        for n, (arch, kind, payload) in enumerate(got):
            if payload[:4] == ELF_MAGIC:
                names = kernel_names(payload)
                stem = names[0] if len(names) == 1 else f"{s:08x}_{n:03d}"
                out = outdir / f"{stem}_sm{arch}.cubin"
                out.write_bytes(payload)
                total += 1
                if len(names) == 1:
                    print(f"  sm_{arch:<4} {out.name}")
                else:
                    print(f"  sm_{arch:<4} {out.name}  ({len(names)} kernels)")
            elif b".target sm_" in payload[:400]:
                (outdir / f"{s:08x}_{n:03d}_sm{arch}.ptx").write_bytes(payload)
                total += 1
    print(f"{total} files written to {outdir}")


def _entry(payload, arch, kind=2, csize=0, dsize=0):
    """One 64-byte fatBinaryEntry header, fields placed where the walker reads them."""
    h = bytearray(64)
    struct.pack_into("<HHI", h, 0, kind, 0x0101, 64)
    struct.pack_into("<Q", h, 8, len(payload))
    struct.pack_into("<I", h, 16, csize)
    struct.pack_into("<I", h, 28, arch)
    struct.pack_into("<Q", h, 56, dsize)
    return bytes(h)


def check():
    payload = ELF_MAGIC + bytes(range(60))
    ent = _entry(payload, arch=90)
    blob = struct.pack("<IHHQ", FATBIN_MAGIC, 1, 16, len(ent) + len(payload)) + ent + payload
    got = list(entries(blob))
    assert len(got) == 1, got
    assert got[0] == (90, 2, payload), (got[0][0], got[0][1], len(got[0][2]))

    # Two entries back to back: the walk must advance by header+payload, not payload alone, or
    # every fatbin with more than one architecture silently loses all but the first -- and this
    # DLL ships four architectures per fatbin, so that bug would look like "sm_120 is missing".
    b2 = struct.pack("<IHHQ", FATBIN_MAGIC, 1, 16, 2 * (len(ent) + len(payload)))
    p2 = ELF_MAGIC + bytes(range(60, 120))
    got2 = list(entries(b2 + ent + payload + _entry(p2, arch=120) + p2))
    assert [g[0] for g in got2] == [90, 120], got2
    assert got2[1][2] == p2

    # A compressed entry must be decompressed with compressed_size, not the padded size: zstd
    # rejects trailing padding, and a walker that passes `size` fails only on real files.
    try:
        import zstandard
    except ImportError:
        print("carve_dlssnr_kernels: entry walk ok (zstandard absent, compressed path unchecked).")
        return 0
    raw = ELF_MAGIC + bytes(200)
    comp = zstandard.ZstdCompressor().compress(raw)
    padded = comp + b"\0" * 7                       # what the file actually stores
    ce = _entry(padded, arch=75, csize=len(comp), dsize=len(raw))
    blob3 = struct.pack("<IHHQ", FATBIN_MAGIC, 1, 16, len(ce) + len(padded)) + ce + padded
    arch, kind, data = list(entries(blob3))[0]
    assert arch == 75 and data == raw, (arch, len(data))

    print("carve_dlssnr_kernels: entries walk in order, and a padded zstd payload decompresses whole.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--check":
        sys.exit(check())
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    carve(Path(sys.argv[1]), Path(sys.argv[2]))
