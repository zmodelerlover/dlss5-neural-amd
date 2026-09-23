# The OpenGL route

> **Status: experimental, and further along than that sounds.** The route is written, builds into
> the single add-on behind `AMDNR_WITH_OPENGL`, and has run the network end to end over a real
> OpenGL game (Luanti 5.17.0, Minetest Game, AMD RX 9070 XT, Adrenalin 26.8.1). It hands over
> between the two APIs on the GPU with imported D3D12 fences, it copes with a multisampled default
> framebuffer, and resize has been exercised with the effect on. It still carries no guides.
>
> **Shipped in v0.6.0**, and installable: the installer detects a 64-bit OpenGL game, routes it and
> puts ReShade in as `opengl32.dll`. A 32-bit OpenGL game still has no route. What is left is under
> [What is not done](#what-is-not-done).

Everything below was measured on this machine rather than reasoned about. Where a number appears,
it came out of a run; where a rule appears, it cost something to learn.

## Why the route has the shape it has

The network is not portable and is not ours. `InitEngine` writes an `ID3D12Device*` and an
`ID3D12CommandQueue*` into fixed offsets inside `dlssnr_amd_pass1.dll` and matches a HIP device by
LUID (`core/addon/neural.cpp`). That device is **ours** — `CreateWorkDevice` builds a private D3D12
device on the same adapter as the game — not the host's. Every route in this project is therefore
the same idea with a different transport: get the presented frame onto our device, run the network,
put the result back.

That forces the direction of the crossing. Memory exported from OpenGL is not something D3D12 can
open, so the shared textures are created on our D3D12 device, exported as NT handles, and imported
into the host's GL context. Identical to the Vulkan route (`vk_route.inc`), and for the identical
reason.

```
D3D12 CreateCommittedResource(D3D12_HEAP_FLAG_SHARED) -> CreateSharedHandle
  -> glCreateMemoryObjectsEXT + GL_DEDICATED_MEMORY_OBJECT_EXT = 1
  -> glImportMemoryWin32HandleEXT(GL_HANDLE_TYPE_D3D12_RESOURCE_EXT)
  -> glTexStorageMem2DEXT, then an FBO wrapping that texture
```

What is not identical is everything on the host's side, and the differences are not stylistic:

| | Vulkan route | OpenGL route |
|---|---|---|
| Back buffer | a `VkImage` to copy | the default framebuffer — nothing to copy |
| Crossing | `vkCmdCopyImage` into ReShade's command buffer | `glBlitFramebuffer` between FBO 0 and our FBO |
| Command recording | ReShade's immediate command buffer | none; GL calls go into the current context |
| Device setup | must hook `vkCreateDevice` to add interop extensions | nothing to hook; the context has them or it does not |
| State | scoped to a command buffer | global to the context, and the game is using it |

The last row is the one that produces field bugs. The first — no device hook — is why this route is
about 350 lines shorter than the Vulkan one despite doing more per frame.

## What the driver allows: `glprobe`

`core/diagnostics/glprobe/glprobe.cpp`, built with `.\build.ps1 -Target glprobe -Exe`, answers the driver's half
of the question with no game and no ReShade. Vulkan could be asked with a query
(`vkprobe`); OpenGL cannot — the extension string says which entry points exist, never whether an
import will be refused — so this one builds the crossing for real and reports what happened. Every
GL call that touches imported memory runs inside `__try`, and there is a control row that performs
the same operations on an ordinary texture, because "the driver faulted" and "this probe is wrong"
look identical without one.

Results on an RX 9070 XT, Adrenalin 26.8.1, in both a 4.6 compatibility context and a 3.3 core one:

| Check | Result |
|---|---|
| `GL_EXT_memory_object`, `_win32`, `GL_EXT_semaphore`, `_win32` | present (`GL_EXT_win32_keyed_mutex` missing, unused) |
| Import as `D3D12_RESOURCE`, all six formats the routes carry | every one imports, every one is FBO-complete |
| Memory really shared | bytes identical D3D12 → GL and GL → D3D12 |
| Default framebuffer → imported texture → D3D12 | the blit lands, Y inverted |
| `glWaitSemaphoreEXT` on an imported D3D12 fence | completes — a GPU-side handover is available |
| `GL_DEVICE_LUID_EXT` | reported, matches a DXGI adapter exactly |

Three rules came out of it that are in no specification:

1. **The shared NT handle is not the application's to close.** Vulkan takes its own reference and
   expects the exporter to close its handle; `EXT_memory_object_win32` is written the same way and
   this driver does not honour it. Closing a handle after the import faults inside the ICD, from a
   driver thread, at an unpredictable later moment — so the crash lands in an unrelated GL call.
   Measured with everything else held constant: **3 faults in 8 runs with closing, 0 in 8 without.**
   The route keeps its handles for the life of the process and leaks two per swapchain size,
   deliberately.
2. **The memory object must be declared dedicated before the import.** A D3D12 resource handle
   backs exactly one resource. This is `VkMemoryDedicatedAllocateInfo` wearing another hat.
3. **`GL_HANDLE_TYPE_OPAQUE_WIN32_EXT` is accepted for a D3D12 resource handle, and is wrong.** The
   import returns no error, the storage is created and the FBO is complete. So "the import
   succeeded" proves nothing about the handle type. The route names `D3D12_RESOURCE` and stands
   down if that is refused rather than falling back to something that appears to work.

The probe exits 0 when the route is possible, and prints a verdict either way.

## What ReShade hands an add-on: `glinfo`

`core/diagnostics/glinfo/glinfo.cpp` is a small add-on that answers the other half, which needs a game. The
existing `probe` add-on cannot: it filters its inventory to `resource_type::texture_2d`, and under
OpenGL nothing it sees is one — it reports "2 distinct render targets" and prints no rows.

Measured under Luanti (menu, then a Minetest Game world):

**The back buffer is the default framebuffer.**

```
back buffer 0  handle 0082180000000405  GL_FRAMEBUFFER_DEFAULT  name 1029 (GL_BACK)
               surface 1920x1009, format 87 (b8g8r8a8_unorm), 1 sample
```

`resource_type::surface`, not a texture. There is nothing to copy, which is what makes the whole
route a pair of blits.

**Binds arrive, and depth with them — in a 3D scene.** In the main menu there was one bind event in
445 frames and the only depth was the default framebuffer's own attachment. In a world, with
post-processing on, there are two bind events per frame and the engine's own targets appear:

```
colour 0  GL_TEXTURE_2D  name 4204  1920x1009, format 24 (r10g10b10a2_unorm)
depth     GL_TEXTURE_2D  name 4205  1920x1009, format 942944836
colour 0  GL_FRAMEBUFFER_DEFAULT ...   (the final composition)
depth     GL_FRAMEBUFFER_DEFAULT ...   (d24_unorm_s8_uint)
```

4492 bind events over 2370 frames, exactly two distinct colour targets and two distinct depth
targets throughout. The offscreen depth is a real texture, which is what makes guides possible at
all — but `942944836` is `0x38343244`, the FourCC `D248`, a value absent from
`reshade_api_format.hpp`. It is `GL_DEPTH24_STENCIL8` with no DXGI equivalent, so a guide path
cannot trust `resource_desc.texture.format` and cannot convert to `R32F` with a blit: depth blits
require matching formats. Guides in OpenGL cost a shader pass.

**The add-on is loaded and unloaded seven times during start-up.** Under SDL — Luanti, and a great
many OpenGL games — contexts are created and destroyed while the window is set up, and ReShade
loads its add-ons once per context. Seven cycles in 230 ms, with timestamps. Anything expensive in
`DllMain` happens that many times, and a log opened with `"w"` keeps only the last cycle.

**ReShade forces the context version.** Its log: `Requesting compatibility OpenGL context for
version 3.2 → Replacing requested version with 4.3`. So direct state access (4.5) is not
guaranteed, and the route uses `glTexStorageMem2DEXT` rather than the DSA form. `glprobe` confirms
both work on this driver in a 3.3 core context as well.

## The route

`core/transport/opengl/gl_route.inc`, included from `neural.cpp` behind `AMDNR_WITH_OPENGL`
(`core/addon/build_config.h`). Integration is five small edits, mirroring the Vulkan ones: the
include, the `device_api::opengl` branch in `OnPresent`, a call in `ReleaseSwapchainSized`, the API
name in the status log, and the description string. There is no `DllMain` work at all — unlike
Vulkan, nothing has to be arranged before the host's device exists.

One frame:

1. **`BlitIn`** — save GL state, disable scissor and `GL_FRAMEBUFFER_SRGB`, read from FBO 0 with
   `glReadBuffer(GL_BACK)`, blit into the imported `colour-in` texture with Y inverted, `glFinish`,
   restore state.
2. **Our device** — `CopyResource` into `crossLocal`, `RecordNetwork`, the result into the imported
   `result-out` texture, submit on the work queue, wait for the fence.
3. **`BlitOut`** — same in reverse, Y inverted again, into FBO 0 with `glDrawBuffer(GL_BACK)`.

Decisions worth writing down:

- **The crossing is `R8G8B8A8_UNORM`, not the `b8g8r8a8_unorm` ReShade reports.** That report
  describes the default framebuffer, not a memory layout. The blit writes our texture through GL's
  view of it, which is RGBA8 — byte 0 is red — and D3D12 must read those bytes the same way round.
  A BGRA crossing would put blue where the network expects red, with no error anywhere and a
  picture that looks plausible until the sky is orange.
- **Y is inverted in both directions and the inversions do not cancel.** OpenGL's origin is bottom
  left; every other surface in this add-on is top-left. See [Results](#results) for the run that
  proves the out-blit is really landing.
- **State put back:** read and draw framebuffer bindings, the read and draw buffer selection of FBO
  0, the scissor test and `GL_FRAMEBUFFER_SRGB`. Per the spec those last two are the only fragment
  operations a blit obeys. The route also drains the GL error queue around its own work, which
  consumes the game's pending errors as a side effect — a real if small cost of being here.
- **Everything is keyed on `wglGetCurrentContext()`.** When the context changes, the GL objects
  belonged to a context that may already be gone: the route forgets them rather than deleting them,
  keeps the handles, and rebuilds. `ReleaseSwapchainSized` can be called on a thread with no context
  at all and checks before touching anything.
- **Synchronisation is the imported fences, and falls back to the CPU stall.** `crossHandle` and
  `backHandle` -- the two shared D3D12 fences the project already creates -- are imported as GL
  semaphores, the same pair the Vulkan route imports. GL signals after the blit in, the work queue
  waits on that value before executing, the queue signals when the network is done, and GL waits
  on that before the blit out. No thread blocks.

  It is not taken on trust. The first frame is still confirmed on the CPU with a two-second
  bounded wait, and only then is the stall dropped; if that wait times out, the route falls back
  to stalling for the rest of the session and says so in the log. A fence wait that never
  completes is a hung queue and a game that has to be killed, which is not a failure mode to
  discover in someone else's session. `GlSemaphores=0` in the ini forces the stall, which is how
  the two were compared below and the first thing to try if a GL host misbehaves.
- **Every frame that reaches the screen has been through the network.** When the previous
  evaluation has not finished, the route waits for it rather than letting the frame through
  uncorrected. That pacing is the default and it is not only about correctness of the picture: a
  route that lets some frames through untouched makes the game alternate between two different
  images, and at a hundred and eighty frames a second that is a strobe rather than an effect.

  This was found the hard way. The first semaphore build dropped the CPU stall and let the game
  run ahead, and the frames the network could not answer went out as the game rendered them --
  reported from the outside as "the effect turns on for one frame and then off". Worth noting
  that the guard is older than the fences: with the game running fast enough, the CPU-stall path
  skipped about two frames in three as well, because the engine's own job counter lags even when
  our fence has completed. Waiting on the outstanding job and re-testing, rather than skipping,
  is what fixed both.

- **Repeating the last result is available and is not the default.** `GlHoldFrames=N` lets up to
  N frames in a row show the last composition again instead of waiting, which raises the present
  rate -- measured at roughly 180 against 97 -- by filling the gaps with duplicates.

  It is off because of what it does to every frame counter in the system. A repeated frame is one
  the player cannot distinguish from a new one, so the game's counter, the overlay's and the
  driver's all report a rate nobody is seeing: 181 presents a second carrying 61 distinct images.
  The number on screen should mean what it says, and the route should not be the reason it does
  not. On a variable-refresh display, with eyes open about the trade, the switch is there.
- **A multisampled default framebuffer is resolved on the way in.** The sample count is read from
  GL (`GL_SAMPLES` with FBO 0 bound) rather than taken from ReShade's description of the back
  buffer -- the two agree on this driver, and the log prints both so they can be compared
  elsewhere. Above one sample the frame is resolved into an ordinary texture first and flipped in
  a second blit, because one blit cannot be asked to resolve and mirror at once: with a
  multisampled read framebuffer the spec wants source and destination rectangles of the same size,
  and a mirrored rectangle is a reasonable thing for a driver to refuse. On the way out a
  single-sampled source blitted into the multisampled default framebuffer replicates to every
  sample, which is right for a frame that has already been resolved.
- **`Stage=2` means transport-only.** The existing staging setting — "1 stops before D3D12 loads, 2
  before the engine, 3 is everything" — does more work on this route than a diagnostic. It crosses
  the frame to D3D12 and back untouched, so every way the transport can be wrong is visible on
  screen instead of in a log: upside down is a missing flip, orange sky is the channel order, black
  is memory that never aliased.

## Two bugs the integration produced

**Re-entrancy, which closed the game.** In OpenGL the route's own calls go through the same hooks
the game's do, so ReShade reports our `glBindFramebuffer` back to this add-on as a render-target
bind — on the present thread, inside `OnPresent`, which already holds `g.lock`. Taking a
`std::mutex` twice on one thread does not deadlock under MSVC: it throws `std::system_error`, which
is unhandled, which closes the game. The log stopped mid-way through building the crossing and the
only other evidence was `E06D7363` in `KERNELBASE`.

The fix is a `thread_local g_selfIssued` flag and a `SelfIssued` RAII guard in `neural.cpp`; the
observers return immediately when it is set. They would be wrong to count our binds in any case —
the route's binds are not the game drawing its frame. D3D11 and D3D12 never needed this because
there the bridge records into its own command list on its own device, where ReShade has nothing to
say.

**Operator precedence, which crashed on the first frame.** In the job-pending test, `&&` binds
tighter than `||`, so the side that reads the engine's job counter through `g.runtime` was
evaluated even in transport-only mode, where no engine is loaded. Parentheses.

Two diagnostics from that hunt are kept in the route, because this driver has already been seen to
fault from a thread of its own, far from the cause:

- step-by-step logging through the first build of each crossing (eight lines, once per size);
- an unhandled-exception filter that names the module and offset and then chains to the previous
  filter. It is what turned "the game closed" into `atio6axx.dll+0xA9A1FE` and later into
  `amd-nr.addon64+0x195C3`.

## Results

**Transport only (`Stage=2`), Luanti in a world:** two runs of roughly 35 s, 1618 and 1903 frames
-- somewhere around 45 to 55 fps, against the same scene's own rate -- no faults, picture identical
to the game's own.

An identical-looking pass-through proves nothing, so the out-blit's inversion was removed on
purpose and the run repeated: **the entire game image came back upside down** — world, HUD and chat
text — while the window title bar and taskbar stayed the right way up. What is on screen is the
route's output, not the game's image, and the shipped flip is correct.

**Synchronisation, measured** -- same ini, same scene, 40 s each, `Stage=3`, scale 0.50, one
pass, inline timing:

Every row below is 40 s in the same world with the same ini, `Stage=3`, scale 0.50, one pass,
inline timing. "Distinct images" is what the player actually sees change.

| | frames | present rate | distinct images | frames left uncorrected |
|---|---:|---:|---:|---:|
| Fences, pacing on (**the default**) | 3897 | ~97 fps | ~97/s | 0 |
| CPU stall, pacing on (`GlSemaphores=0`) | 3438 | ~86 fps | ~86/s | 0 |
| Fences, `GlHoldFrames=3` | 4520 in 25 s | ~181 fps | ~61/s | 0, but two in three repeat |
| First semaphore build, no pacing | 7554 | ~189 fps | ~64/s | ~5000 -- **strobed** |

The fences are worth about 13% over the stall with everything else equal, and that is the whole of
their benefit: they remove two synchronisation points per frame, they do not make the network
faster. The last row is the build that shipped the strobe and is here as a record, not an option.

The third row is the honest picture of what holding buys: nearly twice the presents, fewer
distinct images than the default. A frame counter cannot tell the difference; a player looking at
motion can.

**Multisampling:** Luanti with `antialiasing = fsaa`, `fsaa = 4` and post-processing off gives a
genuinely multisampled default framebuffer -- ReShade and GL both report 4 samples -- and the
route resolves it: 4996 frames in ~27 s with the network running, and a screenshot indistinguishable
from the same scene with the add-on removed.

**With the network (`Stage=3`, scale 0.50, one pass):**

```
opengl: bridge up at 1920x1009 ... blit between the default framebuffer and a D3D12 texture
HIP: adapter AMD Radeon RX 9070 XT matches the game's D3D12 device
engine ready.
opengl: first full round trip done at 1920x1009.

measure, network input:   mean absolute 0.565629      (not a black frame — the image crossed)
measure, residual:        mean 0.019837, max 0.125244 (the network returned a correction)
measure, residual detail: ratio 0.193                 (the correction follows the image's detail)
```

345 frames in ~35 s, about 10 fps, against roughly 50 for the transport alone. The difference is the
network at half resolution plus two CPU stalls per frame — which is exactly what the semaphore
handover is for.

## How it shipped

Released as add-on **v0.6.0** (`amd-nr.addon64`, `c037a69f…`), with the payload the
installer reads pinned to those bytes, and installer **v0.2.0** carrying the detection and the
`opengl32.dll` proxy. The installer's own self-update was exercised end to end by the user after
publication and worked.

Two things went wrong on the way out, both recorded in the installer's handoff for the same date:
the two copies of `payload.json` had drifted, and publishing from the stale one pushed bridge
hashes live that did not match the published files -- caught by the manifest verification pass,
which had never been reaching the end because the publish script died on a line `hf` writes to
stderr. Both are fixed; the verification now passes on all nine files.

## Reproducing it

Paths below are placeholders; nothing in this document needs the game to live anywhere in
particular. The game is deliberately kept **outside** the repository.

```powershell
# 1. the add-on and the two diagnostics
.\build.ps1 -Target neural
.\build.ps1 -Target glinfo
.\build.ps1 -Target glprobe -Exe ; .\build\amd-nr-glprobe.exe    # driver check, no game needed

# 2. ReShade with full add-on support, for OpenGL, into the game's folder only
.\ReShade_Setup_6.8.0_Addon.exe --headless --api opengl "<game>\bin\<game>.exe"

# 3. the runtime the add-on is pinned to, built by this project's own tools
python tools\extract_runtime.py <dlssnr_on_amd_setup.exe v0.3.0> version.dll
#   -> 8321cae728d28cb7632d0d58d3d913e91132bf7645c126505698fbe4cd5a0138 (original_sha256)
python tools\patch_runtime.py version.dll tools\runtime-patches.json dlssnr_amd_pass1.dll
#   -> 70af3fb757f83f71ec947ce461970fdecc9636864bc01d952abffb36ae310be6 (SHA256SUMS.txt)

# 4. beside the game's exe: amd-nr.addon64, dlssnr_amd_pass1.dll,
#    dlssnr_on_amd_weights.bin (6bf8dc93...), and a amd-nr.ini
```

The runtime pairing is worth stating plainly, because a mismatch here is refused with a hash error
and it is easy to arrive at the wrong file: the add-on consumes the **patched** runtime, and
`runtime-patches.json` currently targets **DLSS-NR-on-AMD v0.3.0**, not the v0.2.17 the CHANGELOG
and the superseded 2026-09-10 preview mention. The vendor's own installer output, and any
`dlssnr_amd_pass1.dll` already sitting in a game folder, will not match.

`amd-nr.ini` for a scripted session:

```ini
[dlss5]
StartOn=1        ; no hotkey needed
Stage=2          ; transport only; 3 runs the network
Scale=0.5
Passes=1
GlSemaphores=1   ; 0 forces the CPU stall, for comparison or when a host misbehaves
GlHoldFrames=0   ; above 0 repeats the last result instead of waiting, and inflates every
                 ; frame counter by the number of duplicates
```

The panel writes the full settings back to this file as soon as a control settles, so a file
edited by hand between runs is not necessarily the file the next run reads. For measurements,
write it fresh each time -- two runs that differ in `Scale` are not a comparison of anything
else.

Luanti specifically, which is a good first host — free, 64-bit, pure OpenGL, no account, and it can
be driven entirely from the command line:

```powershell
7z x luanti-5.17.0.exe -ogame          # the download is an NSIS installer; extract, do not install
#   the payload lands under $LOCALAPPDATA\luanti\<version>\ inside the extraction
#   a game is needed too: ContentDB, e.g. https://content.luanti.org/packages/Luanti/minetest_game/download/
#   into game\games\minetest_game\
.\bin\luanti.exe --go --world <path>\worlds\<name> --name probe
```

Create `worlds\<name>\world.mt` with `gameid = minetest_game` and the engine generates the rest on
first run. The offscreen colour and depth targets only exist when post-processing is on -- it was on in
these runs, and `minetest.conf` beside `builtin\` can set `enable_post_processing` and
`enable_dynamic_shadows` explicitly. Without them the engine draws straight into the default
framebuffer and there is no guide-shaped texture to find, which is also what the main menu looks
like.

## What is not done

In the order the value falls out:

1. **Guides.** Depth is reachable in a 3D scene, as a texture, but it needs a shader pass to
   become `R32F` and its format must be read on the GL side rather than from `resource_desc`.
   Motion is estimated, as on Vulkan.
2. **The panel.** Only the API name was added. The OpenGL route reports no status of its own, so
   the Status section is thin under a GL host, and `GlSemaphores` is an ini-only switch with no
   indication anywhere that it is off.
3. **Start-up cost under SDL.** `DllMain` runs seven times in an SDL host. Nothing there is
   expensive enough to have shown up yet, but the log truncation is already visible and the
   engine bring-up would not survive the same treatment gracefully.
4. ~~**CI and a contract test.**~~ Done on 2026-09-20. `.github/workflows/build.yml` builds
   `glinfo` and `glprobe` too, and `tools/opengl_import_check.py` holds the line the route depends
   on: no source calls a `gl`/`wgl` entry point by name, no `#pragma comment(lib, "opengl32")`, no
   `opengl32.lib` on the link line every target shares, and the built add-on imports
   `opengl32.dll` neither statically nor through the delay-load table. That import is the trap the
   NFS 2015 comment in `neural.cpp` documents, and it used to be one careless pragma away.
5. **A second host.** Everything here is one game on one driver. Xonotic (DarkPlaces, 64-bit, free)
   is the obvious next one, and a 3.3 core host would exercise the non-DSA path in anger.

## Files

| Path | |
|---|---|
| `core/transport/opengl/gl_route.inc` | the route |
| `core/addon/build_config.h` | `AMDNR_WITH_OPENGL` |
| `core/addon/neural.cpp` | the branch in `OnPresent`, the teardown hook, `SelfIssued` |
| `core/diagnostics/glprobe/glprobe.cpp` | the offline driver probe (`-Target glprobe -Exe`) |
| `core/diagnostics/glinfo/glinfo.cpp` | the in-game ReShade probe (`-Target glinfo`) |
| `tools/opengl_import_check.py` | the contract test: nothing links to OpenGL, in source or in the built add-on |
