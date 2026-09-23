"""Transport regression: two amd-nr.addon64 builds inside the same minimal hosts, compared per API.

Build hostcheck and hostcapture, and the add-on on both sides (a worktree of the last release for
the baseline):
  python tools/host_check.py --a ../wt-release/build/amd-nr.addon64 --b build/amd-nr.addon64 \
    --tools build --reshade /path/to/ReShade64-as-dxgi.dll --runtime-dir /path/to/runtime \
    --input /path/to/frame.ppm --output /new/directory [--apis d3d11 opengl]

Each host presents the same frame with the effect on from the first frame; the capture add-on
saves the composed back buffer after ReShade's banner is gone. The two builds must differ by no
more than each differs from itself, and every case must differ from the effect-off control, so an
effect that silently stopped reaching the screen cannot pass as SAME. The window is off-screen
and takes no focus.
"""
import argparse
import json
from pathlib import Path
import os
import shutil
import subprocess

import numpy as np

APIS = ("d3d11", "d3d12", "vulkan", "opengl")
RESHADE_INI = """[ADDON]
DisabledAddons=
[GENERAL]
EffectSearchPaths=
TextureSearchPaths=
PresetPath=.\\ReShadePreset.ini
[OVERLAY]
ShowClock=0
ShowFPS=0
ShowFrameTime=0
ShowScreenshotMessage=0
TutorialProgress=4
"""
# Temporal=1 forces the runtime's temporal accumulation off, as framecheck does: with it on the
# result depends on how many frames ran before the capture, which no two runs agree on.
BASE = dict(StartOn=1, FeedEffect=0, Temporal=1, Passes=1, Scale=1, Inline=1, Style=0, DebugView=0,
            NetworkOutput=0, Motion=0, History=0, Depth=0, GameGuides=0, Diagnostics=1)
CASES = {
    "off": dict(StartOn=0),  # the control: what the host draws with nothing applied
    "inline1": {},
    "inline3_scale119": dict(Passes=3, Scale=1.19),
    "style2_additive": dict(Style=2, Guard=0),
}


def run_one(args, api, addon, folder, keys):
    folder.mkdir(parents=True)
    shutil.copy2(args.tools / "amd-nr-hostcheck.exe", folder)
    shutil.copy2(args.tools / "amd-nr-hostcapture.addon64", folder / "zz-hostcapture.addon64")
    shutil.copy2(addon, folder / "amd-nr.addon64")
    if api in ("d3d11", "d3d12"):
        shutil.copy2(args.reshade, folder / "dxgi.dll")
    elif api == "opengl":
        shutil.copy2(args.reshade, folder / "opengl32.dll")
    for filename in ("dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin", "dlssnr_on_amd.ini"):
        src = args.runtime_dir / filename
        try:
            os.link(src, folder / filename)
        except OSError:
            shutil.copy2(src, folder / filename)
    (folder / "ReShade.ini").write_text(RESHADE_INI)
    ini = {**BASE, **keys}
    (folder / "amd-nr.ini").write_text("[amd-nr]\n" + "".join(f"{k}={v}\n" for k, v in ini.items()))
    out = folder / "capture.raw"
    env = dict(os.environ, HOSTCHECK_OUT=str(out))
    with (folder / "stdout.log").open("w") as log:
        r = subprocess.run([str(folder / "amd-nr-hostcheck.exe"), api, str(args.input.resolve())],
                           cwd=folder, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=150)
    meta = folder / "capture.raw.txt"
    if r.returncode != 0 or not meta.exists() or "ok=1" not in meta.read_text():
        raise RuntimeError(f"{folder}: exit {r.returncode}, see stdout.log and amd-nr.log")
    return out.read_bytes()


def mad(x, y):
    return float(np.abs(x.astype(np.int16) - y.astype(np.int16)).mean())


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    results = {}
    for api in args.apis:
        off = None
        for name, keys in CASES.items():
            # Inside a real host the network is not bit-exact run to run (framecheck waits out
            # every job; a host does not), so each build runs twice and the two builds must differ
            # by no more than a build differs from itself. Where that noise is zero, exact.
            shot = {k: np.frombuffer(run_one(args, api, args.a if k[0] == "a" else args.b,
                                             args.output / api / name / k, keys), np.uint8)
                    for k in ("a1", "a2", "b1", "b2")}
            noise = max(mad(shot["a1"], shot["a2"]), mad(shot["b1"], shot["b2"]))
            cross = max(mad(shot[x], shot[y]) for x in ("a1", "a2") for y in ("b1", "b2"))
            verdict = "SAME" if cross <= noise * 1.5 + 1e-9 else "DIFFERS"
            if name == "off":
                off = shot["b1"]
            elif mad(shot["b1"], off) <= max(3 * noise, 0.5):
                verdict += " (effect not on screen)"
            results[f"{api}/{name}"] = verdict
            print(f"{api:7} {name:18} {verdict:8} across {cross:.4f}  noise {noise:.4f}", flush=True)
    bad = [n for n, v in results.items() if v != "SAME"]
    results["result"] = "PASS" if not bad else "FAIL"
    (args.output / "result.json").write_text(json.dumps(results, indent=2))
    print("host_check:", results["result"], *bad)
    raise SystemExit(0 if not bad else 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("a", "b", "tools", "reshade", "runtime-dir", "input", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--apis", nargs="+", choices=APIS, default=list(APIS))
    run(parser.parse_args())
