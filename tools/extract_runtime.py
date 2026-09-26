"""Pull the runtime DLL out of a DLSS-NR-on-AMD `dlssnr_on_amd_setup.exe`.

Since v0.2.6 that project ships one file, an installer with the runtime inside it, raw. No
archive, no compression, no resource section. So the DLL can be lifted out without running
anything, which matters, because running someone's installer to get a file you only want to read
is a bad trade.

Where inside moved with v0.3.3, and the two layouts have nothing else in common:

  * v0.2.6 to v0.3.1: a console installer with the DLL appended. The installer's own PE ends and
    the DLL starts on the next byte, then a tail the setup builder appends -- 242 bytes on
    v0.2.17, 267 on v0.3.0 and v0.3.1, so not a constant to hardcode.
  * v0.3.3 on: a Tauri installer with nothing appended. The DLL is a byte array in its .rdata,
    unaligned, straight after the strings "dlssnr_on_amd_weights.bin" and "nvngx_dlssnr.dll",
    with the rest of the installer after it. It sits at 0x2609c7 on both v0.3.3 and v0.4.0, but
    only because what comes before it happens to be the same size in both.

So neither the file size nor the installer's end says where the DLL is; the DLL does. Every "MZ"
whose e_lfanew leads to a PE signature, whose sections end inside the file and whose header says
DLL is a candidate, and there has to be exactly one besides the installer at offset 0 -- two
means the layout moved again, and guessing between them is how a wrong hash gets pinned.

Its end is the rule found the hard way on the appended layout: the highest (raw pointer + raw
size) across *its* section table, not the file size and not the last section in table order.
Keep the builder's tail, or on the new layout the rest of the .rdata, and every hash is wrong by
bytes nobody documents.

Verified against v0.3.0, v0.3.1, v0.3.3 and v0.4.0; the appended-only version this replaced was
verified against v0.2.14 to v0.3.0. Output hashes:

  v0.3.0  8321cae728d28cb7632d0d58d3d913e91132bf7645c126505698fbe4cd5a0138
  v0.3.1  b108d6407eb7f094a4f9111edd778eee7b978b648d413a9fc7aeedfdd914c154
  v0.3.3  907b30a61644a6d7e43e58a43a9d97a04a24b1a764a88bdef3954ac807e8d112
  v0.4.0  d62be3d8b9fbb3c6c81982c4ddb3dfa00eb9662e3206925cbe5b7e1bc6798b80

The v0.4.0 one is the `original_sha256` in runtime-patches.json, i.e. the file this add-on's
offsets were read out of. The v0.3.0 one is byte for byte the `version.dll` that setup drops, so
a folder that already has one did not need the setup run again. The v0.3.3 and v0.4.0 setups were
never run to compare against what they drop.

Usage:
    python extract_runtime.py <dlssnr_on_amd_setup.exe> [output.dll]

Prints the size, the SHA-256 and where the DLL was found either way. No binaries ship with this
project: the setup is yours.
"""

import hashlib
import struct
import sys

IMAGE_FILE_DLL = 0x2000


def pe_image(data, base):
    """(sections, is_dll) of the PE at `base`, sections as (name, start, end) file offsets.

    None when the bytes there are not a PE header, which most "MZ" in a binary are not.
    """
    if data[base:base + 2] != b"MZ" or base + 0x40 > len(data):
        return None
    e = base + struct.unpack_from("<I", data, base + 0x3C)[0]
    if e + 24 > len(data) or data[e:e + 4] != b"PE\0\0":
        return None
    count = struct.unpack_from("<H", data, e + 6)[0]
    opt, flags = struct.unpack_from("<HH", data, e + 20)
    table = e + 24 + opt
    if count == 0 or table + count * 40 > len(data):
        return None
    sections = []
    for i in range(count):
        entry = table + i * 40
        name = data[entry:entry + 8].rstrip(b"\0").decode("ascii", "replace")
        raw_size, raw_ptr = struct.unpack_from("<II", data, entry + 16)
        sections.append((name, base + raw_ptr, base + raw_ptr + raw_size))
    return sections, bool(flags & IMAGE_FILE_DLL)


def pe_end(sections):
    """Byte just past the last section, by raw end rather than by table order."""
    return max(end for _, _, end in sections)


def extract(path):
    """The runtime's bytes and a line saying where in the setup they were."""
    data = open(path, "rb").read()
    installer = pe_image(data, 0)
    if installer is None:
        raise ValueError("not a PE file -- is this the right file?")
    found = []
    at = data.find(b"MZ", 1)
    while at != -1:
        image = pe_image(data, at)
        if image is not None and image[1] and pe_end(image[0]) <= len(data):
            found.append((at, pe_end(image[0])))
        at = data.find(b"MZ", at + 1)
    if len(found) != 1:
        starts = ", ".join("0x%x" % start for start, _ in found) or "none"
        raise ValueError("expected one DLL in the installer, found %d: %s" % (len(found), starts))
    start, end = found[0]
    holder = next((name for name, lo, hi in installer[0] if lo <= start < hi), None)
    if holder is None:
        where = "0x%x, appended; %d bytes of trailer dropped" % (start, len(data) - end)
    else:
        where = "0x%x, inside the installer's %s" % (start, holder)
    return data[start:end], where


def main(argv):
    if not 2 <= len(argv) <= 3:
        print(__doc__)
        return 2
    dll, where = extract(argv[1])
    print("runtime: %d bytes" % len(dll))
    print("sha256 : %s" % hashlib.sha256(dll).hexdigest())
    print("found  : %s" % where)
    if len(argv) == 3:
        open(argv[2], "wb").write(dll)
        print("written : %s" % argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
