"""Pull the HIP kernels out of the DLSS-NR-on-AMD runtime, so a question about what the port's
kernels do can be answered by reading them instead of by guessing.

    python tools/carve_amd_kernels.py <dlssnr_amd_pass1.dll> <outdir>
    python tools/carve_amd_kernels.py --check

This is the AMD counterpart of carve_dlssnr_kernels.py, and it is much easier than the NVIDIA
side. The runtime embeds ONE clang offload bundle (magic `__CLANG_OFFLOAD_BUNDLE__`, at file
offset 0x98200 on v0.3.0), and its entries are plain uncompressed AMDGPU ELF code objects -- no
zstd, no fatbin wrapper per entry. Nine targets ship: gfx10-3-generic, gfx11-generic, gfx1100,
gfx1101, gfx1102, gfx1200, gfx1201, gfx9-generic, plus an empty host entry.

The handoff of 22/09 said these kernels were "GCN pre-compilado sem fonte" and closed the question
there. No source, true -- but an AMDGPU code object carries its metadata in a note section, so
unlike a CUDA cubin it names every kernel and gives every kernel argument's offset, size and kind
without any disassembly at all:

    llvm-readobj --elf-output-style=GNU --notes gfx1201.co    # amdhsa.kernels: names and .args
    llvm-readobj --elf-output-style=GNU --syms  gfx1201.co    # sizes, and the module globals
    llvm-objdump -d --triple=amdgcn-amd-amdhsa --mcpu=gfx1201 gfx1201.co

34 kernels come out named: k_import, k_pre_block_1h_32_fp8, k_swin_var<32|64|128|256>, k_qkv,
k_qkv_attn, k_attention, k_ffwd, k_conv_res, k_expand, k_contract2, k_dec_upsample, k_final_head,
k_post_block_1h_32_fp8, k_export, k_reproject, k_repack, k_mean, and the GPU-side fence pair
k_flag_set / k_flag_wait. Two module globals matter: g_e4m3_lut (512 bytes) and DlssNrEngine::SH.

Two things this found the hard way, both in spike/rocm-custom-kernel/README.md:

  * hipModuleLoadData accepts a carved entry verbatim -- a bare ELF, no re-bundling. The kernels
    can be called by mangled name from our own process.
  * g_e4m3_lut is ALL ZERO in the shipped image; the runtime fills its own module's copy at init.
    On gfx11 79 code sites read it. Anyone loading a carved object and launching its kernels must
    fill that global first or every fp8 weight dequantises through zeros, silently.

No binaries ship with this project: the runtime is yours.
"""
import struct
import sys
from pathlib import Path

MAGIC = b"__CLANG_OFFLOAD_BUNDLE__"
ELF_MAGIC = b"\x7fELF"

# EF_AMDGPU_MACH values seen in this runtime, for the --check self-test and for naming.
KNOWN_MACH = {0x41: "gfx1100", 0x42: "gfx1101", 0x43: "gfx1102", 0x48: "gfx1200", 0x4E: "gfx1201"}


def entries(blob, base):
    """Walk the bundle at `base`. Yields (target_id, offset, size) with offsets bundle-relative."""
    if blob[base:base + len(MAGIC)] != MAGIC:
        raise ValueError("no bundle magic at %#x" % base)
    pos = base + len(MAGIC)
    count, = struct.unpack_from("<Q", blob, pos)
    pos += 8
    for _ in range(count):
        off, size, tlen = struct.unpack_from("<QQQ", blob, pos)
        pos += 24
        target = blob[pos:pos + tlen].decode("ascii", "replace")
        pos += tlen
        yield target, off, size


def carve(path: Path, outdir: Path):
    blob = path.read_bytes()
    base = blob.find(MAGIC)
    if base < 0:
        raise SystemExit("%s: no clang offload bundle found." % path)
    print("bundle at %#x" % base)
    outdir.mkdir(parents=True, exist_ok=True)

    written = 0
    for target, off, size in entries(blob, base):
        if size == 0:                                   # the host entry is a zero-length stub
            print("  %-46s  (empty, skipped)" % target)
            continue
        payload = blob[base + off:base + off + size]
        # The arch is in the target id, but read it back out of the ELF too: a mismatch means the
        # bundle header and the payload disagree, which is worth knowing before loading either.
        mach = struct.unpack_from("<I", payload, 0x30)[0] & 0xFF if payload[:4] == ELF_MAGIC else None
        arch = target.rsplit("--", 1)[-1] or "host"
        out = outdir / ("%s.co" % arch)
        out.write_bytes(payload)
        written += 1
        note = ""
        if mach is not None and mach in KNOWN_MACH and KNOWN_MACH[mach] != arch:
            note = "  !! ELF says %s" % KNOWN_MACH[mach]
        print("  %-46s  %9d bytes -> %s%s" % (target, size, out.name, note))
    print("%d code objects written to %s" % (written, outdir))
    return written


def check():
    """Self-test on the bundle layout this was written against, with no file on disk."""
    target = b"hipv4-amdgcn-amd-amdhsa--gfx1201"
    # The entry offset is bundle-relative and has to land past the header, so compute it rather
    # than picking a round number. The first version of this test picked 64, which is inside the
    # header, and the last assert below is what caught it.
    size = len(MAGIC) + 8 + 24 + len(target)
    hdr = bytearray(MAGIC) + struct.pack("<Q", 1) + struct.pack("<QQQ", size, 4, len(target)) + target
    blob = bytes(hdr) + b"DATA"
    got = list(entries(blob, 0))
    assert got == [(target.decode(), size, 4)], got
    assert blob[size:size + 4] == b"DATA"
    print("PASS: bundle header walk")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--check":
        return check()
    if len(argv) != 3:
        print(__doc__)
        return 2
    carve(Path(argv[1]), Path(argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
