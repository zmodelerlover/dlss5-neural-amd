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

## Videos

PCSX2, no audio. Click to play.

[![](media/pcsx2-0307.jpg)](https://github.com/zmodelerlover/dlss5-neural-amd/raw/master/media/pcsx2-0307.mp4)

[![](media/pcsx2-0320.jpg)](https://github.com/zmodelerlover/dlss5-neural-amd/raw/master/media/pcsx2-0320.mp4)

## You need

* AMD RDNA3 or RDNA4, with the HIP 7 runtime installed (`amdhip64_7.dll`). HIP 6 won't do.
* The game has to be running D3D12. On anything else the add-on just sits there doing nothing.
* ReShade with add-on support.
* `dlssnr_amd_pass1.dll` and `dlssnr_on_amd_weights.bin` in the game folder.

Those last two aren't in this repo. **They're in the `files` channel on the discord:**
**https://discord.gg/wYhvS3JSHM**

## Getting the runtime and the weights

The add-on needs `dlssnr_amd_pass1.dll` and `dlssnr_on_amd_weights.bin`, and neither is in this
repo. The weights are NVIDIA-derived and the runtime comes from a third-party project that
doesn't declare a license, so they're not going in a public repo.

> **Both are in the `files` channel on the discord → https://discord.gg/wYhvS3JSHM**

The dll there is already rebuilt without the spin cap, so you can drop it straight in. It has to
be exactly that build: the add-on checks it and refuses to load anything else, because the whole
thing is hardcoded offsets into one specific binary and pointing them at a different one hangs
the game.

If you'd rather rebuild it yourself, the untouched `version.dll` and `runtime-patches.json` are
in the same channel, run `tools/patch_runtime.py` on them.

## PCSX2 setup

Set the renderer to Direct3D 12. Careful with per-game overrides: PCSX2 keeps them in
`Documents\PCSX2\gamesettings\<SERIAL>.ini`, and a `Renderer = 3` line in there wins over your
global setting. That one wasted an entire evening for me. 15 is D3D12.

Install ReShade into `pcsx2-qt.exe` (Direct3D 10/11/12 option), then drop these next to the exe:

```
dlss5-neural.addon64
dlssnr_amd_pass1.dll
dlssnr_on_amd_weights.bin
```

Open `ReShade.ini` and check there's no `DisabledAddons=dlss5 neural@...` line under `[ADDON]`.
ReShade writes that if you ever untick the add-on, and then it silently never loads. That one
wasted an evening too.

Start a game, hit Home for the ReShade overlay, Add-ons tab, "DLSS Neural Rendering (AMD)".
The status line tells you if it's running. There's also `dlss5-neural.log` next to the exe.

What worked for me: Encoding sRGB, Resolution Scale 0.50, Pass Count 1, Apply On Same Frame on.

## Building it

Drop the ReShade headers into `external/reshade/`, plus `imgui.h` and `imconfig.h` from tag
`v1.92.5-docking`. The version has to be exact (ReShade wants 19250) and it has to be the
docking branch, because the overlay header uses `ImGuiDockNodeFlags` and `ImGuiWindowClass`.
Grab a close-enough version and it compiles fine, then the function table layout doesn't match
and things get weird.

```powershell
.\build.ps1 -Target neural
```

## Runtime

The OptiScaler installer patches the GPU wait shader down to 262144 iterations, about 6 ms.
The network takes 16 ms at half scale, 125-187 ms at full. So inline mode timed out on every
single frame, and the apply pass just kept the input. Residual came out at exactly zero, which
is a fun way to spend a few hours.

```powershell
python tools\patch_runtime.py version.dll runtime-patches.json dlssnr_amd_pass1.dll
```

That applies the four patches you actually need and skips the cap. Everything is written in
place at the same length so no offset moves. It prints a new SHA256; paste it into
`kRuntimeSha256` in `src/neural/neural.cpp` and rebuild. The hash check is there on purpose,
because pointing these offsets at a different build hangs the game.

## What it does per frame

```
back buffer -> colour prep -> network raster -> network -> residual -> compose -> back buffer
```

Only the network's correction gets resampled, the full-res image goes back untouched. It hooks
`present`, because `reshade_finish_effects` never fires if you have no shaders loaded.

One thing worth knowing: Encoding matters way more than it looks. On an 8-bit SDR back buffer
the right setting is sRGB. Pick scRGB-nl and it linearises something that's already sRGB and
scales it by 203/white, so the network gets a nearly black image and does nothing. Symptom is
"it only shifts the colours a bit". Ask me how I know.

## Numbers

PCSX2, God of War, 1920x974 back buffer, scale 0.50, 1 pass, RX 9070 XT:

```
network input, mean absolute   0.0259
residual, mean                 0.0165
residual, max                  0.777
per job                        15-16 ms
frames with a fresh correction 2184 of 2473
```

The add-on measures that itself on one frame and dumps it in the log. Worth keeping, because
"the network isn't doing anything" and "the network works and my compose is eating it" look
identical from the couch and need completely different fixes.

## Stuff I didn't get to

None of this is settled, it's just where I stopped. One card, one program, one night.

* Upscaling. I couldn't find an upscaling path in the AMD runtime I used, input and output
  share the same texture. Maybe another build has one.
* Model/style, UI correction, character mask. The NVIDIA add-on exposes these. I didn't find
  the fields on the AMD side, which might mean they're not there or might mean I missed them.
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

## License

MIT. No network binaries in here and it doesn't download any.
