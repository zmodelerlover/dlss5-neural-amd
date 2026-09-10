"""Pull the runtime DLL out of a DLSS-NR-on-AMD `dlssnr_on_amd_setup.exe`.

Since v0.2.6 that project ships one file: a console installer with the runtime appended to it,
raw. No archive, no compression, no resource section -- the installer's own PE ends and the DLL
starts on the next byte. So the DLL can be lifted out without running anything, which matters,
because running someone's installer to get a file you only want to read is a bad trade.

There are two trailers to get right, and both were found the hard way:

  * The installer PE is followed by the payload. "End of the installer" is the highest
    (raw pointer + raw size) across its section table, not the file size and not the last
    section in table order.
  * The payload itself is the DLL plus 242 bytes the setup builder appends. Keep those and every
    hash is wrong by a tail nobody documents. The same rule finds the DLL's real end: the highest
    (raw pointer + raw size) across *its* section table.

Verified against v0.2.14, v0.2.15, v0.2.16 and v0.2.17. On v0.2.14 the output hashes to
106223723fd9266c44d38dc2fb77933948ab37803f46bfcea2bae3a0a474ac84, which is the
`original_sha256` in runtime-patches.json, i.e. the file this add-on's offsets were read out of.

Usage:
    python extract_runtime.py <dlssnr_on_amd_setup.exe> [output.dll]

Prints the size and SHA-256 either way. No binaries ship with this project: the setup is yours.
"""

import hashlib
import struct
import sys


def pe_end(data, base=0):
    """Byte just past the last section of the PE at `base`. Raises if there is no PE there."""
    if data[base:base + 2] != b"MZ":
        raise ValueError("no MZ header at 0x%x" % base)
    e = struct.unpack_from("<I", data, base + 0x3C)[0] + base
    if data[e:e + 4] != b"PE\0\0":
        raise ValueError("no PE header at 0x%x" % e)
    count = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    table = e + 24 + opt
    end = 0
    for i in range(count):
        raw_size, raw_ptr = struct.unpack_from("<II", data, table + i * 40 + 16)
        end = max(end, base + raw_ptr + raw_size)
    return end


def extract(path):
    data = open(path, "rb").read()
    payload = data[pe_end(data):]
    if not payload:
        raise ValueError("nothing appended to the installer -- is this the right file?")
    dll = payload[:pe_end(payload)]
    return dll, len(payload) - len(dll)


def main(argv):
    if not 2 <= len(argv) <= 3:
        print(__doc__)
        return 2
    dll, tail = extract(argv[1])
    print("runtime: %d bytes" % len(dll))
    print("sha256 : %s" % hashlib.sha256(dll).hexdigest())
    print("trailer: %d bytes dropped" % tail)
    if len(argv) == 3:
        open(argv[2], "wb").write(dll)
        print("written : %s" % argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
