"""Rebuild the dlssnr_amd runtime so an add-on can drive it from outside.

The installer that ships with DLSS-NR-on-AMD does not patch anything: the `version.dll` it drops
is byte for byte the payload appended to `dlssnr_on_amd_setup.exe`, which
`tools/extract_runtime.py` lifts out without running it. Every change here is ours, and there are
three, all in place and all the same length, so no RVA moves and the offsets the add-on writes
into stay valid:

  * two calls neutralised, because the DLL was built to install its own hooks and to announce a
    submission it did not make. Driving it from an add-on means doing both ourselves.
  * one log string, so a timed-out frame does not report a fallback that no longer happens.

Two things this used to do and no longer does:

  * The shader edit that made a timed-out frame keep its own input rather than paste last frame's
    residual. v0.2.17 exposes that choice as ToneChannels bit 4 with bit 2 clear, and the add-on
    sets it, so there is nothing left to patch.
  * The GPU wait spin cap was never applied and still is not. It bounds the wait shader at 262144
    iterations, which is about 6 ms, against a network that takes 16 to 187 ms -- so inline mode
    times out every frame and the residual comes out at exactly zero. Measured, not assumed.

Usage:
    python patch_runtime.py <version.dll> <runtime-patches.json> <output.dll>

No binaries ship with this project: both inputs are yours.
"""

import hashlib
import json
import sys

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
