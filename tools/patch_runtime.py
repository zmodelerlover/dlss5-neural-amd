"""Rebuild the dlssnr_amd runtime so an add-on can drive it from outside.

The installer that ships with DLSS-NR-on-AMD does not patch anything: the `version.dll` it drops
is byte for byte the DLL inside `dlssnr_on_amd_setup.exe` -- appended to it up to v0.3.1, a byte
array in its .rdata from v0.3.3 -- which `tools/extract_runtime.py` lifts out without running it.
Every change here is ours, and there are two, both in place and both the same length, so no RVA
moves and the offsets the add-on writes into stay valid. They neutralise two calls, because the
DLL was built to install its own hooks and to announce a submission it did not make. Driving it
from an add-on means doing both ourselves.

The offsets are file offsets into one exact build and move with every release, so
runtime-patches.json lists them per build (v0.4.1: the setup thread's `call CreateThread` at 0x65bd
and the doubled ExecuteCommandLists call at 0x91a2; 0x667d and 0x9232 on v0.4.0, 0x60a6 and 0x8873
on v0.3.0). The build is picked by the input's hash, and a file whose hash is no build's
`original_sha256` is refused, so a stale pairing cannot be applied silently. The output has to hash
to that build's `patched_sha256`, the `kSha256` its entry in runtime_offsets.h names.

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
    spec = json.load(open(patches, encoding="utf-8"))

    have = hashlib.sha256(data).hexdigest()
    build = next((b for b in spec["builds"] if b["original_sha256"] == have), None)
    if build is None:
        print(f"input is no build runtime-patches.json knows:\n  got  {have}")
        for b in spec["builds"]:
            print(f"  want {b['original_sha256']}  ({b['runtime']})")
        return 1
    print(f"input is {build['runtime']}")

    for change in build["changes"]:
        offset = int(change["offset"], 16)
        before = bytes.fromhex(change["before"])
        after = bytes.fromhex(change["after"])
        why = spec["why"][change["patch"]]
        if len(before) != len(after):
            print(f"patch would change the size, which moves every RVA: {why}")
            return 1
        if data[offset:offset + len(before)] != before:
            print(f"bytes at offset {change['offset']} do not match: {why}")
            return 1
        data[offset:offset + len(after)] = after
        print(f"applied at {change['offset']}: {change['patch']}")

    digest = hashlib.sha256(bytes(data)).hexdigest()
    if digest != build["patched_sha256"]:
        print(f"patched, it hashes to {digest}, not the {build['patched_sha256']} listed; not written")
        return 1
    open(dst, "wb").write(bytes(data))
    print(f"\nwrote:  {dst}")
    print(f"sha256: {digest} (kSha256 of {build['runtime']} in core/addon/runtime_offsets.h)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
