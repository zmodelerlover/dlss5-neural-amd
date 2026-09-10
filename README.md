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

[![Support this project on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/T6T213OVFE)

**Just want it running?** → [Before you start](#before-you-start) → [Quick start](#1-get-the-add-on).
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

**Where do the files go? Always next to `pcsx2-qt.exe`.** True whether your PCSX2 is a portable
copy on an external drive or a normal install — the add-on only ever looks in the folder the
running `.exe` is in.

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

`dlssnr_on_amd.ini` appears on its own the first time it runs. Don't delete it — see
[the engine ini](#the-engine-ini). A second file, `dlss5-neural.ini`, appears only if you press
**Save Settings** in the overlay; that one is yours and holds whatever you dialled in.

### 5. Set PCSX2 to Direct3D 11

Settings → Graphics → Renderer → **Direct3D 11**.

Both D3D11 and D3D12 work, but **D3D11 is the one you want**: it is the only path where the
game's own depth and motion vectors reach the network. On D3D12 the network only ever sees
colour, because ReShade shows an add-on nothing but the swapchain there. That is measured, not a
guess — see [What the network can actually be fed](#what-the-network-can-actually-be-fed).

**Watch out for per-game overrides.** A renderer pinned on one game silently beats your global
setting, and that cost me an evening. **Right-click the game in the list → Properties →
Graphics** and make sure Renderer is *Direct3D 11* or left on the global setting. In
`gamesettings\<SERIAL>.ini` the same thing reads `Renderer = 3` for D3D11 and `15` for D3D12;
deleting the line falls back to your global setting.

### 6. Start a game

**Home** → **Add-ons** tab → **DLSS Neural Rendering (AMD)**.

**It starts switched off, every launch.** Tick **Enabled**, or press `Ctrl+End`. That is
deliberate: the add-on rewrites every frame the game presents, and a couple of the settings can
take the display driver down, so nothing happens until you have seen what it is set to.

The defaults are fine to start with and there is nothing you have to change. When you do change
something, **Save Settings** keeps it for next time — otherwise every launch starts fresh.

Every control has a `(?)` next to it. Hover it. A control shown in **red** is holding a value
that can reset your display driver; **amber** means past what has actually been measured. The
legend is at the bottom of the panel.

The status line says whether it is really running, and `dlss5-neural.log` next to the exe has
the details.

---

## Troubleshooting

| What you see | What it is |
|---|---|
| Add-on isn't in the Add-ons tab at all | `ReShade.ini` has `DisabledAddons=dlss5 neural@dlss5-neural.addon64` under `[ADDON]`. ReShade writes that line if you ever untick the add-on, and then it never loads again, with no error anywhere. Clear it. |
| Status says the API is wrong | The game is on Vulkan or OpenGL. It needs D3D11 or D3D12. Check the per-game override too, not just the global setting: right-click the game in the list, Properties, Graphics. |
| `HIP: amdhip64_7.dll failed to load` | HIP 7 isn't installed. HIP 6 doesn't count. |
| `hash mismatch; refused` | Wrong `dlssnr_amd_pass1.dll`. Compare against `tools/SHA256SUMS.txt`. The refusal is deliberate — the alternative is a hang. |
| Game dies with `887A0005` / device removed | `DXGI_ERROR_DEVICE_REMOVED`, from a Windows TDR. See [the engine ini](#the-engine-ini). |
| It runs but "only shifts the colours a bit" | Usually Resolution Scale, see the next row. If that isn't it, try **Encoding**: the network was trained on linear light, so handing it sRGB values and calling them linear is the wrong domain, and the symptom is exactly this. Linear with Diffuse White at 100 matches what the NVIDIA route feeds it. |
| Colour and tone change, but **textures look identical** | Resolution Scale. At the default 0.50 the network is handed a half-resolution image, so the finest thing it can see is two screen pixels wide — it cannot put detail into a texture it was never shown, and the only correction it can make is colour, tone and large-scale shading. Textures change at **1.00** and not before. That is four times the cost, so set **Timing** to *Async* first; the correction then lags a few frames instead of stalling the game. |
| Pass Count above 1 changes nothing, or costs a lot | Expected. It has never measured better, and the only reading ever taken of it was `Passes=3` giving a residual of exactly zero. It is an instrument, not a quality setting. It no longer needs extra `pass2..10.dll` files — delete any you have. |
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

## The settings

Every control has a `(?)` beside it, and the tooltip is where the real documentation lives — it
says what the control does and what was actually measured about it. What follows is only the
part you would want before opening the panel.

**Red and amber.** The colour tracks the value a control is holding, not the control itself, so
turning it back down clears it.

| | |
|---|---|
| **Red** | This value can take the display driver down. The game dies on `DXGI_ERROR_DEVICE_REMOVED` and the desktop goes with it, with nothing in any log pointing back here. |
| **Amber** | Past what has been measured on this machine. Not known to break, not known to work either. Change one thing at a time and watch the skip rate under Status. |

**Timing** is the one to understand first. *Same frame* blocks the game on the GPU until the
network finishes, so you see this frame's own correction — honest, and the mode that turns any
slowdown into a stall. *Async* shows a correction a frame or two old, but nothing blocks and an
overrun costs a skipped frame instead of a hang. Anything expensive wants Async.

**Resolution Scale** decides whether textures can change at all. Below 1.00 the network never
sees a full-resolution pixel, so the most it can do is colour, tone and large-scale shading —
which is why the effect reads as a colour filter. 1.00 costs four times what 0.50 does.

**Pass Count** is an instrument, not a quality setting. It has never measured better, and the
only reading ever taken of it was `Passes=3` giving a residual of exactly zero. Raise it to
compare two `measure, residual` lines, not to play.

**Save Settings** writes everything to `dlss5-neural.ini` next to the exe; without it the panel
is a scratchpad. **Language** switches the whole panel, tooltips included, between English and
Brazilian Portuguese.

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

**It depends entirely on the renderer, and that is the single most important thing on this
page.** On D3D11 the network gets colour, depth and motion. On D3D12 it gets colour and nothing
else — not because depth is missing, but because it is out of reach there. Measured with the
probe in `src/probe`, same game, same frame, only the renderer changed:

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
* ~~Depth. Motion.~~ Both done, on D3D11, through the bridge. Depth is copied out of the game
  and converted before it crosses; motion comes from the game's own velocity buffer where there
  is one, and from a block-matching estimator where there isn't. Still open: nobody has read the
  guide probe **during real gameplay**, only on menus, where flat depth and zero motion are what
  you would expect anyway. That reading is the next thing worth having.
* Exposure. The fourth slot of the network's input packet is still never filled, and it has
  never been shown that the engine reads it.
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

## Keeping this going

<div align="center">

### This is one person, one card, and evenings.

[![Support this project on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/T6T213OVFE)

**[ko-fi.com/T6T213OVFE](https://ko-fi.com/T6T213OVFE)**

</div>

There is no company behind this and nothing here is sponsored. Every result in this README came
out of one RX 9070 XT, and the honest limits of the project are written down all over it —
"tested on one card", "measured on menus, not in gameplay", "not reproduced since the rework".
Those gaps are not laziness. They are what one machine and one pair of hands can cover.

What support actually buys, in the order it would get spent:

| | |
|---|---|
| **Tokens, and they are metered** | The research that actually moves this forward -- decompiling a closed runtime, bisecting a driver reset, chasing one field through a binary until it gives up what it does -- burns tokens, billed by use. That meter is the single biggest thing deciding whether the next question gets answered or shelved instead. |
| **Other hardware** | Everything is verified on a single RDNA4 card. RDNA3 is untested. A second card is the difference between "should work" and "measured". |
| **Time** | The useful work here is slow, and most of it produces one number rather than a feature. Running the same scene twice to find out a slider was inert is a whole evening, and evenings are the scarce thing. |
| **The targets nobody has tried** | Adding a game is one row in a table, but *verifying* one means owning it and playing it. |

If this saved you a weekend, or if you just want to see where it goes, a coffee genuinely helps.
If it didn't, don't — the code is MIT either way and nothing is gated behind a donation.

## License

MIT, in `LICENSE`. Third-party headers under `external/` keep their own licenses, listed in
`external/reshade/NOTICE.md` — BSD-3-Clause OR MIT for ReShade, MIT for Dear ImGui. No network
binaries in here and it doesn't download any.
