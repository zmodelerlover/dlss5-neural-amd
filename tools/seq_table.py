"""One metric of several seq_run cases side by side, as a markdown table.

    python tools/seq_table.py ti_out.mean runs/spatial runs/base runs/of [--scale 1000] [--ratio runs/base]

The metric is a dotted path into each sequence's summary (ti_out.mean, ti_out.gt4, detail,
epe_tex_median, disocclusion, convergence ...). --ratio divides every case by that one, per sequence.
"""
import argparse
import json
from pathlib import Path


def pick(summary, path):
    v = summary
    for key in path.split("."):
        v = v.get(key) if isinstance(v, dict) else None
    return v


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("metric")
    p.add_argument("cases", nargs="+", type=Path)
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--ratio", type=Path)
    a = p.parse_args()
    tables = {c: json.loads((c / "summary.json").read_text()) for c in a.cases}
    base = json.loads((a.ratio / "summary.json").read_text()) if a.ratio else None
    seqs = sorted({s for t in tables.values() for s in t})
    names = [str(c).replace("\\", "/").split("runs/")[-1] for c in a.cases]
    print(f"| {a.metric} | " + " | ".join(names) + " |")
    print("|---|" + "---:|" * len(names))
    for s in seqs:
        cells = []
        for c in a.cases:
            v = pick(tables[c].get(s, {}), a.metric)
            if v is not None and base is not None:
                b = pick(base.get(s, {}), a.metric)
                v = v / b if b else None
            cells.append("" if v is None else (f"{v:.3f}" if base else f"{v * a.scale:.3g}"))
        print(f"| {s} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
