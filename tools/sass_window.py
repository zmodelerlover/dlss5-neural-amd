r"""Parse a cuobjdump -sass listing into (addr, mnemonic, operands) and offer windows
around any instruction matching a pattern. Used to read cg2r_post_process_kernel.
"""
import re
import sys

LINE = re.compile(r"^\s*/\*([0-9a-f]+)\*/\s+(\S+)\s*(.*?)\s*;\s*$")


def parse(path):
    out = []
    for raw in open(path, "r", encoding="utf-8", errors="replace"):
        line = raw.split("/* 0x")[0]
        m = LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        out.append((addr, m.group(2), m.group(3)))
    return out


def fmt(ins):
    return "%06X  %-22s %s" % (ins[0], ins[1], ins[2])


def main():
    path = sys.argv[1]
    pat = re.compile(sys.argv[2])
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 6
    ins = parse(path)
    print("# %d instructions" % len(ins))
    hits = [i for i, x in enumerate(ins) if pat.search(x[1] + " " + x[2])]
    print("# %d match(es)\n" % len(hits))
    shown = set()
    for i in hits:
        lo, hi = max(0, i - before), min(len(ins), i + after + 1)
        if all(j in shown for j in range(lo, hi)):
            continue
        print("---- around %06X ----" % ins[i][0])
        for j in range(lo, hi):
            print(("  >>" if j == i else "    ") + fmt(ins[j]))
            shown.add(j)
        print()


if __name__ == "__main__":
    main()
