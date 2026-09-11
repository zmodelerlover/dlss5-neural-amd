"""GPU regression: each inline pass must consume its own tuning.

Build framecheck first. Supply a real P6 SDR frame and the pinned runtime files:
  python tools/pass_profile_check.py --exe build/dlss5-framecheck.exe \
    --runtime-dir /path/to/runtime --input /path/to/frame.ppm --output /new/directory

Runs the actual pipeline, with temporal/history disabled. The old batch path
records tone 1 -> 0 but produces exactly the same image as serial tone 0 -> 0.
The corrected serial tone 1 -> 0 must differ and reproduce across two processes.
No game installation is modified. Outputs/logs are kept for visual inspection.
"""
import argparse
import csv
import json
from pathlib import Path
import shutil
import subprocess

import numpy as np

BASE = """[dlss5]
Scale=1.0
Passes=2
Encoding=0
Tonemap=0
Structure=1.0
Skin=-1.0
Intensity=1.0
AutoMask=1
EngineScale=0.03125
ToneChannels=0
Inline=1
Bicubic=1
Motion=0
History=0
Temporal=1
Depth=0
GameGuides=0
Guard=2
GuardPerPass=0
ColourStrength=1
ResidualLimit=0.25
EdgeFade=0
PassTaper=0
DebugView=0
"""


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    images = {}
    inputs = []
    for name, serial, tone in [("serial_tone1", 1, 1), ("serial_tone0", 1, 0),
                               ("batch_tone1", 0, 1), ("serial_repeat", 1, 1)]:
        folder = args.output / name
        # A watchdog fallback preserves the input instead of applying the network.
        # Keep its evidence, but do not compare that incomplete chain as an inference.
        for attempt in range(1, 4):
            folder = args.output / name / f"attempt-{attempt}"
            folder.mkdir(parents=True)
            exe = folder / "dlss5-framecheck.exe"
            shutil.copy2(args.exe, exe)
            for filename in ("dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin", "dlssnr_on_amd.ini"):
                shutil.copy2(args.runtime_dir / filename, folder / filename)
            (folder / "dlss5-neural.ini").write_text(BASE + f"SerialPasses={serial}\nTone={tone}\n")
            with (folder / "stdout.log").open("w") as log:
                subprocess.run([str(exe.resolve()), str(args.input.resolve()), "40"],
                               cwd=folder, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
            with (folder / "timings.csv").open() as log:
                last = list(csv.DictReader(log))[-1]
            first_job = int(last["job"]) - int(last["accepted"]) + 1
            if int(last["watchdog_job"]) < first_job:
                break
            print(name, f"attempt {attempt}: final frame hit watchdog; retained, retrying", flush=True)
        else:
            raise RuntimeError(f"{name}: no complete final capture in three attempts")
        for kind in ("input", "runtime"):
            filename = "f40-" + kind + ".raw"
            with (folder / "capture.csv").open() as manifest:
                row = next(row for row in csv.DictReader(manifest) if row["file"] == filename)
            a = np.fromfile(folder / filename, dtype="<f2")
            assert row["dxgi_format"] == "10" and a.size == int(row["width"]) * int(row["height"]) * 4
            assert np.isfinite(a).all(), f"non-finite pixels: {name}/{kind}"
            if kind == "input":
                inputs.append(a)
            else:
                images[name] = a
        print(name, "captured", flush=True)
    assert all(np.array_equal(inputs[0], a) for a in inputs), "input differs across cases"
    assert np.array_equal(images["serial_tone1"], images["serial_repeat"]), "serial result not reproducible"
    delta = np.abs(images["serial_tone1"].astype("f4") - images["serial_tone0"].astype("f4"))
    assert float(delta.mean()) > 1e-5, "first-pass tone was lost: profiles produced the same result"
    assert np.array_equal(images["batch_tone1"], images["serial_tone0"]), "legacy reproduction changed; investigate"
    result = {"result": "PASS", "serial_tone_delta_mae_rgba": float(delta.mean()),
              "serial_tone_delta_max": float(delta.max()), "repeat_exact": True,
              "legacy_discards_first_tone": True}
    (args.output / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "runtime-dir", "input", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    run(parser.parse_args())
