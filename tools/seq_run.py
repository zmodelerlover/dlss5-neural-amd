"""Run framecheck --seq over sequences and measure each run. The temporal counterpart of ab_check.

    python tools/seq_run.py --exe build/amd-nr-framecheck.exe --runtime-dir D:/bench \
        --corpus corpus --out runs/base [--only pan-int,cut] [--set History=0 ...] \
        [--arg skip=1 ...] [--spatial runs/off] [--with ffx-dlls] [--keep]

One private folder per sequence under --out: the exe, the runtime and weights (hard-linked when the
volume allows, so a 147 MB weights file is not copied forty times), an amd-nr.ini of BASE plus
--set, then temporal_metrics over the captures. metrics.json lands beside them and the raw captures
are deleted unless --keep, because a sequence of them is half a gigabyte. --spatial points at an
earlier run of the same sequences with Temporal forced off, for the ghosting and HUD measures.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import temporal_metrics as tm  # noqa: E402

BASE = dict(Scale=1.0, Passes=1, Encoding=0, Tonemap=0, Structure=1.0, Skin=-1.0, Intensity=1.0,
            AutoMask=1, EngineScale=0.03125, ToneChannels=0, Inline=1, Bicubic=1, Motion=1,
            History=1, Temporal=0, Depth=0, GameGuides=0, Guard=2, GuardPerPass=0,
            ColourStrength=1, ResidualLimit=0.25, EdgeFade=0, PassTaper=0, DebugView=0,
            SerialPasses=1, Tone=1, Style=0, StyleStrength=1, NetworkOutput=0,
            FixedSeed=0, OutputSmooth=0)  # the defaults before Temporal=2 FixedSeed=1 OutputSmooth=0.8
RUNTIME = ("dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin", "dlssnr_on_amd.ini")


def place(src, dst):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def run_one(args, seq_dir, here, keys, fc_args):
    for attempt in range(1, 4):
        if here.exists():
            shutil.rmtree(here)
        here.mkdir(parents=True)
        shutil.copy2(args.exe, here / "amd-nr-framecheck.exe")
        for name in RUNTIME:
            place(args.runtime_dir / name, here / name)
        for extra in (args.with_dir.iterdir() if args.with_dir else ()):
            place(extra, here / extra.name)
        ini = {**BASE, **keys}
        (here / "amd-nr.ini").write_text("[amd-nr]\n" + "".join(f"{k}={v}\n" for k, v in ini.items()))
        with (here / "stdout.log").open("w") as log:
            done = subprocess.run([str((here / "amd-nr-framecheck.exe").resolve()), "--seq",
                                   str(seq_dir.resolve()), *fc_args], cwd=here, stdout=log,
                                  stderr=subprocess.STDOUT, timeout=1800)
        if done.returncode != 0:
            raise RuntimeError(f"{here}: framecheck exited {done.returncode}; see stdout.log")
        with (here / "seq.csv").open() as f:
            rows = list(csv.DictReader(f))
        # A watchdog fallback preserves the input instead of applying the network: that run says
        # nothing about the network, so it is retried rather than measured.
        fired = [r["frame"] for r in rows if r["ran"] == "1" and int(r["watchdog_job"]) >= int(r["job"]) > 0]
        if not fired:
            return
        print(f"  {here.name}: watchdog on frames {fired[:5]}, attempt {attempt}", flush=True)
    raise RuntimeError(f"{here}: watchdog fired in three attempts")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("exe", "runtime-dir", "corpus", "out"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--only", default="")
    p.add_argument("--set", action="append", default=[], help="ini Key=Value over BASE")
    p.add_argument("--arg", action="append", default=[], help="framecheck key=value")
    p.add_argument("--spatial", type=Path)
    p.add_argument("--with", dest="with_dir", type=Path, help="a folder whose files go beside the exe too")
    p.add_argument("--kind", default="runtime")
    p.add_argument("--keep", action="store_true")
    args = p.parse_args()
    keys = dict(kv.split("=", 1) for kv in args.set)
    fc_args = list(args.arg)
    if not any(a.startswith("dump=") for a in fc_args):
        fc_args.append("dump=runtime,composed,motion" if args.kind == "composed" else "dump=runtime,motion")
    only = [s for s in args.only.split(",") if s]
    seqs = sorted(d for d in args.corpus.iterdir() if (d / "meta.json").exists() and (not only or d.name in only))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "case.json").write_text(json.dumps(dict(set=keys, arg=fc_args), indent=1))
    table = {}
    for seq_dir in seqs:
        here = args.out / seq_dir.name
        run_one(args, seq_dir, here, keys, fc_args)
        spatial = tm.Run(args.spatial / seq_dir.name) if args.spatial else None
        summary, frames = tm.analyse(tm.Seq(seq_dir), tm.Run(here), spatial, args.kind)
        (here / "metrics.json").write_text(json.dumps(dict(summary=summary, frames=frames), indent=1))
        if not args.keep:
            for raw in here.glob("*.raw"):
                raw.unlink()
        table[seq_dir.name] = summary
        ti = summary.get("ti_out") or {}
        print(f"{seq_dir.name:14} TI_out {1000 * (ti.get('mean') or 0):6.2f}e-3  gain {summary.get('gain') or 0:5.2f}"
              f"  detail {summary.get('detail') or 0:5.3f}  EPE {summary.get('epe_median') or 0:5.2f}", flush=True)
    (args.out / "summary.json").write_text(json.dumps(table, indent=1))


if __name__ == "__main__":
    main()
