# Project handoff — 2026-09-20

The state after the OpenGL route was written, measured, released as **v0.6.0** and published to
the payload the installer reads. Read this before the older handoffs: they describe branches and
work that have since shipped.

## 1. Where things stand

| | |
|---|---|
| Branch | `master` at the OpenGL commit, and nothing beside it. The branch this handoff first called `opengl_study` was really `origin/opengl`, zero commits either way from `master`; it was deleted on 2026-09-20 |
| Tag | `v0.6.0`, with `dlss5-neural.addon64` and `SHA256SUMS.txt` on the release |
| Runtime | unchanged: pinned **DLSS-NR-on-AMD v0.3.0**, patched by `tools/patch_runtime.py` to `70af3f…` |
| Payload | `addon` bumped to **0.6.0** (`c037a69f…`) on the dataset the installer reads |
| Installer | **v0.2.0** installs into OpenGL games; its self-update was tested end to end and worked |

Routes now: D3D11 (the only one with the game's own depth and motion), D3D12, Vulkan, **OpenGL**,
and the 32-bit pair for D3D8/D3D9/D3D11. A 32-bit OpenGL game has no route.

## 2. What the OpenGL route is

`src/neural/gl_route.inc`, behind `DLSS5_WITH_OPENGL` in `build_config.h`, included from
`neural.cpp` beside the Vulkan route. The network still runs on this add-on's own private D3D12
device; the crossing is created there and imported into the host, exactly as on Vulkan.

**`docs/opengl-route.md` is the document to read.** It carries every measurement, the reasoning
behind each decision, and the reproduction steps. What follows is only what a person resuming the
work needs in the first five minutes.

Five things that are not obvious and cost time to learn:

1. **ReShade hands an OpenGL add-on the default framebuffer**, `GL_FRAMEBUFFER_DEFAULT` with
   `resource_type::surface` — not a texture. Every copy in the route is a `glBlitFramebuffer`
   because of that, and Y is inverted going in and again coming out.
2. **An imported texture's shared NT handle is not ours to close.** Closing it faults inside the
   AMD ICD from a driver thread, some time later, so the crash lands in an unrelated call.
   Measured: 3 runs in 8 with closing, 0 in 8 without. The route keeps every handle for the life
   of the process and leaks two per swapchain size, deliberately.
3. **`GL_HANDLE_TYPE_OPAQUE_WIN32_EXT` is accepted for a D3D12 resource handle and is wrong.** The
   import succeeds, the storage is created, the FBO is complete. The route names
   `D3D12_RESOURCE` and stands down rather than falling back.
4. **The route's own GL calls come back as ReShade events.** They arrive on the present thread
   inside a lock this add-on already holds, and a second `std::mutex` lock throws under MSVC
   rather than deadlocking — that closed the game until `SelfIssued` was added in `neural.cpp`.
   Anything new that issues host API calls from inside a callback needs the same guard.
5. **The add-on is loaded and unloaded ~7 times during start-up under SDL hosts**, once per GL
   context the host creates and destroys. `DllMain` runs every time, and the log opened with `"w"`
   keeps only the last one.

## 3. Two settings this route added

Both are ini-only, read at load, never written back — the `Stage`/`Events`/`NoBridge` family.

| Key | Default | What it does |
|---|---|---|
| `GlSemaphores` | 1 | Hands over between the two APIs with the imported D3D12 fences. `0` forces the CPU stall the other routes use. Worth about 13% here. |
| `GlHoldFrames` | 0 | Frames the network could not answer repeat the last result instead of waiting. Above 0 the present rate rises and part of it is duplicates, which makes every frame counter read high. |

`GlHoldFrames` defaulting to 0 is a decision, not an oversight: a repeated frame is one the player
cannot tell from a new one, so a route that repeats frames makes the game's own counter lie. With
it at 0 every frame that reaches the screen has been through the network.

## 4. Diagnostics that exist now

| | Build | Needs a game |
|---|---|---|
| `glprobe` | `.\build.ps1 -Target glprobe -Exe` | no — asks the driver whether the crossing is possible at all, and prints a verdict |
| `glinfo` | `.\build.ps1 -Target glinfo` | yes — reports what ReShade hands an add-on inside a real OpenGL host |

`glprobe` is the one to run first on any new machine or driver: if it does not print an open
verdict, nothing else about OpenGL on that machine is worth debugging. Both are written so a
driver fault becomes a line of output rather than a dead process.

## 5. The test host

Luanti 5.17.0 with Minetest Game, kept **outside** the repository at `E:\Projetos\luanti`, driven
entirely from the command line:

```powershell
.\bin\luanti.exe --go --world <path>\worlds\<name> --name probe
```

It is a good first host: free, 64-bit, statically imports `OPENGL32.dll`, renders into its own
offscreen targets when post-processing is on, and can be made to give a multisampled default
framebuffer with `antialiasing = fsaa`, `fsaa = 4`. The panel rewrites `dlss5-neural.ini` when a
control settles, so write the ini fresh before each measurement or two runs will differ in
`Scale` and not in what you were testing.

Not yet tried on a second host or a second driver. Xonotic (DarkPlaces, 64-bit, free) is the
obvious next one, and a 3.3 core host would exercise the non-DSA path in anger.

## 6. What is not done

1. **Guides.** Depth is reachable in a 3D scene as a real `GL_TEXTURE_2D`, but it needs a shader
   pass to become `R32F` and its format has to be read on the GL side — ReShade reports the
   FourCC `D248` for it, which is not in `reshade_api_format.hpp`. Motion stays estimated.
2. **The panel.** Only the API name was added. Under a GL host the Status section is thin, and
   `GlSemaphores`/`GlHoldFrames` are ini-only with nothing on screen saying they are off.
3. **Start-up cost under SDL.** Nothing in `DllMain` is expensive enough to have shown up yet, but
   it runs seven times and the engine bring-up would not survive the same treatment gracefully.
4. **A 32-bit OpenGL frontend**, if a target ever justifies it. The 32-bit pair covers D3D8, D3D9
   and D3D11.
5. ~~**CI.**~~ Done on 2026-09-20. `.github/workflows/build.yml` now builds `glinfo` and `glprobe`
   as well, and `tools/opengl_import_check.py` is the contract test this asked for: no source calls
   a `gl`/`wgl` entry point by name, nothing names `opengl32.lib`, and the built add-on imports
   `opengl32.dll` neither statically nor through the delay-load table. The Windows job reads it off
   the binary it just built; the Linux job runs the source half, which needs no toolchain.

## 7. Published artefacts, for anyone verifying

```
dlss5-neural.addon64     574464  c037a69f31105a7bf029843fbf7e78b420d0f9dbfa0f89610ec2718a6a03942f
dlss5-neural.addon32     265216  3330e256182bf9f4f9c21b085fe0552d8d32cc12981fd644201d6a86d0f63316
dlss5-neural-host64.exe  446464  3340b1d89299420803821c120fa1c5d2314f6375346f901fb98c9b2945344631
dlssnr_amd_pass1.dll    7290880  70af3fb757f83f71ec947ce461970fdecc9636864bc01d952abffb36ae310be6
```

The 32-bit pair is unchanged since v0.5.3 and the bytes above are the published ones, not a local
rebuild — an MSVC rebuild of the same source is not byte-identical, and a release asset that
differs from what the installer downloads is the drift that caused the incident described in the
installer's handoff for the same date.
