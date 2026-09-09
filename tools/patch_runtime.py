"""Rebuild the dlssnr_amd runtime so an add-on can drive it, without the spin cap.

The OptiScaler installer applies five patches to the original `version.dll`. Four are needed
to drive the DLL from outside; the fifth caps the GPU wait shader at 262144 iterations, and
that one is what makes inline mode useless: the GPU waits ~6 ms, the network takes 16-187 ms,
it times out on every frame, and the apply pass preserves its input. The residual comes out at
exactly zero -- measured, not assumed.

This script applies the four and skips the fifth. All of them write in place at the same
length, so no RVA moves and the offsets the add-on uses stay valid.

Usage:
    python patch_runtime.py <original version.dll> <runtime-patches.json> <output.dll>

No binaries ship with this project: both inputs are yours.
"""

import hashlib
import json
import sys

CAP = "bound GPU wait shader"


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    src, patches, dst = argv[1:]

    data = bytearray(open(src, "rb").read())
    spec = json.load(open(patches))

    have = hashlib.sha256(data).hexdigest()
    if have != spec["original_sha256"]:
        print(f"input is not the expected binary:\n  got  {have}\n  want {spec['original_sha256']}")
        return 1

    for change in spec["changes"]:
        offset = int(change["offset"], 16)
        before = bytes.fromhex(change["before"])
        after = bytes.fromhex(change["after"])
        if len(before) != len(after):
            print(f"patch would change the size, which moves every RVA: {change['reason']}")
            return 1
        if data[offset:offset + len(before)] != before:
            print(f"bytes at offset {change['offset']} do not match: {change['reason']}")
            return 1
        if CAP in change["reason"]:
            print(f"SKIPPED {change['reason']}")
            continue
        data[offset:offset + len(after)] = after
        print(f"applied {change['reason']}")

    open(dst, "wb").write(bytes(data))
    digest = hashlib.sha256(bytes(data)).hexdigest()
    print(f"\nwrote:  {dst}")
    print(f"sha256: {digest}")
    print("\nput this hash in kRuntimeSha256, in src/neural/neural.cpp:")
    print("{" + ", ".join("0x%02x" % b for b in bytes.fromhex(digest)) + "}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
