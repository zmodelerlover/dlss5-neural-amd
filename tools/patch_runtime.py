"""Rebuild the dlssnr_amd runtime so an add-on can drive it from outside.

The installer that ships with DLSS-NR-on-AMD does not patch anything: the `version.dll` it drops
is byte for byte the DLL inside `dlssnr_on_amd_setup.exe` -- appended to it up to v0.3.1, a byte
array in its .rdata from v0.3.3 -- which `tools/extract_runtime.py` lifts out without running it.
Every change here is ours, and there are two, both in place and both the same length, so no RVA
moves and the offsets the add-on writes into stay valid. They neutralise two calls, because the
DLL was built to install its own hooks and to announce a submission it did not make. Driving it
from an add-on means doing both ourselves.

The offsets are file offsets into one exact build, and they moved again for v0.4.0: the setup
thread's `call CreateThread` is at 0x667d (0x60a6 on v0.3.0, 0x6006 on v0.2.17), and the doubled
ExecuteCommandLists call at 0x9232 (0x8873, 0x8583). The script refuses a file whose hash is not
`original_sha256`, so a stale pairing cannot be applied silently.

Three things this used to do and no longer does:

  * One log string, 'previous residual shown' rewritten as 'current input kept'. The original was
    the true one: on the add-on's path a timed-out inline frame after the first is shown with
    last frame's residual, on v0.3.0 as on v0.4.0, so the rewrite only made the log wrong.
  * The shader edit that made a timed-out frame keep its own input rather than paste last frame's
    residual. It was dropped on the reading that v0.2.17 exposed the same choice as ToneChannels
    bit 4 with bit 2 clear; on v0.3.0 and v0.4.0 the runtime builds that word from its own state,
    so the choice is not the add-on's to make. The host-side way to make it is listed under
    `dropped` in runtime-patches.json, unapplied until it is measured.
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
    print("\nput this hash in kRuntimeSha256, in core/addon/neural.cpp:")
    print("{" + ", ".join("0x%02x" % b for b in bytes.fromhex(digest)) + "}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
