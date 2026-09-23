"""GPU regression: two framecheck builds must produce the same bytes over a settings matrix.

Build framecheck on both sides first (a worktree of the last release for the baseline):
  python tools/ab_check.py --a ../wt-release/build/amd-nr-framecheck.exe \
    --b build/amd-nr-framecheck.exe --runtime-dir /path/to/runtime \
    --input /path/to/frame.ppm --output /new/directory

Runs the actual pipeline twice per case, one process per build, and compares every capture of
the final frame (input, runtime, residual, composed). A change that was meant to move the picture
shows up here as a DIFFERS; one that was not meant to must not. Each case is also checked to move
the picture away from the base case, so a setting the ini stopped reaching cannot pass as SAME.
"""
import argparse
import csv
import json
from pathlib import Path
import shutil
import subprocess

FRAMES = 12
BASE = dict(Scale=1.0, Passes=1, Encoding=0, Tonemap=0, Structure=1.0, Skin=-1.0, Intensity=1.0,
            AutoMask=1, EngineScale=0.03125, ToneChannels=0, Inline=1, Bicubic=1, Motion=0,
            History=0, Temporal=1, Depth=0, GameGuides=0, Guard=2, GuardPerPass=0,
            ColourStrength=1, ResidualLimit=0.25, EdgeFade=0, PassTaper=0, DebugView=0,
            SerialPasses=1, Tone=1, Style=0, StyleStrength=1, NetworkOutput=0)
# Not here: Tonemap=1. The runtime is not deterministic with it -- the same build against itself
# differs in three runs of four -- so a comparison there says nothing about the add-on.
CASES = {
    "base": {},
    "passes2": dict(Passes=2),
    "passes3": dict(Passes=3),
    "passes3_taper": dict(Passes=3, PassTaper=1),
    "passes2_override": dict(Passes=2, Pass2Override=1, Pass2Structure=0.5, Pass2Tone=0, Pass2Skin=1),
    "scale050": dict(Scale=0.5),
    "scale119": dict(Scale=1.19),
    "scale200": dict(Scale=2.0),
    "additive": dict(Guard=0),
    "guard4_perpass": dict(Guard=4, Passes=2, GuardPerPass=1),
    "intensity15": dict(Intensity=1.5),
    "colour05": dict(ColourStrength=0.5),
    "style1": dict(Style=1),
    "style2": dict(Style=2),
    "style3_half": dict(Style=3, StyleStrength=0.5),
    "structure3_skin2": dict(Structure=3, Skin=2),
    "tone0": dict(Tone=0),
    "nobicubic": dict(Bicubic=0),
    "limit0_fade": dict(ResidualLimit=0, EdgeFade=0.25),
    "automask0": dict(AutoMask=0),
    "netoutput": dict(NetworkOutput=1),
    **{f"debug{v}": dict(DebugView=v) for v in (1, 2, 3, 5, 6)},
}
KINDS = ("input", "runtime", "residual", "composed")


def run_one(args, exe, folder, keys):
    # A watchdog fallback preserves the input instead of applying the network. Retry rather than
    # compare an incomplete chain.
    for attempt in range(1, 4):
        here = folder / f"attempt-{attempt}"
        here.mkdir(parents=True)
        shutil.copy2(exe, here / "amd-nr-framecheck.exe")
        for filename in ("dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin", "dlssnr_on_amd.ini"):
            shutil.copy2(args.runtime_dir / filename, here / filename)
        ini = {**BASE, **keys}
        (here / "amd-nr.ini").write_text("[amd-nr]\n" + "".join(f"{k}={v}\n" for k, v in ini.items()))
        with (here / "stdout.log").open("w") as log:
            subprocess.run([str((here / "amd-nr-framecheck.exe").resolve()), str(args.input.resolve()),
                            str(FRAMES)], cwd=here, stdout=log, stderr=subprocess.STDOUT, check=True,
                           timeout=180)
        with (here / "timings.csv").open() as log:
            last = list(csv.DictReader(log))[-1]
        if int(last["watchdog_job"]) < int(last["job"]) - int(last["accepted"]) + 1:
            return {k: (here / f"f{FRAMES}-{k}.raw").read_bytes() for k in KINDS}
    raise RuntimeError(f"{folder}: no complete final capture in three attempts")


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    results, composed = {}, {}
    for name, keys in CASES.items():
        a = run_one(args, args.a, args.output / name / "a", keys)
        b = run_one(args, args.b, args.output / name / "b", keys)
        diffs = [k for k in KINDS if a[k] != b[k]]
        results[name] = "SAME" if not diffs else "DIFFERS: " + ",".join(diffs)
        composed[name] = b["composed"]
        print(f"{name:18} {results[name]}", flush=True)
    inert = [n for n in CASES if n != "base" and composed[n] == composed["base"]]
    for n in inert:
        results[n] += " (does not move the picture: setting not reaching the pipeline?)"
    bad = [n for n, v in results.items() if v != "SAME"]
    results["result"] = "PASS" if not bad else "FAIL"
    (args.output / "result.json").write_text(json.dumps(results, indent=2))
    print("ab_check:", results["result"], *bad)
    raise SystemExit(0 if not bad else 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("a", "b", "runtime-dir", "input", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    run(parser.parse_args())
