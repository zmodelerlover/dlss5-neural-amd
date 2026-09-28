"""Transport regression: two amd-nr.addon64 builds inside the same minimal hosts, compared per API.

Build hostcheck and hostcapture, and the add-on on both sides (a worktree of the last release for
the baseline):
  python tools/host_check.py --a ../wt-release/build/amd-nr.addon64 --b build/amd-nr.addon64 \
    --tools build --reshade /path/to/ReShade64-as-dxgi.dll --runtime-dir /path/to/runtime \
    --input /path/to/frame.ppm --output /new/directory [--apis d3d11 opengl]

The 32-bit bridge the same way: --a/--b name each build's amd-nr.addon32 (its amd-nr-host64.exe
beside it), --tools the x86 build (build.ps1 -Arch x86), --reshade the 32-bit ReShade, and
--apis d3d9 d3d11, the two the bridge carries.

Each host presents the same frame with the effect on from the first frame; the capture add-on
saves the composed back buffer after ReShade's banner is gone. The two builds must differ by no
more than each differs from itself, and every case must differ from the effect-off control, so an
effect that silently stopped reaching the screen cannot pass as SAME. The window is off-screen
and takes no focus.

--cross holds one build against itself across the APIs instead, at the defaults a fresh ini has
(Temporal On, Fixed seed, Output smoothing): "the same ini gives the same result on every route".
Each API presents the same still frame, --frames of them in a row are kept (HOSTCHECK_FRAMES), and
the line per API is that still frame's flicker, the largest of R, G and B moving between
consecutive captures. Two APIs are SAME within noise when their captures differ on average by no
more than 1.5 times the larger of the two's own frame-to-frame change, or half a code value, since
the crossing formats differ (OpenGL crosses in RGBA8, D3D12 in the back buffer's own). Add the
32-bit bridge's d3d9 and d3d11 with --addon32, --tools32 and --reshade32:
  python tools/host_check.py --cross --b build/amd-nr.addon64 --tools build --reshade ... \
    --runtime-dir ... --input frame.ppm --output /new/directory [--frames 8]

  python tools/host_check.py --selftest   # the verdicts on made-up captures; no GPU
"""
import argparse
import json
from pathlib import Path
import os
import shutil
import subprocess

import numpy as np

APIS = ("d3d9", "d3d11", "d3d12", "vulkan", "opengl")
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
# --cross: the settings a fresh amd-nr.ini is written with, and the stats line in each log.
CROSS = dict(StartOn=1, FeedEffect=0, Temporal=2, FixedSeed=1, OutputSmooth=0.8, OutputSmoothLimit=10,
             Inline=1, Passes=1, Scale=1, Style=0, DebugView=0, NetworkOutput=0, Depth=0, GameGuides=0,
             Diagnostics=2)
# Temporal=1 forces the runtime's temporal accumulation off, as framecheck does: with it on the
# result depends on how many frames ran before the capture, which no two runs agree on. FixedSeed
# and OutputSmooth off, as a release from before they were defaults runs.
BASE = dict(StartOn=1, FeedEffect=0, Temporal=1, Passes=1, Scale=1, Inline=1, Style=0, DebugView=0,
            NetworkOutput=0, Motion=0, History=0, Depth=0, GameGuides=0, Diagnostics=1,
            FixedSeed=0, OutputSmooth=0)
CASES = {
    "off": dict(StartOn=0),  # the control: what the host draws with nothing applied
    "inline1": {},
    "inline3_scale119": dict(Passes=3, Scale=1.19),
    "style2_additive": dict(Style=2, Guard=0),
}


def run_one(args, api, addon, folder, keys, frames=1, base=BASE):
    """The captures of one host run, as bytes: one, or `frames` presents in a row."""
    folder.mkdir(parents=True)
    # An .addon32 is the 32-bit bridge, and it brings its 64-bit helper from the same build.
    ext = addon.suffix
    shutil.copy2(args.tools / "amd-nr-hostcheck.exe", folder)
    shutil.copy2(args.tools / f"amd-nr-hostcapture{ext}", folder / f"zz-hostcapture{ext}")
    shutil.copy2(addon, folder / f"amd-nr{ext}")
    if ext == ".addon32":
        shutil.copy2(addon.parent / "amd-nr-host64.exe", folder)
    proxy = {"d3d9": "d3d9.dll", "d3d11": "dxgi.dll", "d3d12": "dxgi.dll", "opengl": "opengl32.dll"}
    if api in proxy:
        shutil.copy2(args.reshade, folder / proxy[api])
    for filename in ("dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin", "dlssnr_on_amd.ini"):
        src = args.runtime_dir / filename
        try:
            os.link(src, folder / filename)
        except OSError:
            shutil.copy2(src, folder / filename)
    (folder / "ReShade.ini").write_text(RESHADE_INI)
    ini = {**base, **keys}
    (folder / "amd-nr.ini").write_text("[amd-nr]\n" + "".join(f"{k}={v}\n" for k, v in ini.items()))
    out = folder / "capture.raw"
    env = dict(os.environ, HOSTCHECK_OUT=str(out), HOSTCHECK_FRAMES=str(frames))
    with (folder / "stdout.log").open("w") as log:
        r = subprocess.run([str(folder / "amd-nr-hostcheck.exe"), api, str(args.input.resolve())],
                           cwd=folder, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=150)
    meta = folder / "capture.raw.txt"
    if not meta.exists() or "ok=1" not in meta.read_text():
        raise RuntimeError(f"{folder}: exit {r.returncode}, see stdout.log and amd-nr.log")
    # The picture is already on disk; a crash on the way out is a finding of its own, not a reason
    # to throw the comparison away. The v0.6.6 bridge still takes every D3D9 host down here.
    if r.returncode != 0:
        print(f"        {folder}: exited {r.returncode:#x} after the capture", flush=True)
    return [out.read_bytes()] if frames == 1 else [Path(f"{out}.{i:03d}").read_bytes() for i in range(frames)]


def mad(x, y):
    return float(np.abs(x.astype(np.int16) - y.astype(np.int16)).mean())


def still_flicker(frames):
    """How a still frame's captures move one to the next, the largest of R, G and B, in code values:
    the mean, and the shares over 1 and over 4 (1/255 and 4/255)."""
    d = np.concatenate([np.abs(a.reshape(-1, 4)[:, :3].astype(np.int16) - b.reshape(-1, 4)[:, :3].astype(np.int16))
                        .max(axis=1) for a, b in zip(frames, frames[1:])])
    return dict(mean=float(d.mean()), over1=float((d > 1).mean()), over4=float((d > 4).mean()))


def cross_verdicts(runs):
    """{api: [captures]} of one build -> {"a~b": (verdict, across, noise)} for every pair. An API's
    noise is its mean frame-to-frame change; two agree within the larger of the two, half a code
    value at least, and differ past it."""
    noise = {api: float(np.mean([mad(a, b) for a, b in zip(f, f[1:])])) if len(f) > 1 else 0.0
             for api, f in runs.items()}
    out = {}
    names = list(runs)
    for i, x in enumerate(names):
        for y in names[i + 1:]:
            across = float(np.mean([mad(a, b) for a in runs[x] for b in runs[y]]))
            allowed = max(1.5 * max(noise[x], noise[y]), 0.5)
            out[f"{x}~{y}"] = ("SAME within noise" if across <= allowed else "DIFFERS", across, max(noise[x], noise[y]))
    return out


def cross(args):
    args.output.mkdir(parents=True, exist_ok=False)
    legs = [(api, args.b, args) for api in args.apis]
    if args.addon32:
        x86 = argparse.Namespace(**{**vars(args), "tools": args.tools32, "reshade": args.reshade32})
        legs += [(f"x86-{api}", args.addon32, x86) for api in ("d3d9", "d3d11")]
    runs, results = {}, {}
    for name, addon, leg in legs:
        shots = run_one(leg, name.removeprefix("x86-"), addon, args.output / name, {}, args.frames, CROSS)
        runs[name] = [np.frombuffer(s, np.uint8) for s in shots]
        f = still_flicker(runs[name]) if len(runs[name]) > 1 else dict(mean=0.0, over1=0.0, over4=0.0)
        results[f"{name}/flicker"] = f
        print(f"{name:10} still-frame flicker: mean {f['mean']:.3f} of 255, over 1/255 {100 * f['over1']:.2f}%, "
              f"over 4/255 {100 * f['over4']:.3f}%", flush=True)
    sizes = {len(r[0]) for r in runs.values()}
    if len(sizes) != 1:
        raise SystemExit(f"host_check: the APIs captured different sizes ({sorted(sizes)}), nothing to compare")
    for pair, (verdict, across, noise) in cross_verdicts(runs).items():
        results[pair] = verdict
        print(f"{pair:22} {verdict:18} across {across:.4f}  noise {noise:.4f}", flush=True)
    bad = [n for n, v in results.items() if isinstance(v, str) and v != "SAME within noise"]
    results["result"] = "PASS" if not bad else "FAIL"
    (args.output / "result.json").write_text(json.dumps(results, indent=2))
    print("host_check --cross:", results["result"], *bad)
    raise SystemExit(0 if not bad else 1)


def selftest():
    """The verdicts on made-up captures, and the old invocation still parsing."""
    rng = np.random.default_rng(7)
    still = rng.integers(0, 256, (64 * 32 * 4,), np.uint8)
    same = {"d3d11": [still] * 4, "vulkan": [still] * 4}
    assert all(v[0] == "SAME within noise" for v in cross_verdicts(same).values()), "identical APIs differ"
    off = {"d3d11": [still] * 4, "opengl": [np.clip(still.astype(np.int16) + 3, 0, 255).astype(np.uint8)] * 4}
    assert cross_verdicts(off)["d3d11~opengl"][0] == "DIFFERS", "an API three codes off passes"
    graded = np.clip(still.astype(np.int16) + 6, 0, 255).astype(np.uint8)
    f = still_flicker([still, graded, still, graded])
    assert f["over4"] > 0.9 and f["mean"] > 5, "raw and processed frames alternating read as still"
    assert still_flicker([still] * 3) == dict(mean=0.0, over1=0.0, over4=0.0), "a still capture flickers"
    noisy = {"d3d12": [still, graded, still], "d3d11": [graded, still, graded]}
    assert cross_verdicts(noisy)["d3d12~d3d11"][0] == "SAME within noise", "noise the APIs share reads as a difference"
    old = parser().parse_args(["--a", "a.addon64", "--b", "b.addon64", "--tools", "t", "--reshade", "r",
                               "--runtime-dir", "d", "--input", "i.ppm", "--output", "o", "--apis", "d3d11"])
    assert not old.cross and old.frames == 8 and old.apis == ["d3d11"], "the A/B invocation no longer parses"
    print("PASS host_check selftest: SAME within noise, three codes off DIFFERS, alternating frames flicker, "
          "shared noise tolerated, the A/B invocation parses")


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
                                             args.output / api / name / k, keys)[0], np.uint8)
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


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("a", "b", "tools", "reshade", "runtime-dir", "input", "output", "addon32", "tools32", "reshade32"):
        p.add_argument("--" + name, type=Path)
    p.add_argument("--apis", nargs="+", choices=APIS, default=[a for a in APIS if a != "d3d9"])
    p.add_argument("--cross", action="store_true", help="one build (--b) across the APIs")
    p.add_argument("--frames", type=int, default=8, help="--cross: presents kept in a row per API")
    p.add_argument("--selftest", action="store_true", help="the verdicts on made-up captures; no GPU")
    return p


if __name__ == "__main__":
    args = parser().parse_args()
    if args.selftest:
        selftest()
        raise SystemExit(0)
    need = ("b", "tools", "reshade", "runtime_dir", "input", "output") + (() if args.cross else ("a",))
    if missing := [n for n in need if getattr(args, n) is None]:
        parser().error("needs --" + ", --".join(m.replace("_", "-") for m in missing))
    if args.cross and args.addon32 and not (args.tools32 and args.reshade32):
        parser().error("--addon32 needs --tools32 and --reshade32")
    (cross if args.cross else run)(args)
