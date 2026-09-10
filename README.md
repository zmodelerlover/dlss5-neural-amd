# dlss5-neural-amd

ReShade add-on that runs the DLSS-NR network on AMD cards.

Every tool I found for DLSS 5 (renodx-dlss, DLSS5-Feeder, DLSS5-Swapper) calls NVIDIA's
`nvngx_dlssnr.dll`, so none of them do anything on a Radeon. This one drives the AMD port of
the network instead, from a ReShade add-on, same idea as RenoDX: one core file plus a small
table where you add a game.

**PCSX2 is the only thing I've ever run this on.** Not a native D3D12 game, not RPCS3, not
another card, not RDNA3. I genuinely don't know how it behaves anywhere else, and the other
rows in the target table are guesses I typed, not results.

That said the plumbing isn't pcsx2-specific. It's a ReShade add-on talking to a D3D12 device,
so anything ReShade attaches to and that renders in D3D12 is at least in scope. Adding a target
is one row in a table. If you want to point it at something else, most of the work is already
sitting here, and figuring out what a new target actually exposes is what the probe in
`src/probe` is for.

Discord: https://discord.gg/wYhvS3JSHM
That server is for DLSS 5 in general, AMD, ports, whatever people are building. It isn't a
support channel for this add-on.

**Just want it running?** → [Before you start](#before-you-start) → [Quick start](#quick-start).
Six steps, no compiler needed.
**Something's broken?** → [Troubleshooting](#troubleshooting), which is keyed by what you see on
screen.
**Want to change the code?** → [Building it yourself](#building-it-yourself-optional).

## Videos

PCSX2, no audio. Click a thumbnail to play, or use the plain links if the thumbnails do not load.

[![PCSX2 running the network, clip 1](media/pcsx2-0307.jpg)](https://github.com/zmodelerlover/dlss5-neural-amd/raw/master/media/pcsx2-0307.mp4)

**[Clip 1 (mp4, 7 MB)](https://github.com/zmodelerlover/dlss5-neural-amd/raw/master/media/pcsx2-0307.mp4)**

[![PCSX2 running the network, clip 2](media/pcsx2-0320.jpg)](https://github.com/zmodelerlover/dlss5-neural-amd/raw/master/media/pcsx2-0320.mp4)

**[Clip 2 (mp4, 25 MB)](https://github.com/zmodelerlover/dlss5-neural-amd/raw/master/media/pcsx2-0320.mp4)**

---

## Before you start

| | |
|---|---|
| **GPU** | AMD **RDNA3 or RDNA4** with the **HIP 7** runtime, i.e. `amdhip64_7.dll` on the search path. HIP 6 will not do. A current Adrenalin driver ships it. This does nothing on NVIDIA or Intel. |
| **Renderer** | **Direct3D 12**, or **Direct3D 11** through the bridge (see [What the network can actually be fed](#what-the-network-can-actually-be-fed)). On Vulkan or OpenGL the add-on loads and then sits there. D3D11 is the better path now: it is the only one where the game's own depth and motion vectors are reachable. |
| **ReShade** | The **add-on** build, 6.x. The plain one will not load add-ons. Tested on 6.8.0. |
| **Disk** | About 150 MB for the network weights. |

Tested on: RX 9070 XT, ReShade 6.8.0, **PCSX2 2.3.14 and 2.8.2**, God of War 1. Nothing else has
been tried by me.

**Where do the files go? Always next to `pcsx2-qt.exe`.** That is true whether your PCSX2 is a
portable copy on an external drive or a normal install — the add-on only ever looks in the
folder the running `.exe` is in. The portable-vs-installer difference only matters for PCSX2's
own *settings*, which comes up once in [step 5](#5-set-pcsx2-to-direct3d-12).

---

# Quick start

Three files end up next to `pcsx2-qt.exe`: one from the release, two from the discord. A fourth
writes itself. That's the whole install.

### 1. Get the add-on

Download `dlss5-neural.addon64` from
**[Releases](https://github.com/zmodelerlover/dlss5-neural-amd/releases/latest)**.

That's it. **You do not need to build anything** — the file in the release is compiled from this
exact repository. Building is only if you want to change something, and it has
[its own section](#building-it-yourself-optional) at the bottom.

### 2. Get the runtime and the weights

`dlssnr_amd_pass1.dll` (7 MB) and `dlssnr_on_amd_weights.bin` (141 MB) are **not in this repo
and never will be.** The weights are NVIDIA-derived and the runtime comes from a third-party
project that declares no license, so I'm not the one redistributing them.

> **Both are in the `files` channel on the discord → https://discord.gg/wYhvS3JSHM**

The `.dll` there is already rebuilt without the spin cap, so it goes straight in the folder with
no patching. Check what you downloaded against `tools/SHA256SUMS.txt`:

```powershell
Get-FileHash dlssnr_amd_pass1.dll, dlssnr_on_amd_weights.bin -Algorithm SHA256
```

It has to be exactly that build. The add-on hashes it at load and refuses anything else, because
the whole thing is hardcoded offsets into one specific binary and pointing them at a different
one hangs the game.

### 3. Install ReShade into PCSX2

Get the **add-on** build of ReShade (the one labelled "with full add-on support") from
<https://reshade.me/>, run it, pick `pcsx2-qt.exe`, choose **Direct3D 10/11/12**. Skip the
shader download, this doesn't use any.

### 4. Copy the files in

Next to `pcsx2-qt.exe`:

```
dlss5-neural.addon64
dlssnr_amd_pass1.dll
dlssnr_on_amd_weights.bin
```

`dlssnr_on_amd.ini` shows up on its own the first time it runs. Don't delete it — see
[the engine ini](#the-engine-ini) below.

### 5. Set PCSX2 to Direct3D 12

Settings → Graphics → Renderer → **Direct3D 12**. On anything else the add-on loads and then
sits there doing nothing.

**Watch out for per-game overrides.** A renderer pinned on one game beats your global setting
silently, and that wasted an entire evening for me. Easiest way to check, no matter where PCSX2
lives: **right-click the game in the list → Properties → Graphics**, and make sure Renderer is
either *Direct3D 12* or left on the global setting.

If you'd rather look at the file, it's `gamesettings\<SERIAL>.ini` — but *which folder* that is
depends on how PCSX2 was installed:

* **Portable** — you extracted the `.7z`/`.zip`, or there's a `portable.ini` next to the exe.
  Common if PCSX2 lives on an external drive. Everything sits next to `pcsx2-qt.exe`:
  `gamesettings\`, `inis\`, `memcards\`, `cache\`. There is no `Documents\PCSX2` at all.
* **Installer** — `Documents\PCSX2\gamesettings\`.

**You want `Renderer = 15`.** 15 is Direct3D 12, which is the only thing this add-on works on.
`Renderer = 3` is Direct3D 11 — if you find that line, it is the problem, not the fix. Deleting
the line entirely is also fine: it just falls back to your global setting.

### 6. Start a game

Hit **Home** for the ReShade overlay → **Add-ons** tab → **DLSS Neural Rendering (AMD)**.
The status line says whether it's actually running. There's also `dlss5-neural.log` next to
the exe.

The defaults are the settings I got the numbers below with — Encoding sRGB, Resolution Scale
0.50, Pass Count 1, inline on — so there is nothing you have to change. Nothing is saved
between runs either; every launch starts from those defaults.

---

## Troubleshooting

| What you see | What it is |
|---|---|
| Add-on isn't in the Add-ons tab at all | `ReShade.ini` has `DisabledAddons=dlss5 neural@dlss5-neural.addon64` under `[ADDON]`. ReShade writes that line if you ever untick the add-on, and then it never loads again, with no error anywhere. Clear it. |
| Status says the API is wrong | PCSX2 isn't on D3D12. Check the per-game override too, not just the global setting: right-click the game in the list, Properties, Graphics. |
| `HIP: amdhip64_7.dll failed to load` | HIP 7 isn't installed. HIP 6 doesn't count. |
| `hash mismatch; refused` | Wrong `dlssnr_amd_pass1.dll`. Compare against `tools/SHA256SUMS.txt`. The refusal is deliberate — the alternative is a hang. |
| Game dies with `887A0005` / device removed | `DXGI_ERROR_DEVICE_REMOVED`, from a Windows TDR. See [the engine ini](#the-engine-ini). |
| It runs but "only shifts the colours a bit" | Encoding is wrong. On an 8-bit SDR back buffer it has to be **sRGB**. scRGB-nl linearises something that is already sRGB and then scales it by 203/white, so the network gets a nearly black image and does nothing. Ask me how I know. |
| Colour and tone change, but **textures look identical** | Resolution Scale. At the default 0.50 the network is handed a half-resolution image, so the finest thing it can see is two screen pixels wide — it cannot put detail into a texture it was never shown, and the only correction it can make is colour, tone and large-scale shading. Textures change at **1.00** and not before. That is four times the cost, so turn **Apply On Same Frame** off first; the correction then lags a few frames instead of stalling the game. |
| Turning Pass Count up switched the add-on off | Pass 2 and 3 load `dlssnr_amd_pass2.dll` and `dlssnr_amd_pass3.dll`, separate files so each pass gets its own copy of the runtime's globals. Only pass1 is distributed. Copy `dlssnr_amd_pass1.dll` and rename it — same hash, so it passes the check. Without them Pass Count now snaps back to what loaded instead of stopping the add-on. |
| `imgui.h` or `reshade.hpp` not found when building | You deleted `external/`. It's in the repo now; `git checkout external` puts it back. |

### If you're reporting a problem

Screenshots don't help much here — flicker is frames alternating, and a still image freezes one
of them, so it always looks fine. Two things settle almost anything:

1. **The status line.** ReShade overlay → Add-ons → DLSS Neural Rendering (AMD). It reads
   `Running: X processed, Y skipped (Z%)` plus the back buffer and network size. Quote that line.
2. **The logs**, both next to `pcsx2-qt.exe` — the same folder you put the add-on in, portable
   install or not:
   * `dlss5-neural.log` — the add-on: what it detected, back buffer size and format, and the
     residual measurement it takes on frame 240.
   * `dlssnr_on_amd.log` — the runtime: staging formats, per-job timings, timeouts, faults.

The residual measurement in the first one is the useful bit. `mean 0.000000` means the network
returned its input untouched, which is a completely different problem from a nonzero residual
that looks wrong on screen. They are indistinguishable from the couch.

### The overlay

Everything is on the ReShade overlay, under **DLSS Neural Rendering (AMD)**. `Ctrl+End` toggles
the effect without opening it.

**It starts switched off, every run.** The add-on rewrites every presented frame, and the
settings that do that are the ones that have taken a machine down, so nothing happens until you
turn it on and have looked at what it is set to. That is deliberate and not persisted.

Each control has a `(?)` next to it. Hover it: the tooltip says what the control does, what was
measured about it, and — where it applies — why it is not what you would guess. Most of that
text is a measured result rather than a description, so it is worth reading once.

**Colour means risk, and it tracks the value, not the control.**

| | |
|---|---|
| **Red** | The value it is holding right now can take the display driver down. The game dies on `DXGI_ERROR_DEVICE_REMOVED` and the desktop goes with it, with nothing in any log pointing back here. |
| **Amber** | Past what has actually been measured on this machine. Not known to break, not known to work. Change one thing at a time and watch the skip rate under Status. |

Turning the value back down clears the colour.

**Save Settings** writes everything to `dlss5-neural.ini` next to the exe, so it survives a
restart. Without it the overlay is a scratchpad and every A/B test means re-dialling half a
dozen controls on the next run. **Reload Settings** throws away anything changed since the last
save.

**Language** switches the whole panel, tooltips included, between English and Brazilian
Portuguese. English is the default.

### Timing, and why Pass Count is not a quality setting

**Timing** is the one to understand first. *Same frame* blocks the game on the GPU until the
network is done, so what you see is this frame's own correction — honest, and the mode that
turns any slowdown into a stall. *Async* runs the network on its own timeline and shows a
correction a frame or two old; nothing blocks, and an evaluation that overruns costs a skipped
frame instead of a hang. Anything expensive wants Async.

**Pass Count** runs the network over its own output N times. It is the one declared difference
of the ShortFuse route and it has never been shown to help here — the only reading ever taken of
it was `Passes=3` giving a residual of exactly zero.

It used to load a separate copy of the runtime per pass, because the runtime keeps all its state
in module globals at fixed RVAs and Windows hands back the same `HMODULE` for the same path. So
two passes meant two full engine bring-ups: 147 MB of weights plus activation buffers each, in
VRAM, next to the game's own working set. That is what took machines down, and a cap on the
count — which is what was tried first — was never going to fix it, because two evaluations at
0.50 scale are about 32 ms against a 2 s driver timeout.

It now records **one** engine N times, so the cost is time rather than memory, and the ceiling
is 3. `dlssnr_amd_pass2.dll` and up are no longer used and can be deleted.

Raise it to measure, not to play: run a scene at 1 and at 2 and compare the `measure, residual`
line in the log.

### The engine ini

The runtime reads `dlssnr_on_amd.ini` from the game folder when it loads. **Its own built-in
default for the host watchdog is 600 ms**, and one stalled job that long trips Windows TDR,
which removes the D3D12 device and takes the emulator with it. The symptom is a pile of
`887A0005` in `emulog.txt` and a dead PCSX2, with nothing pointing back at this add-on.

So the add-on writes the file itself if it isn't there, with `InlineWaitMs=100`. At 0.50 scale
the network takes about 16 ms, so 100 is a wide margin, and past it you get a frame without the
effect instead of a freeze. Delete the file and it gets written again; edit it and your version
is kept. Don't raise `InlineWaitMs` far without knowing why.

### What the runtime actually reads

Mapped by decompiling the runtime's own ini reader and its static initialiser, not guessed. Worth
having written down, because an offset that is written but never read looks identical from
outside, and this table is what separates the two.

```
0x76E10 int   DepthInverted   default 1
0x76E1C bool  Enabled           0x76E1D bool Temporal
0x76E1E bool  UseFsrInputs      0x76E1F bool UseDepth
0x76E20 int   Tonemap         default -1
0x76E30 float LocalTone       default 0.0     -> Local Tone Strength
0x76E34 float LocalStructure  default 1.0     -> Structure Intensity
0x76E38 float SkinStructure   default -1.0    -> Skin Structure Strength
0x76E3C float Scale           default 0.03125 -> Engine Scale
0x76E40 int   UseAutoMask     default 1       -> Character Mask
0x76E44 int   ToneChannels    default 0       -> Tone Channels
```

Its full ini surface is `Enabled` `Temporal` `UseFsrInputs` `UseDepth` `Tonemap` `Interop`
`Inline` `InlineWaitMs` `LocalTone` `LocalStructure` `SkinStructure` `Scale` `UseAutoMask`
`HipDevice` `ToneChannels`, plus six environment variables: `DLSSNR_NOBLEND`
`DLSSNR_NOPOSTHIST` `DLSSNR_NO_REPACK` `DLSSNR_SLOW_PREPOST` `DLSSNR_STAGES` `DLSSNR_WBLOG`.

`VIT512_OLD` is also an environment variable, read as a bitmask — each bit swaps one kernel
launch for a legacy one. And `vit512a`, `vit512b`, `vit512_attn`, `vit512_conv1`, `vit512_conv2`
and `vit512_ffwd` are profiling labels for pipeline stages, not model variants. Both are easy to
mistake for a model selector; neither is one.

Two of these were being written wrong. `Tonemap` was forced to 0 at init when the engine's own
default is -1, and `DepthInverted` defaulted to off when both runtimes default it on — so the
add-on inverted the engine's own default on every run. `UseAutoMask` was never written at all.

## Building it yourself (optional)

**Skip this unless you want to change the code.** The `.addon64` in
[Releases](https://github.com/zmodelerlover/dlss5-neural-amd/releases/latest) is built from this
repository and is the same file you would produce here.

**What to install first.** One thing: Microsoft's C++ compiler. You do not need the full Visual
Studio IDE — **Build Tools for Visual Studio** is free and enough. Get it from
<https://visualstudio.microsoft.com/downloads/>, under *Tools for Visual Studio* → *Build Tools
for Visual Studio*. In its installer tick the single workload **"Desktop development with C++"**
and install. That workload brings the Windows SDK with it, which is the other half of what the
build needs. If you already have Visual Studio with C++, you already have all of this.

**Then, in PowerShell:**

```powershell
git clone https://github.com/zmodelerlover/dlss5-neural-amd
cd dlss5-neural-amd
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Target neural
```

Note the `-ExecutionPolicy Bypass`. Windows refuses to run downloaded `.ps1` files by default, so
plain `.\build.ps1` usually fails with *"cannot be loaded because running scripts is disabled on
this system"*. That message is Windows, not this project. The line above sidesteps it for that
one command without changing anything on your machine.

**That is the whole build.** Nothing to download first, no submodules, no `vcpkg`, no CMake, no
`.sln` to open. The ReShade and ImGui headers are already in `external/reshade/` — see
[external/reshade/NOTICE.md](external/reshade/NOTICE.md) for what they are and where they came
from. `build.ps1` finds the compiler and the SDK by itself; if it picks the wrong one, pass
`-VsPath` or `-SdkPath`.

**It worked if** the last two lines look like this, and `build\dlss5-neural.addon64` exists:

```
OK: ...\build\dlss5-neural.addon64
dlss5-neural.addon64  73728  ...
```

**If it fails:**

| Message | Fix |
|---|---|
| `cannot be loaded because running scripts is disabled` | You dropped the `powershell -ExecutionPolicy Bypass -File` part. |
| `vswhere.exe not found` / `No Visual Studio install with the C++ tools` | The C++ workload isn't installed. Re-run the Build Tools installer and tick *Desktop development with C++*. |
| `Windows 10/11 SDK not found in the registry` | Same installer, same workload — it includes the SDK. Or pass `-SdkPath`. |
| `fatal error C1083: 'imgui.h'` | You deleted `external/`. `git checkout external` puts it back. |
| `git` is not recognised | Install Git for Windows, or just use the release build instead. |

The other two targets build the same way: `-Target probe` (dumps what a game exposes) and
`-Target session`. CI on `windows-latest` builds all three from a bare checkout on every push,
so if the badge is green the repository builds as-is.

## What the network can actually be fed

Right now: **colour only**, taken from the presented back buffer. Not because depth is missing,
but because on D3D12 it is out of reach. Measured with the probe in `src/probe`, same game, same
frame, only the renderer changed:

| | PCSX2 on **D3D12** | PCSX2 on **D3D11** |
|---|---|---|
| render targets ReShade shows the add-on | **2** | **8** |
| depth | none | **1536x1254 `R32G8X24_TYPELESS`**, 41 draws |
| colour | 1918x1008, the presented back buffer | **1536x1254 at render resolution**, 52 draws |

On D3D12 an add-on sees the swapchain and nothing else. `bind_render_targets_and_depth_stencil`
fires **zero** times in 600 frames, with or without also subscribing to the draw events -- both
tried, both measured. So the depth code sitting behind the Depth switch has nothing to bind to,
and that is a property of the D3D12 path, not of the game.

On D3D11 both guides are right there, and the two are the same size, so they need no realignment.
The render-resolution colour is also a better input than what is used today: it is the image
before it gets scaled down to the window.

The catch is that the AMD network runtime is D3D12. Getting at those sources means running PCSX2
on D3D11 and carrying the textures to a separate D3D12 device -- own device on the game's
adapter, shared texture, shared fence. `src/session/session.cpp` does exactly that and measures
what it costs. Build it with `-Target session`, run it with PCSX2 on **Direct3D 11**, and read
`dlss5-session.log`. It runs no network and changes no pixels.

Measured on an RX 9070 XT, God of War, colour and depth both 1536x1254:

```
submit  0.21 - 0.27 ms mean per frame   (the copy and the fence signal, on the CPU timeline)
land    ~0.85 ms                        (sampled; a deliberate CPU wait, not paid in normal use)
```

About a quarter of a millisecond a frame to carry both guides across, against a 16.7 ms frame.
The way back -- D3D12 writes a shared texture, signals a fence, the game's D3D11 context waits on
it and composes -- adds **0.066 ms mean**. The whole loop is affordable.

**But the depth buffer is empty, and that is the wall.** Not a transport problem: the round trip
carries whatever is in it faithfully. PCSX2's depth target simply reads as all zeros. Four
independent ways agree, and the fourth is the one that settles it -- the probe dumps colour and
depth from the same frame through ReShade's own readback, and in that one file colour is 94.8%
non-zero while depth is 0.0%, min 0, max 0. So it is not the measurement. Also tried, same
result: reading it through a compute shader at present, and reading a private copy snapshotted
at the moment PCSX2 binds a different depth target, which is early enough that the content
should still be there.

Where that leaves it: the transport is built and priced, the render-resolution colour is real and
usable, and depth needs someone to work out where PCSX2 actually keeps usable depth on D3D11 --
the target the bind events point at is not it.

Two things that cost a day to find, so they are written down here:

* **The depth buffer cannot be shared directly.** PCSX2's depth is `R32G8X24_TYPELESS`, and D3D11
  refuses to *create* a shared texture in that format at all -- `E_INVALIDARG`, not a permission
  problem. Same for `R32_FLOAT_X8X24_TYPELESS` and `R32G32_FLOAT`. What does share: `R32_TYPELESS`,
  `R32_FLOAT`, `R16_FLOAT`, `R16G16_FLOAT`, `R16G16B16A16_FLOAT`, `R8G8B8A8_UNORM` -- and they
  still share with `BIND_UNORDERED_ACCESS` added. So depth goes through a compute shader that
  reads it and writes `R32_FLOAT`, which is the format the network wants anyway. That pass does
  not measurably change the numbers above.
* **Pick the colour target by matching the depth target's size, not by area.** On PCSX2 the
  swapchain is 1918x1008 and the render target is 1536x1254 -- the swapchain has *more* pixels,
  so "the biggest colour target" picks the wrong one. The pair that renders together is the pair
  that is the same size.

Motion is a separate matter and not a plumbing problem: the PS2 never computed per-pixel motion,
so there is nothing to capture on either API.

## Rebuilding the runtime yourself

You don't need this — the DLL on the discord is already the rebuilt one. It's here for anyone
who wants to see what was changed rather than take my word for it.

`tools/runtime-patches.json` is the spec: five patches, with offsets, the bytes before, the
bytes after, and why. Grab the untouched `version.dll` from the discord and:

```powershell
python tools\patch_runtime.py version.dll tools\runtime-patches.json dlssnr_amd_pass1.dll
```

That applies four of the five and **skips the fifth on purpose.** The fifth is the one the
OptiScaler installer uses to cap the GPU wait shader at 262144 iterations, about 6 ms. The
network takes 16 ms at half scale and 125-187 ms at full, so inline mode timed out on every
single frame, and the apply pass just kept its input. Residual came out at exactly zero, which
is a fun way to spend a few hours.

Everything is written in place at the same length, so no RVA moves and the add-on's offsets stay
valid. The script prints a new SHA256; paste it into `kRuntimeSha256` in `src/neural/neural.cpp`
and rebuild.

## What it does per frame

```
back buffer -> colour prep -> network raster -> network -> residual -> compose -> back buffer
```

Only the network's correction gets resampled, the full-res image goes back untouched. It hooks
`present`, because `reshade_finish_effects` never fires if you have no shaders loaded.

## Numbers

PCSX2 2.8.2, God of War, 1920x1080 back buffer, scale 0.50, 1 pass, RX 9070 XT:

```
network input, mean absolute   0.0207
residual, mean                 0.0115
residual, max                  0.792
per job                        15-16 ms
frames with a fresh correction 3932 of 3960
```

The add-on measures that itself on one frame and dumps it in the log. Worth keeping, because
"the network isn't doing anything" and "the network works and my compose is eating it" look
identical from the couch and need completely different fixes.

## Repository layout

| | |
|---|---|
| `src/neural/neural.cpp` | the add-on. One file. |
| `src/probe/probe.cpp` | render target probe — dumps what a game actually exposes. Build with `-Target probe`. |
| `src/session/session.cpp` | small session logger. |
| `external/reshade/` | ReShade + ImGui headers, vendored. See `NOTICE.md`. |
| `tools/patch_runtime.py` | rebuilds the runtime from `version.dll`. |
| `tools/runtime-patches.json` | the five patches, with offsets and bytes. |
| `tools/SHA256SUMS.txt` | hashes of the four files the repo does not ship. |
| `build.ps1` | builds an add-on with `cl.exe`, no VS project. |
| `CHANGELOG.md` | what changed between releases. |
| `tools/check_shaders.ps1` | extracts the HLSL out of `neural.cpp` and runs `fxc` on it. A shader typo otherwise only shows up as a log line inside the game. |

## Stuff I didn't get to

None of this is settled, it's just where I stopped. One card, one program, one night.

* Upscaling. I couldn't find an upscaling path in the AMD runtime I used, input and output
  share the same texture. Maybe another build has one.
* ~~Model/style, UI correction, character mask.~~ Settled, see
  [Model A/B/C](#model-abc-will-not-be-implemented). Character mask was there all along
  (`UseAutoMask`) and is now exposed. Model/style genuinely is not, and that one is closed.
* Depth. PCSX2 writes a real 512x512 R32G8X24_TYPELESS buffer, the probe in this repo finds it.
  ReShade's `bind_render_targets_and_depth_stencil` never reached my add-on on D3D12 though, so
  it's not hooked up. Code's written, sitting behind the Depth switch. If you get it working
  you're feeding one more guide than the NVIDIA path does here.
* Motion. Nothing showed up. Those games didn't compute per-pixel motion, but optical flow or
  digging into the emulator's own buffers are both unexplored.
* The network itself. DLSS-NR is a denoiser for modern ray traced stuff. Something built for
  restoration or upscaling old content would probably fit emulators better, and swapping it
  doesn't mean rewriting everything.
* Literally anything that isn't PCSX2. A D3D12 game with a real upscaler would hand the network
  colour, depth and motion, which is what it was built for. That's where it should look like
  the videos everyone's seen. Nobody's tried it with this.

## Model A/B/C: will not be implemented

The NVIDIA add-on has a **Model** combo with A, B and C, and switching it does change the
picture. Its own tooltip says it goes through "the prerelease `DLSSNR.Style` field". People ask
for it here, so: this is what it is, and why it is not coming.

`DLSSNR.Style` is an int in the options struct. It gets clamped against a count that comes from
the network description — the number of models is data, not code — and then used to look up an
entry in a table of 8 slots of 68 bytes. The entry holds a bitmask plus a short vector of
floats. Each set bit lerps one slot of a 14-float block from a neutral value toward the entry's
value, scaled by `LocalToneStrength` clamped to 0..1. Those 14 floats are then copied
contiguously into the parameter block the kernels receive.

Two of the eight slots are populated, so three models:

| | Model A | Model B | Model C |
|---|---|---|---|
| mask | `0x00` | `0x34` | `0x20` |
| slot 75 | — | **-0.10** | — |
| slot 77 | — | **-0.25** | — |
| slot 78 | — | **-0.10** | **-0.15** |

Model A is style 0, which matches no entry and falls back to a neutral descriptor. It is the
literal baseline, which is why there are only two entries for three models.

**They are not three networks.** `nvngx_dlssnr.dll` carries 156 distinct `block*` tensor names,
each appearing exactly once — one weight set, three configurations over it. That is the good
news, because it means porting this would need no data that isn't already here.

The bad news is the AMD side. Its option block is mapped field by field above and none of the
slots is Style. The three constants do not appear anywhere in the binary. Nothing writes a
14-float span. And the kernels settle it: their argument metadata gives the size of the struct
each one takes by value, and the style vector alone is 56 bytes, while `k_final_head` and
`k_post_block_1h_32_fp8` — exactly where appearance knobs would land — are **32 bytes total**.
There is no room, and the kernels are precompiled GCN code objects inside the DLL.

Whoever did the AMD port compiled the network with the neutral style folded in and dropped the
inputs. Not a missing offset, not a hidden field: the input is not in the compiled binary.

**So this will not be implemented, and it is not a matter of effort or of finding the right
offset.** The AMD port is closed source. Its kernels ship as precompiled GCN code objects inside
`dlssnr_amd_pass1.dll`, and adding a network input means recompiling them, which needs sources
that were never published. Nothing an add-on does from outside can put a parameter into a kernel
that does not have one.

Please don't file this as a missing feature. **Model A is the only model that exists on this
side**, and the work above is written down precisely so nobody spends another week rediscovering
that.

## License

MIT, in `LICENSE`. Third-party headers under `external/` keep their own licenses, listed in
`external/reshade/NOTICE.md` — BSD-3-Clause OR MIT for ReShade, MIT for Dear ImGui. No network
binaries in here and it doesn't download any.
