#!/usr/bin/env python3
# Real-frame-path lens check. Exits non-zero if any claim in the verdict is false.
# Run: python check_lens.py
import hashlib, os, re, subprocess, sys, json

DLL  = r"D:/pcsx2-v2.8.2-test/dlssnr_amd_pass1.dll"
REPO = r"C:/Users/claudinhh/Desktop/dlss5amdrework/repo"
ROCM = r"C:/Program Files/AMD/ROCm/7.1/bin/llvm-objdump.exe"
HERE = os.path.dirname(os.path.abspath(__file__))
fails = []
def ck(name, cond, got=""):
    print(("ok   " if cond else "FAIL ") + name + ("  " + str(got) if got else ""))
    if not cond: fails.append(name)

blob = open(DLL, "rb").read()

# C1 the running DLL is the PATCHED one, not the original the reports quote
sha = hashlib.sha256(blob).hexdigest()
patches = json.load(open(os.path.join(REPO, "tools/runtime-patches.json")))
src = open(os.path.join(REPO, "core/addon/neural.cpp"), encoding="utf8", errors="ignore").read()
pin = "".join(re.findall(r"0x([0-9a-f]{2})", src.split("kRuntimeSha256[32] = {")[1].split("};")[0]))
ck("C1 shipped DLL sha256 == neural.cpp kRuntimeSha256 (patched build)", sha == pin, sha[:16])
ck("C1b shipped DLL sha256 != runtime-patches original_sha256", sha != patches["original_sha256"])

# C2 per-arch: do the kernels address module-scope globals?
ARCHES = {"gfx1201": (0x603200, 425120, "gfx1201"),
          "gfx1200": (0x59b200, 425120, "gfx1200"),
          "gfx11gen": (0x603200, 425120, "gfx1201")}
for name, (off, size, cpu) in ARCHES.items():
    co = os.path.join(HERE, name + ".co")
    open(co, "wb").write(blob[off:off + size])
    txt = subprocess.run([ROCM, "-d", "--triple=amdgcn-amd-amdhsa", "--mcpu=" + cpu, co],
                         capture_output=True, text=True).stdout
    lines = txt.split("\n")
    tgts = {}
    for i, l in enumerate(lines):
        if "s_getpc_b64" not in l: continue
        nxt = add = None
        for j in range(i + 1, i + 6):
            if nxt is None:
                m = re.search(r"// ([0-9A-F]{8,}):", lines[j])
                if m: nxt = int(m.group(1), 16)
            m2 = re.search(r"s_add_u32 s\d+, s\d+, (0x[0-9a-f]+)", lines[j])
            if m2 and add is None: add = int(m2.group(1), 16)
        if nxt is None or add is None: continue
        if add >= 0x80000000: add -= 0x100000000
        tgts[(nxt + add) & 0xFFFFFFFFFFFF] = tgts.get((nxt + add) & 0xFFFFFFFFFFFF, 0) + 1
    lut = tgts.get(0xAA80, 0)            # g_e4m3_lut, symbol value 0xAA80 in every arch
    if name.startswith("gfx12"):
        ck(f"C2 {name}: no kernel addresses a module global (our own module is safe)",
           sum(tgts.values()) == 0, tgts)
    else:
        ck(f"C2 {name}: kernels DO read g_e4m3_lut -> our zero copy would be wrong",
           lut > 50, f"{lut} sites at 0xAA80")

# C3 the shipping pre/post path is k_swin_var, not k_pre_block: the env switch is never set
hits = subprocess.run(["grep", "-rl", "--exclude-dir=spike", "--exclude-dir=.git", "DLSSNR_SLOW_PREPOST", REPO], capture_output=True, text=True).stdout
ck("C3 DLSSNR_SLOW_PREPOST set nowhere in the add-on tree", hits.strip() == "", hits.strip())

# C4 nothing the add-on owns is importable into HIP, and its fence is not shared
ck("C4 CreateTexture uses D3D12_HEAP_FLAG_NONE", "D3D12_HEAP_FLAG_NONE, &rd, initialState" in src)
ck("C4b no CreateFence with D3D12_FENCE_FLAG_SHARED on g.fence",
   src.count("CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))") == 2)

# C5 the controls the add-on writes are the option struct, not the engine object
off = open(os.path.join(REPO, "core/addon/runtime_offsets.h"), encoding="utf8", errors="ignore").read()
ck("C5 kLocalTone is 0x97b30 (option struct), not 0x96f98 (engine object)",
   "kLocalTone = 0x97b30" in off and "0x96f98" not in off)

print(("\nLENS CHECK: PASS" if not fails else "\nLENS CHECK: FAIL -> " + "; ".join(fails)))
sys.exit(1 if fails else 0)
