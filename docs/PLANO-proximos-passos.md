# dlss5-neural-amd — what is left, and in what order

State at commit `e057f66`. Everything below the line "Done" is proven by measurement, not by
reading code. Everything under "Left" is not.

Target for comparison: the NVIDIA `renodx-dlss.addon64` feeds NGX these and only these —
`DLSSNR.Color`, `DLSSNR.Output`, `DLSSNR.MVec`, `DLSSNR.Depth`, `DLSSNR.UI`, `DLSSNR.UIAlpha`,
plus `MVecScaleX/Y`, `DepthInverted`, `Enabled`, `Reset`. Extracted from the binary, not guessed.

---

## Done

| | evidence |
|---|---|
| Colour into the network | back buffer, working since before this work |
| **Motion**, estimated by optical flow | engine reports `mean \|mv\|` non-zero and varying frame to frame |
| **Temporal history** | engine log flipped from `history off` to `history on` |
| D3D11 → D3D12 → D3D11 bridge | round trip runs; submit 0.21-0.27 ms, return 0.066 ms |
| Real depth crossing the bridge | snapshot, post-shader and post-return scans all agree: max 0.002008, 119385/120576 non-zero |
| Shareable format map | `R32_FLOAT`, `R32_TYPELESS`, `R16_FLOAT`, `R16G16_FLOAT`, `R16G16B16A16_FLOAT`, `R8G8B8A8_UNORM` share, with UAV bind too. `R32G8X24_TYPELESS`, `R32_FLOAT_X8X24_TYPELESS`, `R32G32_FLOAT` do not |

Caveat on the last two rows of "Done" for motion and history: **25 seconds, one game, one run.**
That is a test, not validation.

---

## Left, in order of value

### 1. Wire the bridge into the add-on — the big one

This is the only thing that brings in a source the network does not have. It also upgrades the
colour input from the presented back buffer to the render-resolution target, which is a better
image (1536x1254 rather than a downscale to the window).

**Why it is not a plug.** `src/neural/neural.cpp` assumes, everywhere, that the game's device is
the device the work runs on. `g.device` and `g.queue` come straight from the swapchain. The
bridge requires those to be two different devices: the game on D3D11, the network on a D3D12
device we create on the same adapter.

**What already exists to copy from.** `src/session/session.cpp` is a working, measured
implementation of exactly this transport: own device on the game's LUID, shared textures, shared
fences both ways, a compute pass to convert depth, and a compose pass on the D3D11 side. It runs
no network — it is the transport, proven and priced.

**It is smaller than it looks.** `g.device` appears 39 times in `neural.cpp`, but most of those
are already on the right side of the split — creating textures, pipelines, descriptor heaps and
views for the network all stay on the work device and do not change at all. Only three places
actually care which device is which:

| where | now | becomes |
|---|---|---|
| `neural.cpp:858-863`, in `OnPresent` | takes `g.device`/`g.queue` from the swapchain, i.e. the game's | create our own D3D12 device and queue on the game adapter's LUID. `session.cpp::Session::CreateOn` does this already |
| `neural.cpp:947` and `:952` | SRVs over `backbuffer`, a game resource | SRVs over the *crossed* colour texture. A resource from the game's device cannot be viewed on ours |
| `neural.cpp:957` + the `CopyResource` into `backbuffer` | composes straight into the swapchain | cross the residual back, then compose on the D3D11 side |

Everything else — `InitPipeline`, `CreateTexture`, `EnsureResources`, the flow passes, the
network `Packet` — keeps working unchanged, because all of it already runs on whatever device
`g.device` points at.

**Order to do it in, each step testable on its own:**

1. **Create the work device but keep using the game's.** Add the second device next to the
   existing one, log that it came up on the same adapter, change nothing else. Proves device
   creation and adapter matching in isolation. One short run.
2. **Switch the network to the work device, colour still from the back buffer** — crossed over
   rather than read directly. At this point the picture should look exactly as it does today; if
   it does not, the transport is at fault and nothing else has been touched yet.
3. **Add the return crossing and compose on D3D11.** Now the image round-trips. Compare the
   residual measurement against today's numbers: same scene, same setting, it should not move.
4. **Swap the colour source** from the back buffer to the render-resolution target. This is the
   first step that should visibly improve anything.
5. **Add depth.** Snapshot at `clear_depth_stencil_view`, convert to `R32_FLOAT` with the compute
   pass, cross, feed. Turn on the `Depth` switch and check the engine log stops saying
   `depth off`.

Steps 1-3 are pure plumbing with a known-good end state to compare against — if the residual
changes, something broke, and you know which step did it. Only 4 and 5 change what the network
sees.

**The one thing that has no answer yet:** PCSX2 has to be on D3D11 for any of this, and the
add-on currently declares itself unavailable on anything but D3D12. That check inverts. Whether
users are willing to run the emulator on D3D11 is a product question, not a technical one —
worth deciding before building, because it decides whether this work reaches anyone.

**Traps already paid for, do not rediscover them:**

* On D3D12 an add-on sees **two** render targets and no depth. On D3D11 it sees **eight**,
  depth included. This is why depth was called impossible for so long.
* **The depth buffer must be copied before PCSX2 clears it.** Hook `clear_depth_stencil_view`
  and snapshot there. Read at present it is uniformly zero. ReShade's own Generic Depth add-on
  has a "preserve before clear" option for the same reason.
* PS2 depth values are **tiny** — max around 0.002. A viewer that maps to 0-255 shows solid
  black, and `%.6f` prints `0.000000`. That is what made four separate reads look empty when the
  data was there. Scale before judging it.
* Pick the colour target by **matching the depth target's size**, not by area. The swapchain is
  1918x1008 and the render target 1536x1254 — the swapchain has *more* pixels, so "the largest
  colour target" picks the wrong one.
* A resize orphans views. Rebuild the SRV *and* the UAV whenever the texture under them is
  recreated, not just when the view is null. Both bugs happened, both produced silent zeros.

### 2. Calibrate the optical flow, and make it adjustable

The flow works but is untuned. Three settings are hardcoded in `kFlowShader`:

| value | now | what it does |
|---|---|---|
| contrast gate | `0.02` | below this the patch is called flat and forced still |
| accept ratio | `0.70` | a match must beat standing still by this much |
| search | ±4 coarse, two passes at strides 2 then 1 | reach, about ±96 raster pixels |

Measured on God of War 2, gameplay: at `0.02` the field is 99% still, which is too aggressive for
a dark scene. Before the gate existed it was 28% pinned at the search limit, which is noise.
The right value is somewhere between and **depends on the game**, so it belongs on a slider, not
in the shader.

Verification is free: the engine prints `mean |mv|` every hundred jobs, and the add-on's own flow
probe prints the share of blocks still and pinned.

### 3. Prove the Structure / Skin / Tone sliders do anything

The add-on writes them (`0x76e30` tone, `0x76e34` structure, `0x76e38` skin) and the runtime
contains matching strings (`Skin` ×2, `Structure` ×6, `Tone` ×10). **Neither fact proves the
engine reads them.** Writing to an offset and the value having an effect are different things —
that assumption cost most of a day on depth.

Cheap test, one run: make the add-on take its residual measurement twice, once with skin at 0 and
once at 3. Same scene, same frame distance. If the residual does not move, the slider does
nothing and should be labelled as such rather than left implying an effect.

### 4. Honest labelling of the Guides section

Right now the section offers Depth, Motion Vectors, Jitter and Exposure. Jitter and Exposure are
dead checkboxes and always were. Depth has nothing to bind to on D3D12. A control that suggests
an effect it cannot have is a defect.

---

## Not worth attempting, and why

| | |
|---|---|
| **Normals** | No slot. The AMD `Packet` has exactly four resources — colour, motion, depth, exposure — pinned by `static_assert`, and the runtime binary contains zero occurrences of `normal`, `albedo` or `roughness`. NVIDIA does not feed them here either: `renodx-dlss.addon64` has none of those parameter names, and the only four hits for "normal" are the phrase "normalized range 0 to 1" in a tooltip. DLSS Ray Reconstruction *as an API* does take normals — for path-traced games with a G-buffer. A PS2 emulator has none. |
| **Upscaling** | The AMD runtime has no separate output resource; the network writes into its input texture. NVIDIA has `DLSSNR.Output`. Nothing to build against. |
| **UI correction** | NVIDIA passes the HUD as its own layer with alpha (`DLSSNR.UI`, `DLSSNR.UIAlpha`). The AMD runtime has zero occurrences of `UIAlpha`, `DLSSNR.UI` or `HUDLess`. The slot does not appear to exist. |
| **Real motion vectors from the game** | The PS2 never computed per-pixel motion. Nothing to capture, on either API. Estimating from the image is what the NVIDIA route does here too. |

---

## How to test without breaking the machine

Two hard shutdowns happened during this work, both under sustained full-load runs: Kernel-Power
event 41, `BugcheckCode = 0`, no power-button press, no TDR (4101), no bugcheck, no dump. That is
the signature of a hard power or thermal event, not a driver reset — a driver reset logs 4101 and
recovers. Whatever the cause, the load that exposed it came from running the emulator at full
tilt back to back.

So:

* **One short run at a time.** 25-30 seconds is enough to reach frame 300 and get every diagnostic.
* **Windowed, not fullscreen.**
* **Boot from a save state** — `-statefile D:\pcsx2-v2.8.2-test\gow2.p2s`. With `-fastboot` half
  the run is logos and menus, where the image is flat and optical flow has nothing to match. This
  is what made the flow field look broken when it was only untested.
* Watch temperature and the power supply under load. Two drops with no TDR and no dump is not a
  code problem.

Command that works, quoting included — the save state path must have no spaces or PowerShell
splits the argument:

```powershell
Start-Process "D:\pcsx2-v2.8.2-test\pcsx2-qt.exe" -ArgumentList `
  '-portable','-batch','-nogui','-statefile','D:\pcsx2-v2.8.2-test\gow2.p2s', `
  '-logfile','D:\pcsx2-v2.8.2-test\emulog.txt','--','"D:\iso ps2\God of War 2 VERSÃO I.A.iso"'
```

Logs to read afterwards, next to the exe: `dlss5-neural.log` (add-on, flow probe, residual
measurement) and `dlssnr_on_amd.log` (engine, `mean |mv|`, `history on/off`, job timings).
