# How close this is to what NVIDIA shows, and how to compare

Written after the ETS2 measurement on an RTX 3050 (`handoffs/RESULTADO-nvidia-preset-style-ets2-20260921.md`)
and a read of the public reference for the NVIDIA-side composition, the OptiScaler DLSS-NR fork
(`y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG`, branch `dlss-neural-rendering`,
`OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl`), whose README states its composition is
RenoDX's design reimplemented. RenoDX's own DLSS 5 addon source is not public; the fork is the
nearest thing to reading it.

The first thing to know: **on NVIDIA the pixels on screen are also a composition.** Neither RenoDX
nor the fork shows the DLL's raw output. Both run the network on a proxy of the frame and transfer
its answer back as a bounded luminance ratio with a chroma blend. So "the same as NVIDIA" means
"the same network, the same controls, and the same composition", and each of the three can be
checked separately.

## What is the same, and how it was checked

| | NVIDIA (DLL + RenoDX) | here | evidence |
|---|---|---|---|
| Weights | one set, `preset=1` | the same set, extracted | NGX log: `1 config(s) available` |
| Network controls | LocalTone 1, LocalStructure 1, Skin 1, UseAutoMask 1 | identical defaults, same slots (`97b30/34/38/40`) | `ngx_params.log` P0; `runtime_offsets_check.py` |
| Style clamp | unsigned, to `n-1 = 2` | `clamp(style, 0, 2)` | `Evaluate` at `0x21BB0`, `n=3` at runtime |
| Skin / structure derivation with AutoMask off | both effective values `-1.0` | the AMD worker does the same: `0x180019070..0xE5`, constant `0x18006ABAC = 0xBF800000` | IDA, both DLLs |
| What the forward receives | LocalTone, LocalStructure (raw), style/128, ControlMask device pointer (null under RenoDX), skinEff, structEff | **four** floats at `0x96F98..0x96FA4`: tone, structure (raw), skinEff, structEff; no style, no mask input. `0x96FA8` (`97b3c`, "Scale") is the post kernel's output scale, not a control | NVIDIA `0x225A7..0x225CB`; AMD worker `movq [0x96F98]`, `[0x96FA0]`, `[0x96FA4]`; evaluate `sub_18002D2D0` object+32/40 to the pre kernel, object+48 to the post kernel; log format `ctl (%.2f %.2f %.2f %.2f)` |
| Grading operator | normalise, `exp2` exposure, smoothstep contrast, HSL saturation, in that order, each saturating | `NeuralStyle()` in `kComposeShader`, same order | operator's §5.7: sm_86 SASS read instruction by instruction; `style_check.py` |
| Grade coefficients between frames | struct re-initialised to neutrals every evaluate, style 0 writes nothing | coefficients recomputed every frame from `g.style` | operator's §5.8, `0x19FBD..0x1A076` |
| History reset | on Style, UseAutoMask (exact); Tone, Structure, Skin (`>1e-5`); not on Intensity | `ControlsChanged()`, same set, same tolerance | 27 `reset temporal history` lines in the trace |
| Grade constants | style 1: `-0.10, -0.25, -0.10`; style 2: `0, 0, -0.15`, scaled by `clamp(LocalTone, 0, 1)` | `StyleCoefficients()` × `StyleGradeStrength()` | `.rdata` at `0x1800B0D80+0x64/+0xA8`; `style_check.py` |
| Composition, intensity ≤ 1 | `lerp(original, model, s)`, luminance ratio with floor 1/512, two-sided guard 2.0, `lerp(original·ratio, upgraded, colour)` | the same expression, in `kComposeShader` | fork `dlssnr.hlsl` 585–792, this file's derivation below |
| Composition, intensity > 1 | ratio raised to `1 + (s-1)`, guard still binds | the same, since this change | fork line 764 |
| Where the grade is applied on ETS2 | at present (RenoDX Hook Method = Present) | at present | RenoDX panel: "presentation backbuffer with dummy temporal inputs" |
| NR Preset | no effect; single entry | no control | NGX log, `CG2RFindWeightByPreset` |

### Why the two compositions are the same expression

The reference feeds the network an encoded *proxy* and composes with two branches: if the frame is
darker than the proxy, `ratio = originalLuma / proxyLuma`; otherwise the difference is headroom the
proxy could not carry and `ratio = (modelLuma + (originalLuma - proxyLuma)) / modelLuma`. Then
`upgraded = lerp(original, HueOkLab(model·ratio, model), s)`.

Here the network is shown the frame itself (sRGB as-is, or linear × diffuse-white when an HDR
encoding is selected), so `proxy == original` and both branches collapse to `ratio = 1`; the OkLab
step is the identity at ratio 1. What remains is `upgraded = lerp(P, M, s)` with `M` the network's
picture, followed by the same floor, the same two-sided guard, the same `bounded / ratio` rescale
and the same colour blend. That is `kComposeShader`'s ratio path line for line. The residual form
(`M = CubeScale(P, P + E)`) is the fork's own "matched residual" path (`gTransfer == 1`), with
hhkbble's cube scaling, which the fork ships on by default.

## What is not the same

- **The Model does not reach the network.** On NVIDIA `style/128` is an input of the forward and
  is what makes B and C differ in kind. This runtime's network has no such input; a Model here is
  its grade only. For one day the add-on wrote `style/128` into `97b3c`, which is the post
  kernel's output scale, and Model A returned the input unchanged (residual mean 0.00024 on an
  input of 0.45, ETS2). Reverted; the add-on refuses that field near zero and reports an inert
  network in the log and on the status line.
- **Residual Limit 0.25 and Edge Fade.** Ours. They cap a single pixel's correction and roll it off
  at the border, added after a measured tile-extrapolation blow-up on the AMD port (max correction
  4.16 against a frame mean of 0.13). The reference has neither. Set `ResidualLimit=0` to remove
  the difference; expect the blown blocks back on the scenes that produced it.
- **Upsample filter for a reduced Resolution Scale.** Ours defaults to Catmull-Rom; the fork samples
  the model bilinearly. RenoDX exposes both as Upsample Filter. `Bicubic=0` matches the fork. At
  Scale 1.0 there is no upsample and no difference.
- **HDR encodings.** The reference shows the network `LinearToSrgb(SoftKnee(frame / paperWhite))`;
  ours shows `ToLinear(frame) × k` and tells the runtime the encoding. Different proxies, so the
  network answers differently. Not touched here; SDR (Encoding 0) has no knee on either side.
- **Temporal inputs on ETS2.** RenoDX had none: ETS2 has no DLSS, so its motion vectors and depth
  were dummies. Ours are real (Launchpad optical flow, the game's depth). Better inputs, not the
  same inputs. To reproduce the NVIDIA configuration for a comparison, not for playing:

  ```ini
  Motion=0
  Depth=0
  Temporal=2
  ```

  Motion off and depth off remove the guides; Temporal forced on keeps the history running with
  zero motion, which is what the DLL did with the dummies.
- **DepthInverted.** RenoDX writes 0; the runtime's default, and what this add-on wrote until now,
  is 1. Its 0 was measured with a dummy depth, so it is not a reading about any game. Now an ini
  key (`DepthInverted`, default 1) and a checkbox under Depth, so it can be measured on a game with
  a real buffer.
- **Network Output mode.** Not a thing NVIDIA users see. It now carries the Model's grade like the
  composed frame does, but it bypasses the composition entirely; keep it off for any comparison.
- **UICorrection.** RenoDX sends 1, the DLL's default is 0, and the AMD runtime has no mapped field
  for it. What it does was not investigated on either side.
- **The DLL on the NVIDIA machine was the pre-RTX-50 patched build.** Same size, different hash;
  the style table and `Evaluate` match, the rest was not audited.

## What has not been measured

Nobody has yet put the same scene through both machines and compared the numbers. The table above
says the arithmetic is the same; it does not say the pictures are. `tools/capture_pair.py` and
`tools/compose_check.py` are the instruments on this side. Screenshots are not evidence.
