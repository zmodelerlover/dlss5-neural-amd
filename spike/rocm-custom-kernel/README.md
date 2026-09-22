# Spike: can a custom ROCm kernel lift the precompiled-HIP limitation?

Run 22/09/2026, against `handoffs/HANDOFF-style-preset-fechado-e-roadmap-20260922.md` §3.1, which
closed the NR Style question with:

> Não há como o addon dar o lado "rede" do Model: os kernels HIP do port são GCN pré-compilado sem
> fonte, e o controle não existe na assinatura deles. Só quem compilou o port poderia acrescentar.

**Thesis tested:** that limitation can be lifted by writing a custom kernel with the ROCm APIs.

**Verdict: the limitation is real but its stated reason is false, and a custom kernel is the wrong
tool for it.** Both halves are measured, and both are reproducible from the checks in this folder.

---

## 1. What the checks establish

Every claim below has one runnable check beside it that exits non-zero if the claim is false.
Nothing here ran in a game; everything is headless and standalone.

| # | Claim | Check | Result |
|---|---|---|---|
| 0 | The ROCm toolchain here builds and runs a custom `gfx1201` kernel | `hipenv.sh` + any check | PASS |
| 1 | The port's own code object loads under `hipModuleLoadData` and its kernels run under our launches | `check_module_load.cpp` | PASS |
| 2 | `hiprtc` compiles a kernel at runtime for whatever card is present | `check_hiprtc.cpp` | PASS, route rejected |
| 3 | A custom kernel shares D3D12 VRAM with no CPU round trip | `check_d3d12_interop.cpp` | PASS, route redundant |
| 4 | The port's network has a **fifth trained conditioning input**, hard-coded to `0.0f` | `weights_lane_columns.py`, `probe_preparams*.cpp` | PASS |
| 5 | The kernel where #4 was measured is **not the one the game runs** | `check_claims.py` C3 | PASS (i.e. #4 is on a debug path) |
| 6 | Driving the port's kernels from our own module is **silently wrong on RDNA3** | `check_claims.py` C2 | PASS (a real landmine) |

`check_claims.py` bundles nine assertions and currently exits 0. `check_claims_teeth.py` is the
same file with one input deliberately swapped; it must report FAIL, and does. `check_module_load.exe`
has three independent teeth — bogus symbol, wrong architecture, swapped struct fields — each
demonstrated to exit 1.

## 2. The kernels were never opaque

`tools/carve_amd_kernels.py` lifts the clang offload bundle out of the runtime (file offset
`0x98200`, nine targets, plain uncompressed AMDGPU ELFs — no zstd, no per-entry fatbin). An AMDGPU
code object keeps its metadata in a note section, so unlike a CUDA cubin it names every kernel and
every argument without a line of disassembly:

    python tools/carve_amd_kernels.py <runtime.dll> out/
    llvm-readobj --elf-output-style=GNU --notes out/gfx1201.co

34 named kernels come out: `k_import`, `k_pre_block_1h_32_fp8`, `k_swin_var<32|64|128|256>`,
`k_qkv`, `k_qkv_attn`, `k_attention`, `k_ffwd`, `k_conv_res`, `k_expand`, `k_contract2`,
`k_dec_upsample`, `k_final_head`, `k_post_block_1h_32_fp8`, `k_export`, `k_reproject`, `k_repack`,
`k_mean`, and a GPU-side fence pair `k_flag_set` / `k_flag_wait`.

`check_module_load.cpp` then loads `gfx1201.co` **as a bare ELF** — no re-bundling needed — resolves
`_Z6k_mean10MeanParams` by mangled name, drives it with a reconstructed 32-byte `MeanParams`, and
gets `0.07641130` against a CPU Rec.709 reference of `0.07641130`. Padding columns are poisoned
with `1000.0f`, so a misread row pitch produces a wild answer, not a near miss. Swapping `h` and
`w` moves it to `0.05603288` and the check fails.

So "só quem compilou o port poderia acrescentar" is false **as an API statement**. Nothing about the
port being precompiled keeps us out of its kernels.

## 3. The real finding: the fifth input already exists

`k_pre_block` builds a 16-lane fp16 feature vector per pixel in LDS and multiplies it by the first
linear layer. Lanes 0–2 are Gaussian noise, lane 3 the bias, lanes 4–9 the two colour images,
lanes 11–14 the four controls the add-on already drives (tone, structure, skinEff, structEff), and
lane 15 is hard-zeroed by the kernel itself.

**Lane 10 is a live input that the host hard-codes to zero** — one 10-byte instruction,
`mov dword ptr [rbp+238h], 0` at RVA `0x2D79B`, filling `PreParams+48`.

The question that decides whether that matters is whether lane 10's trained weight column is
non-zero in the shipped weights (`dlssnr_on_amd_weights.bin`, 147 689 451 bytes, magic `DLSSNRW1`,
153 tensors). `weights_lane_columns.py` reads it:

- lane 10 is non-zero in **every** row of the 16×16 input projection, rms ≈ 0.22–0.27 —
  **above all four shipped controls** (0.091 / 0.066 / 0.168 / 0.165).
- lane 15 — the one the kernel hard-zeroes — has an **exactly zero** column across those same 16
  rows. That is a built-in negative control: the trainer does zero out unused columns, so lane 10
  being non-zero is not an artefact.

`probe_preparams_real_weights.cpp` confirms it by execution, splicing the real tensor in as the
arena: driving `+48` changes more output than any shipped control, with a monotone dose response
(0.001 → 19 016 bytes, 0.01 → 49 472, 0.1 → 96 447, 0.9 → 129 033), while the dead pad at `+52`
changes exactly 0 and repeat runs are bit-identical.

**So §3.1's "A rede recebe 4 controles e nada mais" is measurably wrong.** There is a fifth trained
conditioning input, the weights for it shipped, and the AMD port ties it to zero.

## 4. Two reasons the spike does not validate its own thesis

### 4.1 A custom kernel cannot reach it

The 16-lane vector exists **only in LDS** — built with `ds_store` at `0xE080 + tid*32`, consumed by
`ds_load` into the `v_dot2_f32_f16` chain. It is never written to global memory. No kernel running
beside the network, no IAT hook, no shared D3D12 buffer and no stream ordering can touch it. The
three interop rungs all pass and are all structurally incapable of carrying the thesis.

What actually reaches lane 10 is a **host-side store into a parameter struct** — no kernel at all.
A custom kernel that pre-modulates the input image instead would be a gain in front of an encoder,
i.e. another grade, just earlier.

### 4.2 The struct measured is behind an environment variable

    18002d6ba  80 3d 1f a9 06 00 01   cmp byte ptr [rip+0x6a91f], 1   ; -> 0x97fe0
    18002d6cb  0f 85 57 01 00 00      jne 0x2d828                     ; skips [0x2d6d1, 0x2d828)
    18002d79b  c7 85 38 02 00 00 ...  mov dword ptr [rbp+238h], 0     ; the +48 zero-write
    18002d81c  48 8d 0d 25 6b 03 00   lea rcx, [0x64348]              ; k_pre_block stub
    18002d9aa  48 8d 0d b7 e2 03 00   lea rcx, [0x6bc68]              ; k_swin_var<32,true> stub

`0x97fe0` is written once, by `setne` after `getenv("DLSSNR_SLOW_PREPOST")`, and read exactly twice
— immediately before the pre and the post block. Unset (the default) means the `jne` is taken and
the whole `PreParams` fill, the `+48` zero-write **and** the `k_pre_block` launch are jumped over.
The shipping path falls through to `k_swin_var<32,true>` with a 168-byte `VarParams` whose float
lanes nobody has mapped. The string appears once in the runtime, nowhere in the add-on tree and
nowhere in the ini (`check_claims.py` C3).

`k_pre_block` is a debug path. Everything in §3 is true and is about a kernel the game never
dispatches.

Worth recording: the handoff's own §2.3 called `off_18006BC68` the pre kernel and it was **right** —
that is the stub the default path loads. The reading that "corrected" it was the wrong one.

## 5. A landmine found on the way

`g_e4m3_lut` (512 bytes) is **all zero in the shipped image**; the runtime fills its own module's
copy at init. Resolving every `s_getpc_b64`+`s_add_u32` pair across the carved objects: gfx1201 and
gfx1200 have **zero** such sites — the compiler inlined everything — but `gfx11-generic` has 82, of
which **79 land exactly on `g_e4m3_lut`**, including 9 inside `k_swin_var<32,true>`, the shipping
pre/post kernel.

So on RDNA3 — gfx1100/1101/1102, most of the add-on's user base — launching the port's kernels out
of a module *we* loaded dequantises every fp8 weight through a table of zeros, with `hipSuccess`
everywhere and no error. Rung 1 could not see this: it ran only `k_align_probe` and `k_mean`, the
two kernels that touch no globals, on the one architecture where the inlining hides it.

The fix is one `hipModuleGetGlobal` plus a 512-byte fill before any launch. **Anyone taking the
custom-kernel route must do this.** (`check_claims.py` C2.)

## 6. Routes 2 and 3, measured and set aside

- **hiprtc** works and adapts to the card, but shipping it costs **111.8 MiB** of redistributable
  DLLs to save a 60 ms compile that a cached 5 KB `.co` replaces. The licence position could not be
  settled from the files on this machine. Rejected on its own numbers, not on taste.
- **D3D12 zero-copy** works for shared *buffers*. It is redundant: the runtime already bridges its
  textures into HIP with a GPU copy costing **0.04 ms/frame**, forty times cheaper than the
  1.58 ms pinned-system-RAM alternative. Its genuinely useful result is negative — a shared D3D12
  *texture* is not linearly addressable by a kernel, and the surface-object route **silently zeroes
  the allocation while returning `hipSuccess` from all five `hip*External*` calls**.

## 7. Corrections to the handoff and to this project's own notes

1. §3.1 "a rede recebe 4 controles e nada mais" — **false**. A fifth trained conditioning input
   exists and is hard-coded to zero (§3).
2. §3.1 "só quem compilou o port poderia acrescentar" — **false as an API claim** (§2). It remains
   true that the shipping kernel's equivalent slot is not yet located.
3. The shipped runtime is sha256 `70af3fb757f83f71…`, **not** `8321cae7…`. The latter is
   `original_sha256` in `tools/runtime-patches.json`, i.e. the *input* to `patch_runtime.py`. The
   35 differing bytes are exactly the three declared patches, none in `.hip_fat`, so the carved
   kernels are the vendor's and every RVA above holds. Consequence: `patch_runtime.py` would refuse
   the analysed binary, and any byte-patch plan also trips the add-on's own `RuntimeHashMatches`
   gate (`neural.cpp:756-800`, `kRuntimeSha256` at `neural.cpp:725`).
4. The add-on writes the controls at `0x97b30` — the runtime's **ini option struct** — not at
   `0x96f98`. The engine object is downstream of a propagation step whose timing against the worker
   thread is unestablished.
5. `off_18006BC68` is the `__hipRegisterFunction` host-stub slot for `k_swin_var<32,true>`, not a
   table of dispatch modes — but it *is* what the default path launches, so §2.3's conclusion stood.

## 8. What to do next, cheapest first

1. **Map `VarParams` on the kernel the game actually dispatches** (`k_swin_var<32,true>`, 168 bytes,
   424 kernarg). Find the field that feeds lane 10 there. Until this is done nothing in §3 is
   actionable. `probe_preparams.cpp` is the template — it probed `PreParams` field by field on the
   real GPU; the same harness points at `VarParams` with an hour's work.
2. **Settle what lane 10 *is* before driving it.** §3 proves it is *a* conditioning input, never
   *which one*. Calling it Style is the same adjacency reasoning that cost §2 a day, one level
   deeper. The handoff's own §4 names a rival nobody considered: `UICorrection` — NVIDIA default 0,
   RenoDX sends 1, "AMD não tem o campo". A lane hard-wired to `0.0f` fits that better than
   `style/128`, which with n=3 clamped to 2 yields only {0, 0.0078, 0.0156}.
3. **The discriminator already exists and needs no new code.** `DrainReadbacks`
   (`neural.cpp:2463-2474`) logs `measure, residual detail … ratio` = gRes/gIn. Sweep the candidate
   control and record mean residual **and** the ratio: a grade or a gain anywhere in the chain
   scales the residual and leaves the ratio roughly fixed; a genuine conditioning change moves
   where the network puts detail and shifts it. One existing log line.
4. If the custom-kernel route is revived for anything: **fill `g_e4m3_lut` first** (§5).

Not worth doing: reimplementing `k_pre_block` (it is not on the shipping path); shipping hiprtc;
flipping `CreateTexture` to `D3D12_HEAP_FLAG_SHARED` (§6 measured that artefact unusable).

## 9. What came out of this that is reusable regardless of the verdict

Added after the verdict, all measured here, none of it dependent on the thesis holding.

### 9.1 The port runs NVIDIA's network, byte for byte

`tools/read_amd_weights.py` parses `dlssnr_on_amd_weights.bin` (magic `DLSSNRW1`, 153 tensors,
`data_base` 5673). The parse is self-validating: the index ends exactly at `data_base`, and
`data_base + sum(sizes)` equals the file size exactly, 147 689 451 bytes.

Then, against NVIDIA's `nvngx_dlssnr.dll`:

- **all 153 tensor names appear verbatim** in it;
- a **25-tensor sample of the blobs is byte-identical** inside it (25/25), from `block0.layer0.layer`
  at 21 696 bytes up to `block37.layer1.layer` at 4 196 352.

So the port is not a lookalike reimplementation — it runs NVIDIA's own weights, lifted out of the
DLL. `docs/nvidia-parity.md` closes with "a aritmética é igual; se a imagem é, só a comparação diz."
Half of that is now settled: **the network is identical**. Any difference on screen is the
composition around it, the four-vs-five conditioning inputs (§3), and the fp8 kernels' arithmetic —
not the model. That narrows roadmap item 2 from "are they the same?" to "where does the composition
diverge?".

    python tools/read_amd_weights.py <weights.bin> --nvidia <nvngx_dlssnr.dll>

### 9.2 Six undocumented debug knobs in the runtime

The runtime reads six environment variables. Each string has exactly one xref; four set a byte in
`.data` with `setne`, and the mechanism below was read out of the instruction stream, not guessed.
**What they do to the image is not measured — each is a candidate for one measurement, not a claim.**

| Variable | Flag byte | Read at | Mechanism, as read |
|---|---|---|---|
| `DLSSNR_SLOW_PREPOST` | `0x97fe0` | `0x2d6ba`, `0x2f606` | picks the dedicated `k_pre_block`/`k_post_block` over the fused `k_swin_var<32,true>` (§4.2) |
| `DLSSNR_NOBLEND` | `0x97fe8` | `0x2f559` | `pxor xmm6,xmm6` / `jne` skips `movaps xmm6,xmm7` — forces one post-stage float to 0 |
| `DLSSNR_NOPOSTHIST` | `0x97ff0` | `0x2f572` | `xor esi,esi` / `cmove rsi,rax` — passes NULL instead of the post-stage history pointer |
| `DLSSNR_WBLOG` | `0x98000` | `0x32b28` | gates weight-loading logging; `"missing blob %s"` sits in the same handler |
| `DLSSNR_STAGES` | (value) | `0x190f6` | not a boolean — the string is parsed into a value |
| `DLSSNR_NO_REPACK` | (inline) | `0x20315` | boolean, branches inline rather than storing a flag |

**Measured, and then rejected — do not re-run this.** God of War 2 from a save state, network
raster 653x330, two baselines giving a noise floor of +/-6% on the residual mean and +/-4% on the
detail ratio:

| run | residual mean | vs baseline | detail ratio | vs baseline |
|---|---|---|---|---|
| baseline A | 0.0314 | — | 0.211 | — |
| baseline B | 0.0297 | −5.6% | 0.203 | −3.8% |
| `DLSSNR_NOBLEND` | 0.0521 | **+66%** | 0.268 | +27% |
| `DLSSNR_NOPOSTHIST` | 0.0411 | **+31%** | 0.247 | +17% |

So the runtime spends between a third and two thirds of the effect's strength buying temporal
stability, and both switches buy that strength back. The user then looked at the screen: both add
visible flicker, and the trade was judged not worth it. **The idea is closed.** The controls were
built as two overlay checkboxes writing the bytes live, measured, and then removed again; the
source is back to where it was and the build is byte-for-byte the same size.

Two things worth keeping from the attempt:

* The bytes really are live. Writing `0x97fe8` per frame from the add-on moved the residual +31.5%
  against a +/-6% noise floor, so the runtime does re-read them every evaluation and they do not
  need a relaunch. But it did **not** reproduce the environment-variable path (0.0413 against
  0.0521) and nobody established why. If this is ever reopened, that gap is the first thing to
  settle, not the flicker.
* The "double damping" hypothesis that motivated it was **wrong, and was checked too late**:
  `PersistentResidual` / `ResidualHold` / `ResidualMaxAge` appear in the bench's ini and in the old
  1.2 MB `ORIG-addon64.bak`, but in no source file, no committed revision and neither current
  binary. The add-on has no temporal damper of its own. The ini keys are dead leftovers. Reading a
  feature's existence off an ini key is the same class of mistake as section 2.5.

`DLSSNR_SLOW_PREPOST` is immediately useful as a diagnostic: it is the only way to run the path
whose parameter struct is fully mapped (§3), so a question about what a conditioning input does can
be asked on the slow path first and only then re-asked on the shipping one. `DLSSNR_WBLOG` is the
cheap way to cross-check `read_amd_weights.py` against the runtime's own view of the file.

### 9.3 A method upgrade, not a feature

"Precompiled GCN with no source" closed questions in this project. It should not have: an AMDGPU
code object carries its metadata in a note section, so `carve_amd_kernels.py` plus
`llvm-readobj --notes` answers "what does this kernel take and what is it called" with no
disassembly at all. Future questions about the runtime's internals are now readable rather than
guessable, and §2 shows its kernels can be driven directly for a differential measurement.

### 9.4 Two negative results worth not re-deriving

- A shared D3D12 **texture** is not linearly addressable from a kernel, and the surface-object route
  **silently zeroes the allocation while returning `hipSuccess` from all five `hip*External*`
  calls**. Exchange through a shared *buffer* instead.
- The runtime already bridges its textures into HIP at **0.04 ms/frame**, against 1.58 ms for a
  pinned system-RAM round trip. Do not rebuild that bridge.

### 9.5 The method lesson, which is different from §2.5's

§2.5 was one confident reading that nobody measured. This time two readings **contradicted each
other** — one agent said `k_pre_block` was gated behind an env var, another "corrected" it — and the
more confident one won without anyone running anything. A contradiction between two readings is not
a debate to settle by argument; it is a measurement request. It cost most of this spike's yield.

---

## Running the checks

    source spike/rocm-custom-kernel/hipenv.sh        # MSVC + SDK paths for hipcc
    python tools/carve_amd_kernels.py <runtime.dll> out/

    hipbuild -o check_module_load.exe check_module_load.cpp
    ./check_module_load.exe out/gfx1201.co                       # 0 = PASS
    ./check_module_load.exe out/gfx1100.co                       # 1, wrong arch
    ./check_module_load.exe out/gfx1201.co _Z13k_align_probeXX   # 1, bogus symbol

    python check_claims.py          # 0 = all nine claims hold
    python check_claims_teeth.py    # must report FAIL
    python weights_lane_columns.py  # per-lane weight column magnitudes

`hipenv.sh` hardcodes this machine's MSVC and Windows SDK paths; edit the two lines if they move.
No binaries ship with this project — the runtime and the weights are yours.
