# Model A / B / C: what the style vector actually does

`DLSSNR.Style` is an NGX parameter, not an ini key. It selects Neural Rendering Model A, B or C.
RenoDX exposes it and it visibly changes the image on NVIDIA hardware.

## Correction, 21/09/2026: Style is a network control first and a grade second

Everything below about the grading vector stands, byte for byte. What was wrong is the sentence
"not network conditioning". It is both, and the network half is the one that changes the picture:

- `CG2RNetworkManager::Evaluate` (`0x180021BB0`) reads `Style` from `opts+0xEC`, clamps it
  **unsigned** to `n-1` where `n` is the descriptor's style count (`descriptor+0x64`), and hands
  the forward pass `style * 0.0078125` (= style/128) beside LocalTone, LocalStructure and the
  derived skin/structure pair. Measured at runtime on NVIDIA: `n = 3`, so the network takes
  0, 1 and 2 -- exactly Model A, B, C -- as controls 0, 0.0078125 and 0.015625. Anything else,
  negative included, silently becomes 2.
- **The AMD runtime does not have that slot -- corrected 22/09.** For one day (builds 21/09
  22:24 to 22/09 00:04) this document and the add-on said it did: the worker (`sub_180018670`,
  `0x180019070..0x1800190E5`) builds four control floats at `0x96F98..0x96FA4` (tone, structure
  raw, skinEff, structEff) and copies `97b3c` -- the ini key `Scale`, default `0.03125` -- into
  the float right after them, `0x96FA8`, which looked like a fifth control. It is not one. The
  runtime's own log line prints `ctl (%.2f %.2f %.2f %.2f)`, four values; and in the evaluate
  (`sub_18002D2D0`) object+32..44 go to the *pre* kernel that feeds the network while object+48
  (`0x96FA8`) goes to the *post* kernel that writes the output, as the argument after the history
  pointer. Writing `style/128` there made Model A (0) return its input unchanged -- measured in
  ETS2: residual mean 0.00024 against an input mean of 0.45, "enabled" and "disabled" identical,
  every frame reported processed -- and cut Models B and C to a quarter and a half. The add-on
  writes the runtime's default `1/32` again, refuses values near zero, and measures the residual
  every 1800 frames so this class of failure is reported instead of found by eye.
  **On AMD a Model is its grade and nothing else.** The network half has no input to reach: the
  original conclusion of this document stands for this runtime.
- Measured on NVIDIA hardware (ETS2, D3D11 via RenoDX's D3D12 proxy, RTX 3050, driver 610.62;
  `handoffs/RESULTADO-nvidia-preset-style-ets2-20260921.md`): switching Model writes **only**
  `DLSSNR.Style` plus a one-frame `DLSSNR.Reset` pulse. Tone, Structure, Skin, Intensity, AutoMask
  and Preset are untouched. RenoDX defaults: Style 0, Intensity 1, LocalTone 1, LocalStructure 1,
  Skin 1, AutoMask 1, MVecScale 1/1, DepthInverted 0 (the DLL's own default is 1; RenoDX sends 0
  explicitly), Preset 1.
- `CG2R_ResetTemporalHistoryOnControlChange` (`0x1800179D0`) drops the history when Style or
  UseAutoMask change (exact) or LocalTone, LocalStructure, Skin, skinEff or structEff move by more
  than `1e-5`. **Not** on Intensity or MVecScale. 27 resets in the trace, all `control change`.
  The add-on does the same in one place, `ControlsChanged()`, where the controls are written.
- The derived pair (`0x1AA4B..0x1AAA1`): with `UseAutoMask != 0`, `skinEff = Skin >= 0 ? Skin :
  LocalStructure` and `structEff = LocalStructure`; with it off, **both** are `-1.0`. A present
  `ControlMask` forces UseAutoMask to 0. On AMD this derivation lives inside the runtime worker,
  not in the add-on.
- The grading vectors are in `.rdata`, not the kernel: the 648-byte entry at `0x1800B0D80` carries
  one `0x44`-byte block per non-identity style -- style 1 at `+0x64` (exposure `-0.10`, contrast
  `-0.25`, saturation `-0.10`), style 2 at `+0xA8` (saturation `-0.15`) -- and its header has
  `+0x24 = 3`, the same `n`. The same entry holds the weight name, id 1, `WEIGHTS_HT` and
  `CC_SILVER_AARDWOLD`; those are fields of one record, not different DLLs.
- **NR Preset is closed on both sides.** The NGX log prints `1 config(s) available` once per
  `CreateFeature`, both features report `preset=1 -> CC_Control_History_Blend_Quantize_With_Teacher_
  honest_tench_2026_07_04_22_30_weights`, and the fallback line never appears because 1 is the
  only entry. `CG2RFindWeightByPreset` walks a one-entry table. There is nothing for an AMD
  control to select; Deep Fried Chicken's `NRPreset` combo has no effect on the shipping DLL.
- `GlobalToneStrength` is not read by this DLL (the 60 `DLSSNR.*` names it consults are in
  `sub_180019F30`); it is a Streamline ABI field. RenoDX sends it; nothing happens.

The NVIDIA machine's DLL was the pre-RTX-50 patched build (sha256 `E67DEE20…`, same 165 840 496
bytes as the `e16bcf15…` analysed below); the style table and `Evaluate` match the reading here.

The rest of this document is the grading half and remains the reference for the compose shader.
Its "Still closed" section at the end is right for this runtime: the grade is all a Model can be
here.

Two earlier passes concluded the feature was out of reach, and a third one here nearly repeated
that conclusion from the DLL's C++ alone. It was wrong. Disassembling the CUDA kernel shows the
style vector is **colour grading on the output RGB** and that Models B and C are three scalar
knobs our own composition stage can apply. No HIP backend is involved for that half.

Reference binary: `nvngx_dlssnr.dll`, 165 840 496 bytes,
sha256 `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`, image base `0x180000000`.

## Two parameters, not one: Style is not Preset

The DLL takes both, and they are unrelated:

| parameter | what it selects | strings in the DLL |
|---|---|---|
| `DLSSNR.Style` | the fourteen-float grading vector below -- Model A, B or C | the descriptor table at `0x1800B0D80` |
| `DLSSNR.Hint.Render.Preset` | **a set of weights**, through `CG2RFindWeightByPreset` | one entry: `CC_SILVER_AARDWOLD`, `WEIGHTS_HT`, `CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights` |

This build ships exactly one weight set, so every preset but the default takes the fallback:

```
DLSSNR: preset %d is not available in this DLL build; falling back to shipping default preset %d ('%s')
DLSSNR: CG2RFindWeightByPreset(%d) returned null; using descriptor [0] '%s' as last-resort fallback
```

### "Natural" and "Cinematic" are these same three, renamed

The DLL itself carries no named looks -- a search of `.text` and `.rdata` for those words returns
nothing, and the only names in it are the weight tags above. The names come from the consumers,
and there are two conventions for one field:

| value | RenoDX | Deep Fried Chicken |
|---|---|---|
| 0 | Model A | Default |
| 1 | Model B | **Natural** |
| 2 | Model C | **Cinematic** |

RenoDX's string is *"Selects Neural Rendering Model A, Model B, or Model C through the prerelease
DLSSNR.Style field."* Deep Fried Chicken's panel writes `NRStyle`, a three-entry combo; AMDNR-Feeder
mirrors that panel one-for-one and has the list verbatim from the add-on's string table
(`src/amd-nr-feed32.cpp`):

```c
static const char *const kNRStyleItems[] = { "Default", "Natural", "Cinematic" };
...
{ "NRStyle", "NR Style", NR_COMBO, 0.0f, 0.0f, 2.0f, nullptr, kNRStyleItems, 3,
  "Default keeps the add-on's own choice; Natural and Cinematic force it." },
```

Combo index is the field value, so Natural is `DLSSNR.Style = 1` and Cinematic is `2`. Nothing is
different about them; the two projects picked different labels for the same descriptors.

Beside it, Deep Fried Chicken has a separate `NRPreset` -- `{ "Default", "Preset #1", "Preset #2",
"Preset #3" }` -- and *that* is the weight hint. Confusing the two is easy: one changes colour and
costs nothing, the other would change the network and has only one option to change it to.

The practical consequence: Model A/B/C is a grading choice and reproducing it needs no weights,
which is why it is in the compose shader. A *preset* would be a different network, and there is
only one in this build to have.

## The table, read from the image

`sub_18001D7C0` picks a descriptor; `sub_18001D5F0` applies it:

```c
float t = opts[57];                  // LocalToneStrength, clamped to [0,1]
uint mask = *(uint *)desc;
if (mask & 0x0001) opts[73] = (desc[1] - 0.0f) * t + 0.0f;
if (mask & 0x0002) opts[74] = (desc[2] - 1.0f) * t + 1.0f;   // the only neutral that is not 0
...
if (mask & 0x2000) opts[86] = (desc[14] - 0.0f) * t + 0.0f;
```

Eight slots of 68 bytes at `record+100`, `record` = `0x1800B0D80`, count at `record+36` = 3,
Model A's fallback descriptor at `record+40`:

| | descriptor | mask | overrides |
|---|---|---|---|
| Model A | `record+40` | `0x00` | nothing. Neutral vector `{0, 1.0, 0 x12}`, and a zero mask writes no slot. |
| Model B | `record+108` | `0x34` | `opts[75] = -0.10`, `opts[77] = -0.25`, `opts[78] = -0.10` |
| Model C | `record+176` | `0x20` | `opts[78] = -0.15` |

Six of eight slots are `valid = 0`. Three models, two entries, because Model A is the absence of an
entry. One set of weights, not three: 156 `block*` tensor names, each appearing once.

## Where the fourteen floats go

`sub_18001C920` copies them contiguously into a `NGXCubinParameterStruct<CG2RPostProcessParams>`:

```c
v63 = *(_OWORD *)((char *)a4 + 292);                        // opts[73..76]
*(_OWORD *)((char *)&v106[39] + 4) = v63;                   // -> params +316
*(_OWORD *)((char *)&v106[41] + 4) = *(_OWORD *)(a4 + 308); // -> params +332
*(_OWORD *)((char *)&v106[43] + 4) = *(_OWORD *)(a4 + 324); // -> params +348
HIDWORD(v106[45]) = *((_DWORD *)a4 + 85);                   // -> params +364
LODWORD(v106[46]) = *((_DWORD *)a4 + 86);                   // -> params +368
```

56 bytes at struct offsets 316..371 of a 376-byte (`0x178`) parameter block.

`sub_1800176E0` tests that vector against neutral, and its answer is one of four conditions that
decide whether the pass is recorded at all:

```c
bool sub_1800176E0(float *a1) {
  return fabs(a1[73] - 0.0f) > 1e-5f || fabs(a1[74] - 1.0f) > 1e-5f || ... ;
}
...
result = sub_1800176E0(a4);
if ( !v17 && !v18 && !(_BYTE)result && !v13 ) return result;   // nothing recorded
```

At Model A the vector is neutral, so as far as style is concerned there is nothing to run.

## Reading the kernel

The kernels are not greppable in the DLL because each fatbin entry is **zstd-compressed** -- which
is why the first look found "no readable kernels" and nearly closed the question. Decompressed,
they are named. `tools/carve_dlssnr_kernels.py` does the extraction.

```
python tools/carve_dlssnr_kernels.py nvngx_dlssnr.dll out/
nvdisasm -c out/cg2r_post_process_kernel_sm120.cubin
```

The parameter layout is authoritative from the cubin's own metadata, not inferred:

```
EIATTR_PARAM_CBANK -> cbank base 0x380, size 0x178
ordinal 0, offset 0, size 376        # the struct, passed by value
```

So struct byte *N* is `c[0x0][0x380 + N]`, which puts the style vector at `0x4bc .. 0x4f0`:

| slot | struct | SASS constant |
|---|---|---|
| `opts[73]` | +316 | `c[0x0][0x4bc]` |
| `opts[74]` | +320 | `c[0x0][0x4c0]` |
| `opts[75]` | +324 | `c[0x0][0x4c4]` |
| `opts[76]` | +328 | `c[0x0][0x4c8]` |
| `opts[77]` | +332 | `c[0x0][0x4cc]` |
| `opts[78]` | +336 | `c[0x0][0x4d0]` |
| ... | ... | ... |
| `opts[86]` | +368 | `c[0x0][0x4f0]` |

All fourteen are read (`LDCU.64` at `0x4c8` covers `[76]`+`[77]`; `LDCU.128` at `0x4e0` covers
`[82]`..`[85]`).

## What the kernel does with them

A colour-grading chain on the final RGB triple. Each operation is the identity at its neutral
value, which is why Model A is indistinguishable from the pass not running.

**`opts[73]` / `opts[74]` -- black and white point (levels):**

```
LDCU    UR5, c[0x0][0x4bc]        ; opts[73]
FADD    R0, R2, -UR5              ; rgb - black
LDCU    UR4, c[0x0][0x4c0]        ; opts[74]
UFADD   UR4, -UR5, UR4            ; white - black
UFADD   UR4, UR4, 1.0e-10         ; divide-by-zero guard
MUFU.RCP R9, UR4
FFMA.SAT R5, R0, R9, RZ           ; saturate((rgb - black) / (white - black))
```

Neutral `{0, 1}` gives `(x - 0) / 1` = x.

**`opts[75]` -- exposure, in stops:**

```
LDCU     UR4, c[0x0][0x4c4]
MUFU.EX2 R3, UR4                  ; exp2(opts[75])
FFMA.SAT R5, R3, R5, RZ           ; rgb *= exp2(opts[75])
```

Neutral 0 gives `exp2(0)` = 1.

**`opts[77]` -- contrast, as a blend toward a smoothstep S-curve:**

```
FADD  R6, R5, R5
FMUL  R4, R5, R5
FADD  R6, -R6, 3
FFMA  R4, R4, R6, -R5             ; x*x*(3 - 2x) - x  =  smoothstep(x) - x
FFMA  R0, R4, UR5, R5             ; x + opts[77] * (smoothstep(x) - x)
```

Neutral 0 leaves x. Negative values pull *away* from the S-curve, i.e. flatten contrast.

**`opts[78]` -- saturation, inside an HSL round trip:**

```
FADD     R9, R13, R14             ; max + min
FMUL     R2, R9, 0.5              ; L = (max + min) / 2
FSETP.GT P1, PT, R2, 0.5          ; which half of the lightness range
FADD     R15, R13, -R14           ; d = max - min
@P1  FADD R0, -R13, 2 ; FADD R3, -R14, R0 ; MUFU.RCP R3, R3   ; 1 / (2 - max - min)
@!P1 MUFU.RCP R6, R9                                          ; 1 / (max + min)
@P1  FMUL R0, R15, R3 ; @!P1 FMUL R0, R15, R6                 ; S
LDCU     UR4, c[0x0][0x4d0]
UFADD    UR4, UR4, 1              ; 1 + opts[78]
FFMA.SAT R11, R0, UR4, RZ         ; S' = saturate(S * (1 + opts[78]))
...
FSETP.GEU P0, PT, R2, 0.5
@!P0 FADD R3, R11, 1  ; FMUL R0, R2, R3      ; q = L * (1 + S')
@P0  FADD R5, R2, R11 ; FFMA R0, -R2, R11, R5 ; q = L + S' - L*S'
FADD     R3, R2, R2
FADD     R11, R3, -R0             ; p = 2L - q
```

**HSL, not HSV.** The lightness is `(max+min)/2` and the saturation denominator switches between
`max+min` and `2-max-min` on which side of `0.5` that lightness falls -- both of which are HSL's,
and neither of which appears in HSV, where `V = max` and `S = d/max` with no branch at all. The
rebuild through `q`/`p` and the `+-1/3`, `0.16666667` hue-sector constants is the standard
HSL->RGB, which shares those constants with HSV->RGB and was what made the first reading say HSV.

The difference is not academic. HSV desaturation holds the brightest channel still and lifts the
others towards it; HSL pulls both ends towards `L`, so a bright saturated colour also gives up
some of its peak. That is exactly the kind of colour Models B and C are aimed at. This add-on
implemented the HSV reading until the block above was decoded, and `tools/style_check.py` agreed
with it -- because the check was written from the same premise and only ever proved the closed
form matched itself.

Neutral 0 gives `x1` either way.

**Others, identified but not needed for B/C:** `opts[80]` is a lerp of each channel against a
grey (`FFMA R4, R4, |UR6|, R5` over channel differences, gated on `|k| >= 1e-6`); `opts[81]`
(`c[0x0][0x4dc]`) takes its own `(max+min)*0.5` and branches on the sign of the coefficient -- a
second, separate lightness operation, and one that runs *before* exposure rather than beside the
saturation block above. `opts[76]`, `opts[79]`, `opts[82..86]` are read but their roles were not
pinned down, because Models B and C leave all of them neutral.

## What this means for us

Model B and Model C, relative to Model A, are exactly three operations on the output image:

| slot | operation | Model B | Model C |
|---|---|---|---|
| `opts[75]` | `rgb *= exp2(k)` | `-0.10` -> x0.9330 (-0.1 EV) | -- |
| `opts[77]` | `x + k * (smoothstep(x) - x)` | `-0.25` -> 25% softer contrast | -- |
| `opts[78]` | HSL `S *= (1 + k)`, hue and lightness held | `-0.10` -> x0.90 | `-0.15` -> x0.85 |

The compose shader writes the third as its closed form, `c_i' = L + (S'/S)(c_i - L)`: holding
hue and lightness while the saturation scales moves every channel along the line through `L`,
because `d = 2*S*min(L, 1-L)` on both sides of the branch, so the hue sectors cancel exactly and
a literal round trip would only add its own rounding. `tools/style_check.py` checks that closed
form against `colorsys.rgb_to_hls` rather than asserting it.

Both are scaled by `LocalToneStrength` in `[0,1]`, applied as `(desc - neutral) * t + neutral`.

None of this touches the network, the weights, or the backend. It is the kind of operation our
compose stage already performs, on data we already hold. The earlier conclusion -- that styles
needed the HIP route and a reimplemented pass -- does not hold: it was reasoning from the C++
parameter plumbing without reading the kernel the parameters were going to.

### What this does not establish

- **The input domain and the exact position in the chain.** The operations run on values already
  saturated to `[0,1]` after a `TEX` fetch. The order of the three slots B and C use is settled --
  by the order the kernel loads them, `0x4c4` exposure, then `0x4c8`/`0x4cc` contrast, then
  `0x4d0` saturation, and the compose shader runs them in that order with the same intermediate
  `saturate` the kernel's `FADD.FTZ.SAT` performs after contrast. The kernel's second copy of the
  chain, from `0x4eb0`, loads the same constants in the same order, so that is both branches and
  not one path. What is not settled is the placement of the slots B and C leave neutral: the
  `opts[80]` grey lerp and the `opts[81]` lightness both run *before* exposure, which matters the
  moment a fourth slot is ever given a value.
- **That our pipeline's output is the same signal.** NVIDIA applies this to its own post-process
  output. Ours is a composed image. The knobs transfer; the tuning may not.

### Where it is applied, and why that is the same place

NVIDIA runs `cg2r_post_process_kernel` at the end of the evaluate, on the network's own output --
which, for Neural Rendering, *is* the frame the game goes on to present. The grading is therefore
the last thing done to the picture that reaches the screen.

The compose shader does the same thing in the same place: `NeuralStyle()` is the last expression
before the store, applied to `outc` after the residual has been composed and after `ToSrgb`, so it
runs on display-referred values in `[0,1]`. That matches the kernel, where every step ends in an
`FFMA.SAT` and the chain never sees anything outside the cube. It is not applied to the network's
input, to the residual, or to any guide -- grading a source and then correcting it would put the
grade under the correction instead of over it.

One difference that cannot be closed from a ReShade add-on: NVIDIA's grading happens mid-pipeline,
so a game's own post-processing after DLSS lands on top of it. This runs at present, after
everything. Nothing here can change that.

**What did need fixing:** the composed frame was only pasted back over the back buffer when the
network had produced a correction that frame (`CompositionIsFresh`). On a frame the network sat
out -- the previous evaluation still pending, which the log counts -- the game's own frame went
out **ungraded**, so a selected style came and went with the skip rate. NVIDIA has no equivalent
of that frame: its grading is inside the evaluate. Compose now runs with the correction forced to
zero on those frames, which leaves the composition as the identity and the grade as the only thing
it does, and the paste is allowed. With `Style=0` the old gate is unchanged.
- **That it is worth shipping.** Three colour operations are not a different network, and calling
  them "Model B" carries an implication about the image that only a comparison can settle. That
  comparison is now cheap -- `tools/ab.ps1` measures it.

### The earlier scope decision needs revisiting

"Cosmetic presets in compose pretending to be a style" was ruled out when the work was scoped, and
that was right under the belief that a real style was network conditioning we could not reach.
That premise is now false. Applying `exp2(-0.10)`, a `-0.25` smoothstep blend and a `x0.90`
saturation is not a cosmetic imitation -- it is the operation, with the constants read out of
NVIDIA's own kernel. Whether to ship it under NVIDIA's model names is a separate question, and
the user's call.

## Still closed: the danielblnc runtime cannot carry a style

For one day (21/09 22:24 to 22/09 00:04) this section was marked superseded, on the reading that
`97b3c` was the network's style control. It is the post kernel's output scale; see the correction
at the top. The runtime cannot carry the **grading vector** (its kernels are precompiled GCN code
objects whose appearance-path parameter structs are 32 bytes total against 56 bytes of style
vector, with no source), and its network takes four controls with no style among them. The grade
sits after the network, on our side, and that is all a Model is here.

The original text follows. The operations above sit after
the network, on our side of it.
