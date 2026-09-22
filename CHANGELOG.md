# Changelog

## v0.7.0 - 2026-09-22 - AMD Neural Rendering

Same pinned **DLSS-NR-on-AMD v0.3.0** runtime and the same weights, so an upgrade is the add-on,
the companion effect, and nothing else.

**The project is now AMD Neural Rendering.** Every name this project owns has been renamed: the
add-on is `amd-nr.addon64`, the settings are `amd-nr.ini`, the log is `amd-nr.log`, the companion
effect is `AMD_Neural_Feed.fx`, and ReShade's Add-ons tab reads **AMD Neural Rendering**. Nothing
belonging to anyone else moved -- the runtime is still `dlssnr_amd_pass1.dll`, its log is still
`dlssnr_on_amd.log`, the NGX parameter names are unchanged, and the upstream project is still
[DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD) by danielblnc, which is what this
add-on drives and does not reimplement. **An existing `dlss5-neural.ini` is carried over to
`amd-nr.ini` on first run**, section header and all, and the old file is left where it is. Two
things a rename cannot migrate: a ReShade preset naming the old effect has to be re-enabled
against `AMD_Neural_Feed.fx`, and the `AMDNR_MV_PROVIDER` preprocessor definition starts again at
its default of Launchpad. The repository keeps its URL so links already in circulation keep
working.

### The correction is composed as a bounded ratio now, not added

This is the change that makes everything below it worth having. The add-on used to add the
network's correction to the frame channel by channel and clip whatever left the range -- and a
clipped channel is a hue rotation, not a stronger version of the same picture. That is what "three
passes looks deep fried" was, and it is why Pass Count was not worth using: two passes meant twice
the difference, three meant three times it, and none of it read as detail.

The answer is now made into a picture of its own, its luminance compared against the frame's as a
ratio, that ratio bounded, and two finished pictures blended. A bounded ratio cannot move hue. This
is what the OptiScaler DLSS-NR fork does and what RenoDX's addon did before it. `Composition` in
the ini still selects the old behaviour for an A/B.

Four controls come out of that composition, all of them new:

- **Colour Strength.** Whether the network's colour arrives with its light. At 0 every pixel keeps
  the game's own hue and only its brightness carries what the network decided -- by construction it
  cannot change colour there. This is the answer to "it changed the colours of my game".
- **Highlight Guard.** The most compose may move a pixel, as a multiple of what it already was. One
  scalar taken from luminance and applied to the whole triple, so it bounds brightness without
  touching hue. 2.0x by default, which is what 254 composition lines across seven games and nine
  RTX machines all read.
- **Residual Limit.** The control for blown blocks. A measured two-pass run came back with a mean
  correction of 0.072 and a **maximum of 4.16** -- four times brighter than white, in a picture
  whose own mean is 0.13. That is a tile where the network extrapolated rather than saw, and every
  extra pass ran on top of it. The whole correction is scaled rather than one channel clamped,
  because clamping one channel of a triple is the hue rotation this path exists to avoid.
- **Edge Fade.** Border tiles have no neighbour on one side, so what the network returns there is
  invented rather than seen, and bicubic upsampling rings on top of it. A corner sits inside two
  border bands at once, which is why the corners went first. Off by default.

**Bicubic residual upsample.** Below full Resolution Scale only the correction comes back up.
Stretching it bilinearly is a blur that throws away everything but colour and brightness, and that
alone was enough to make the whole effect look like a colour filter. Catmull-Rom keeps the rest.

### Neural Rendering Models A, B and C

The same three models `DLSSNR.Style` selects on NVIDIA, selectable live from the overlay. On NVIDIA
a model is two things: an input of the network, which is what moves lighting and detail, and a
grade on the finished frame. **This runtime has no slot for the first that anything here can
reach**, so here a model is its grade only -- B darkens by 0.1 stop, flattens contrast a quarter of
the way off its S-curve and removes a tenth of the saturation; C removes 15 percent of the
saturation. The constants were read out of `nvngx_dlssnr.dll` and verified against its SASS, and
the saturation slot works in HSL rather than HSV, which is checked against a real round trip in CI.

Two names for the same three values are in circulation, so the overlay prints both: RenoDX writes
Model A, B and C, Deep Fried Chicken writes Default, Natural and Cinematic.

**NR Preset is closed.** It is `DLSSNR.Hint.Render.Preset`, a weight-set hint; the shipping DLL
carries one set, its own log says `1 config(s) available`, and every other value falls back to it.
It does nothing here and it does nothing on NVIDIA either.

**One measured bug, found and fixed inside this cycle.** For about two hours the style vector was
written to engine offset `97b3c` on the reading that it was the network's fifth control. It is not
-- it is the runtime's `Scale`, and it belongs to the post kernel that writes the output. Model A
sends 0, so the effect was multiplied by zero: residual mean 0.00024 against an input of 0.45, with
"enabled" and "disabled" producing identical frames while every frame was reported as processed.
The field is back at the runtime's own default of 1/32, the ini refuses a value near zero, and the
overlay's slider will not reach it.

### The companion effect, and real motion vectors

`AMD_Neural_Feed.fx` hands the add-on a real optical-flow field from whichever motion-vector shader
is installed -- **iMMERSE Launchpad, VORT or LumeniteFX** -- along with ReShade's own depth buffer
with its `RESHADE_DEPTH_INPUT_*` fixes applied. It bundles none of them and includes none of them.

This matters most where the add-on has nothing of its own. On an emulator the PS2 never computed
per-pixel motion, so the only alternative is this add-on's built-in estimator: two levels of block
matching at a search radius of four, because it has to share the frame with the network. Launchpad
runs eight levels and filters between each one. A game that renders its own velocity buffer still
beats both, and the overlay says which one is actually feeding the network.

Depth is handed over in the range the network was trained on rather than raw. `DepthNormalise` is
still there, and it now ships **off** -- see below.

### The engine's option struct, mapped rather than guessed

The runtime's ini reader was decompiled, so the key string sits beside the address it writes.
Five fields this add-on never wrote, and two it wrote wrong:

- **`Temporal`** is the byte this project had labelled "motion is valid". That was a guess and it
  was wrong, and it explains a measurement nobody could account for: `Temporal=1` was the only run
  where the engine reported non-zero motion. Accumulating over time is what gives a motion vector
  something to point at.
- **`UseAutoMask`** is the engine's semantic character mask -- the same control RenoDX exposes as
  Character Mask. It defaults to 1 and was never written, so it has always been on by omission.
  Turning it off removes the effect from the whole frame, not just from characters.
- **`ToneChannels`**, **`Scale`** and **`Tonemap`** are exposed, with what is and is not known
  about each stated where it sits.

**Per-pass profiles.** A later pass is looking at a picture an earlier one already edited, so the
same numbers again ask it to sharpen its own sharpening. Local Tone now reaches the first pass only,
which is what the reference fork does in one deliberate line, and each pass can carry its own
Structure, Tone and Skin. Each pass also keeps its own history rather than sharing the chain's last
output.

### Weight parity, proven

153 tensors, byte for byte identical to `nvngx_dlssnr.dll`. **The network is NVIDIA's**, so any
difference in image between this and an RTX machine is in the composition, the controls or the fp8
arithmetic -- never in the model. The AMD kernels were carved out and read: the conditioning vector
lives only in LDS, which is why no external kernel, IAT hook or shared buffer can reach it, and why
feeding a style value into the network is closed by construction rather than by effort.

### Three controls that were broken, and are not any more

- **Skin Structure shipped at 1.0**, writing over the `-1` the engine boots with -- and `-1` means
  *automatic*, "derive it from local structure". The add-on turned that automatic off before anyone
  touched a control, while the overlay's own help text described it in the past tense. The default
  is `-1` again, and because `-1` is a mode and not a strength it is a checkbox now with the slider
  behind it, instead of a position on a `-1..3` scale where every value between `-1` and `0` meant
  nothing.
- **Tone Channels had four positions and two meanings.** The record path writes
  `(value & ~2) | 4` -- bits 2 and 4 stopped being tone channels and became the frame-timeout
  policy -- so slider position 2 was byte for byte identical to 0, and 3 to 1. It is a two-position
  control now. Bit 4 has to stay set for a second reason read in the worker: with the whole word at
  0 the runtime zeroes Local Tone and Local Structure before they reach the network, whatever the
  sliders say.
- **`DepthNormalise` shipped on** while the overlay painted it amber for being past what had been
  measured, and its own help text said it reads as the wrong operation: PCSX2's depth already has
  its bulk at the top of its own tiny range (probe: mean 0.00197 against max 0.00200), so scaling
  by 1/max lands nearly every pixel at 0.99 rather than spreading anything out. It ships off.

### The overlay, rebuilt

It carried 47 controls across eight headers. It carries 15, and it is built for a **narrow panel**,
because that is how it is used -- kept thin so the game stays visible behind it.

- **Nothing was removed, only taken off screen.** Every atomic, every load and every save is
  untouched: a hidden control still reads and writes its own key in `amd-nr.ini`. **More settings**
  at the bottom opens a cascade with a checkbox per hidden control, grouped under the header it will
  appear beneath, so turning one on tells you where to look for it.
- **Status moved to the right-hand column**, opposite the switches, in vertical space those rows
  already occupied. Left is what you change, right is what happened. It used to be a collapsing
  section that spent a header and a click on five lines of text.
- **A colour rule, and it is a report rather than a threshold.** The old panel painted Timing, Scale
  and Passes amber the moment Scale passed 0.50 or Passes passed 1 -- which is most of a working
  configuration, and amber that is on while everything is fine is amber nobody reads. Red is now the
  documented device-removal path and nothing else; amber comes from the measured skip rate, from the
  card's own cap having fired, or from a switch seen to break the picture. Section headers each
  carry their own hue so a thin panel reads as regions.
- **The MEASURED / TRACED / UNKNOWN / INERT tags are gone.** They were provenance, which belongs in
  the source and in the handoffs, and they cost eight to twelve characters on every row.
- **Export logs to desktop.** One button copies `amd-nr.log`, the runtime's `dlssnr_on_amd.log`,
  `ReShade.log` and `amd-nr.ini` into a dated folder on the desktop. The settings go with the logs
  because a log without them cannot be compared against anything.
- Labels are short because ImGui neither wraps nor clips a checkbox label -- it runs off the right
  edge. What a control means lives in its `(?)`, which has room.
- Tooltips are one or two sentences. Performance sits above Image.

### Checks

`compose_check.py` had never looked at Edge Fade, which is the honest reason nobody could tell
whether it was working. It now proves the correction reaches zero at the border, that a corner fades
harder than an edge because it sits in two bands, that halfway into the band is half the correction,
and that the whole triple is scaled so hue cannot move -- and writing it caught a wrong claim about
what the maximum value does.

`style_check.py` checks the B and C coefficients against the descriptor table, that every knob is
the identity at neutral, and that the closed-form saturation agrees with a real HSL round trip.
`feed_fx_check.py` proves the companion effect and the add-on still agree on every texture name,
which is what made the rename safe to do mechanically.


## v0.6.0 - 2026-09-19 - OpenGL

Same pinned **DLSS-NR-on-AMD v0.3.0** runtime and the same weights, so an upgrade is the add-on
and nothing else.

- **An OpenGL route.** The network runs where it always runs -- on this add-on's own private D3D12
  device -- and the frame crosses to it the way the Vulkan route's does: the shared textures are
  created on our device, exported as NT handles and imported into the host, here through
  `GL_EXT_memory_object_win32`. What is different is the host's half, and all of it follows from
  one measurement: ReShade hands an OpenGL add-on the **default framebuffer**, not a texture, so
  every copy in the route is a `glBlitFramebuffer` rather than a resource copy, Y is inverted on
  the way in and back on the way out, and the route puts back every piece of context state it
  touches. Validated end to end on Luanti 5.17.0 with the network running: colour and estimated
  motion, no depth, same as Vulkan.
- **The hand-over runs on the GPU.** The imported D3D12 fences are used as GL semaphores, which
  makes this the only route in the project that does not stall the CPU twice a frame -- worth
  about 13% here against the same run with `GlSemaphores=0`, which forces the old behaviour.
  The first frame is still confirmed on the CPU before the stall is dropped: a fence wait that
  never completes is a hung queue and a game that has to be killed, not a log line.
- **Every frame that reaches the screen has been through the network.** When the previous
  evaluation has not finished the route waits for it rather than letting the frame past
  uncorrected, because alternating between a corrected frame and an uncorrected one at a hundred
  and eighty a second is a strobe. `GlHoldFrames=N` repeats the last result instead, which raises
  the present rate and fills it with duplicates -- off by default, since a duplicated frame makes
  every frame counter in the system report a rate nobody is seeing.
- **A multisampled default framebuffer is resolved on the way in**, read from `GL_SAMPLES` rather
  than from the back buffer's description.
- **Two rules this driver adds, both paid for.** An imported texture's shared handle is not the
  application's to close, whatever the extension says -- closing it faults inside the ICD from a
  driver thread, measured at 3 runs in 8 -- so the route keeps them for the life of the process.
  And `GL_HANDLE_TYPE_OPAQUE_WIN32_EXT` is *accepted* for a D3D12 resource handle and is wrong, so
  the route names `D3D12_RESOURCE` and stands down rather than falling back to something that
  appears to work.
- **Re-entrancy, which used to close the game.** In OpenGL the route's own calls go through the
  same ReShade hooks the game's do, so its `glBindFramebuffer` came back as a render-target bind on
  the present thread, inside a lock this add-on already held -- and a second `std::mutex` lock on
  one thread throws under MSVC rather than deadlocking. The observers now ignore what the add-on
  issued itself, which they should have done anyway: the route's binds are not the game drawing.
- Two new diagnostics, neither needing a game: **`glprobe`** answers whether this driver will let
  OpenGL import D3D12 memory and fences at all, and **`glinfo`** reports what ReShade hands an
  add-on inside a real OpenGL host. `docs/opengl-route.md` is the whole of what was measured.

Not in this release: depth or motion from the game on OpenGL (the depth is reachable as a texture
and needs a shader pass to become usable), and any route at all for a 32-bit OpenGL game -- the
32-bit pair covers D3D8, D3D9 and D3D11.

## v0.5.3 - 2026-09-17 - Pipelined presentation for the 32-bit bridge

Same pinned **DLSS-NR-on-AMD v0.3.0** runtime and the same weights as v0.5.2, so an upgrade is the
32-bit pair and nothing else. The 64-bit add-on's behaviour is unchanged by this release.

- **The bridge posts the frame and composes the previous present's answer**, instead of sitting in
  the IPC round trip waiting for this one. The helper works while the game builds its next frame.
  Measured as an A/B on that flag alone, with the ini compared before and after so nothing else
  differed: GTA IV **43.8 -> 61.6 FPS (+41%)** on classic D3D9 staging, Resident Evil 5 **46.6 ->
  62.2 FPS (+33%)** by the game's own benchmark, Half-Life 2 **77.0 -> 89.3 FPS (+16%)** on the
  D3D9Ex shared path. No dropped frames and no faults in any of them.
- **It is the default on the 32-bit bridge**, as `Async=1` under `[dlss5]` in `amd-nr.ini`.
  `Async=0` restores same-frame presentation exactly -- the same code path, minus two clock reads.
  The startup line reports `present=pipelined` or `present=same-frame` so a log says which it was.
  Note the key means something else on the 64-bit add-on, where `Async` is the older inline
  composition and same-frame is still the tested path.
- **The cost is one frame of latency and nothing else.** It does not smear: `DownloadD3D9Frame`
  replaces the back buffer whole, so what reaches the screen is frame N-1 finished and
  self-consistent rather than a mix of two. Three games were checked in both modes with no
  difference seen. What pipelining removes is `min(game, network)`, less whatever the game's own
  GPU work already overlapped, and all three land between 66% and 97% of that ceiling.
- **The Timing control switches the mode while the game runs.** It used to be disabled on this
  route, with `Async` changeable only by editing the ini before launch. It now reads and writes the
  frontend's own flag, so the panel cannot drift from what the bridge is doing, and nothing has to
  reach the helper because `Async` never crosses the protocol. Switching costs at most one frame in
  either direction: turning it off collects the answer in flight and drops it, turning it on leaves
  the first present with no previous answer to compose. The choice is written one key at a time,
  because rewriting the whole ini from the frontend would drop everything the helper owns.
- **Arm the stage probe from the ini with `Timing=1`.** `AMDNR_X86BRIDGE_TIMING=1` still works
  where it already worked, but a game that re-launches itself through its own launcher, GTA IV
  among them, loads the add-on into a process that inherits no environment from whoever started it
  and reported `probe=off` however you launched it. A timing window that spans a mode switch is
  discarded rather than averaged, and both probe lines now name the mode and the effect state they
  were measured in.
- Subscribe `destroy_device` and settle the `IDirect3DDevice9` reference-count warning ReShade
  prints at exit. It is cosmetic and no add-on can prevent it: measured with an unconditional log
  line at the top of the handler, ReShade never delivers the event when a game leaves through
  `ExitProcess`, and it prints the warning about two seconds before it unloads the add-on. The
  handler stays because it is correct for a game that does shut its renderer down.
- **Promoting a classic D3D9 device to D3D9Ex was measured and dropped**, with
  `tools/d3d9ex-probe.cpp` on this hardware. A plain D3D9 device refuses `CreateTexture` with a
  shared handle, so the fast path is unreachable without promotion; a D3D9Ex device refuses
  `D3DPOOL_MANAGED`, which is what a legacy D3D9 or translated D3D8 game creates nearly all of its
  textures in. Promotion therefore means a per-resource translation layer -- what dgVoodoo and DXVK
  are, and this project removed its dgVoodoo dependency deliberately. The classic path's fixed
  transport cost stands: roughly 5.5 to 6 ms a frame at 1920x1080, which no Resolution Scale
  setting reaches.

## v0.5.2 - 2026-09-15 - The overlay saves itself

Same pinned **DLSS-NR-on-AMD v0.3.0** runtime and the same weights as v0.5.1, so an upgrade is the
three add-on files and nothing else — the 141 MB does not move.

- **The overlay saves itself.** Every control now writes to `amd-nr.ini` the moment you let
  go of it, on both the 64-bit route and the 32-bit bridge. Save Settings stays — it is still what
  puts a line in the log saying a write happened — but nothing is lost to closing a game without
  having scrolled down to it, which is where half a dozen A/B tests went. The write is armed when a
  control settles rather than while it is being dragged: `WritePrivateProfileString` rewrites the
  whole file once per key, so a held slider would otherwise be fifty full rewrites a second.
  Factory Defaults now lands in the ini with everything else instead of asking for a Save after it.
- Keep every ini key in one list, which the writer and the change check both read. Two lists would
  drift, and a setting in one but not the other is a control that quietly stops being saved.

## v0.5.1 - 2026-09-15 - The hotkey, depth, and not taking the driver down

Requires the pinned **DLSS-NR-on-AMD v0.3.0** runtime; v0.2.14 and v0.2.17 are both refused by
hash. The weights are unchanged, so an upgrade replaces a 7 MB DLL and downloads nothing else.

- Move to **DLSS-NR-on-AMD v0.3.0**. Every offset this add-on writes into was re-derived against
  it; see [below](#the-runtime-moved-to-v030).
- Keep every one of those offsets in `src/neural/runtime_offsets.h` and nowhere else. They used to
  be copied into four files, the move to v0.3.0 updated two, and the 32-bit bridge's host then died
  with an access violation on its first evaluation in every game — the v0.2.17 job counter lands in
  `.rdata` on v0.3.0, and the interlocked write there is a write into read-only memory. The Vulkan
  route carried the same crash, and a stale entry point beside it that would have been called
  rather than faulted.
- Copy the depth guide as a depth-stencil on the 32-bit bridge as well. The 64-bit path got that
  fix earlier in this release and the bridge kept the broken half, so a 32-bit D3D11 game with a
  planar depth format was still feeding the network garbage.
- Read the settings once on the run that writes the ini, rather than parsing it twice and printing
  the same two lines twice.
- Write `Async=0` rather than `Inline=1` into a new `dlssnr_on_amd.ini`. v0.3.0 renamed that key
  and inverted it. An ini written by an older release still comes out inline, because the unknown
  key is ignored and the new one defaults to inline.
- Fix the toggle hotkey rebind, which could not work for three reasons at once: ReShade answers 0
  for every key through `GetAsyncKeyState` while its overlay holds the keyboard, the captured key
  was written to a copy nothing else reads, and a capture armed on the click frame took the click's
  own key. Capture now reads `effect_runtime::is_key_down`, waits for release, and writes through
  the shadow the panel and the host both read.
- Strip a UTF-8 byte-order mark from `amd-nr.ini` at load. `GetPrivateProfileInt` reads the
  file as bytes, so a BOM hides the whole file and every setting silently falls back to its default.
- Carry an `X8R8G8B8` back buffer as its `A8` twin on the D3D9 CPU route. `B8G8R8X8_UNORM` has no
  typed UAV store on this hardware, so there was no way to write the corrected image back and the
  add-on stopped on every frame from the first.
- Fill the depth guide from three presents of binds when nothing is chosen yet, instead of asking
  one candidate to win three presents running. An engine that rotates two or three depth targets
  never does, so the slot stayed empty for the whole run.
- Rewrite the D3D12 depth pick: tally binds and clears per present, only consider buffers shaped
  like the swapchain, and prefer the ones the game clears. It used to take the largest, which is
  a shadow map.
- Create the depth snapshot with `ALLOW_DEPTH_STENCIL`, on both the D3D12 and the D3D11 paths. A
  depth-stencil surface is planar; the same format without the flag is not, and the copy between
  them returns garbage that D3D12 does not refuse.
- Judge the depth probe on how much of the reading is in 0..1 rather than on the minimum differing
  from the maximum, which `min 0, max 4.4e30, mean NaN` passes. Junk no longer ends the search, and
  no longer keeps it going for ever either.
- Hold the resolution scale one step lower after three network evaluations over 250 ms. In inline
  mode the game's queue waits for the network, and a multi-second dispatch is a Windows TDR: the
  driver resets and takes the game with it. The person's own Scale setting is untouched.
- Refuse a guide buffer below 256 pixels on a side. With the swapchain size still unknown every
  buffer passed the floor, including a 1x1 that was taken as the motion guide.
- Write every setting into `amd-nr.ini` on the first run, at its default, so the add-on can
  be tuned from the file alone with the overlay never opened.
- Retire the Rust terminal installer. Installing is AMD-NR ReShade Installer, which finds games,
  fetches and verifies the payloads, installs ReShade, and keeps a manifest of what it wrote.

### The runtime moved to v0.3.0

`.hip_fat` is the same size to within 24 bytes and `dlssnr_on_amd_weights.bin` is byte for byte
the file v0.2.17 used, so the network did not change. `.text` grew 28 KB and `.data` 1 KB, and
**every offset this add-on writes into moved.** All of them were re-derived against the new
binary and none carried over on faith. The deltas are not one constant: the device pointer block
moved by 0xA080, the inline block by 0xA0E0, the job block by 0xA158 and the option struct by
0xA160, because v0.3.0 inserts new globals between them.

The method was the same one that worked for v0.2.17, and it is worth writing down because it is
not guesswork. The runtime reads its own settings with `GetPrivateProfileIntA`, so decompiling
that one function names fourteen of the globals outright — the key string is beside the address
it writes. The rest came from three functions matched by structure across the two builds: the
notify entry, identical line for line and the same 0x21C bytes long; the record entry, whose
first three tests are the same three bytes in the same order; and the weights loader, the same
0x9A6 bytes with the same `DLSSNR_NO_REPACK` string and the same single call site.

One correction to how this was read last time. Hex-Rays renders `HIDWORD(xmmword_X)` for what is
sometimes `+4` and sometimes `+0xC`, so the pseudocode alone puts `HipDevice` in the wrong place.
The disassembly is the ground truth and every address here was taken from it.

| | v0.2.17 | v0.3.0 |
|---|---|---|
| notify | 0x9170 | 0x9460 |
| record | 0xf600 | 0x12640 |
| weights loader | 0x19240 | 0x1fe80 |
| watchdog job counters | 0x8d808 / 0x8d80c | 0x97950 / 0x97954 |
| option struct (Enabled) | 0x8d9bc | 0x97b1c |
| effective HIP device | 0x8dad0 | 0x97c30 |

The three binary patches moved too — `0x6006` → `0x60a6`, `0x8583` → `0x8873`, `0x6e3db` →
`0x76c0e` — and are otherwise the same three: kill the runtime's own setup thread, remove the
doubled `ExecuteCommandLists`, and correct one log string. `tools/patch_runtime.py` refuses a file
whose hash is not the one in `tools/runtime-patches.json`, so a stale pairing cannot be applied by
accident.

**What v0.3.0 brings that reaches this route.** The runtime's GPU wait is no longer one long spin
dispatch: it runs as predicated slices that are preemptible between them, tuned by the new
`PredWait` and `PredSlice` keys. That is inside the record function, which is the function this
add-on calls, so it applies here — and it is the right shape for the problem the scale cap in this
same release works around, because a preemptible wait is far less likely to trip a TDR. Also
inside the two functions this add-on drives: a per-stage breakdown when a job spikes, which says
whether the GPU ran slowly throughout or was taken away for one stage, and a check that refuses
to apply a residual into an output format with no UAV, falling back to frame replacement instead
of writing nothing.

**What does not.** v0.3.0's pre-upscale mode, its FFX/FSR upscaler detection, its frame-generation
awareness and its DRED page-fault reporting all live behind the detours the first patch removes.
This add-on drives the runtime itself and cannot have both, so none of those four are available on
this route. The packet field that opts into pre-upscale is left zero, which is the default and the
only correct value here.

### The two routes that were still on v0.2.17

The port above was verified against both binaries, compiled clean, passed the whole suite and two
code reviews, and was still wrong in two of the four files that hold these offsets. `neural.cpp`
and `framecheck.cpp` were re-derived. `host64.cpp` and `vk_route.inc` were not, because the sweep
that found them searched a partial list of patterns and never matched `.inc` at all. It reported
clean.

What that cost: `0x8d6f4` is the v0.2.17 job counter, and on v0.3.0 that address is inside
`.rdata`. The access is `lock cmpxchg`, a write, and `.rdata` is mapped read-only — so the 32-bit
bridge's 64-bit host took `0xc0000005` on its first evaluation, in every game, deterministically.
The frontend saw it as `ERROR_BROKEN_PIPE`, restarted the host, and watched it die again. The
Vulkan route had the same line and had simply not been run. Beside each sat a stale `0x9170`,
which is worse in the way that matters: on v0.3.0 that RVA is still inside `.text`, in the middle
of an unrelated function, so it would have been **called** rather than faulted.

The fix is not the four lines. `src/neural/runtime_offsets.h` now holds every address, named, and
the four files use the names — all of them compile into one translation unit, so one header
reaches all four. `tools/runtime_offsets_check.py` reads that header and, alongside checking every
address against the runtime's own sections and decoding the record entry's opening tests out of
the instruction stream, **fails when any source file writes an offset of its own**:

```
FAIL no source file writes an offset of its own (1 found)
     src/x86bridge/host64.cpp:156 writes 0x8d6f4 -- name it in runtime_offsets.h instead
```

Compiled, statically verified against both binaries, and now run in a game: Need for Speed 2015
reads the engine's own defaults back at the new addresses (`LocalStructure` 1.000,
`SkinStructure` -1.000, `Scale` 0.031), 4800 jobs at about 10 ms each, a non-zero residual, and
the runtime's new preemptible wait active. The 32-bit bridge came up on Bully after the fix. **The
Vulkan route is fixed and has not been run.**

## v0.5.0 — 2026-09-14 — The 32-bit bridge, D3D8, and one installer

- Add native 32-bit D3D9 and D3D11 ReShade frontends and a separate 64-bit host that reuses the
  existing neural engine and HIP runtime on the game's exact adapter.
- Bridge D3D9 frames through private D3D9/D3D11 staging resources: shared GPU textures on D3D9Ex
  and a CPU-compatible upload/readback fallback on classic D3D9. This removes the dgVoodoo
  dependency and its third-party binary/security-detection problems.
- Define a fixed-width protocol with process identity, generation, frame and settings-revision
  validation across the x86/x64 boundary.
- Bound frontend IPC and helper startup waits so an unresponsive helper falls back instead of
  freezing the game's Present thread indefinitely.
- Apply Resolution Scale only after slider editing ends, avoiding repeated network-raster staging
  and unnecessary VRAM growth while dragging.
- Stop the x86 installer from executing an unverified ReShade Setup sidecar. ReShade remains a
  manual prerequisite or a separately supplied, hash-pinned payload.
- Replace the stale checkout-hash baseline with builds of the current integrated source tree.
- Add Windows x86/x64 build, protocol, PE/import and native named-pipe timeout checks to CI, plus
  portable protocol/control tests on Linux.
- Honor ReShade's `[INSTALL] BasePath` when it safely points inside the selected game directory,
  including Source-engine layouts that load the D3D9 proxy from `bin`.
- Keep the x86 D3D9 frontend alive across `IDirect3DDevice9::Reset`. ReShade emits
  `destroy_swapchain` from inside the driver's reset, where submitting an event query, waiting on
  either private GPU device or running IPC re-enters `amdxx32.dll` and crashed GTA IV on
  exclusive-fullscreen Alt+Tab. That branch now only releases the D3D9 default-pool resources and
  defers remote-generation retirement to the next stable presentation.
- Return HRESULTs from the x86 D3D9 staging operations. A transient `D3DERR_DEVICELOST` or
  `D3DERR_DEVICENOTRESET` is treated as an interrupted reset frame that keeps the host connected
  and resets history on recovery, instead of permanently faulting the bridge.
- Add an experimental D3D8 preset through the official, hash-pinned d3d8to9 compatibility layer.
  It translates D3D8 to the native D3D9 frontend and does not restore the old dgVoodoo route.
- Merge the two installers into one. `installer-x86` was a separate C++ tool with its own Win32
  GUI covering only the 32-bit routes; its transactional model -- install manifest, backups, journal
  with rollback, ownership tracking, safe path handling and ReShade `[INSTALL] BasePath` -- was
  ported into the Rust installer, and both routes now run on it. The x64 route gains backups and a
  manifest it never had, so an install can be undone precisely instead of by deleting known
  filenames, and installs made before this keep working through a name-sweep fallback.
- Detect the target's architecture instead of asking. The installer reads the PE header, names the
  executable it read, and offers only the presets that exist for that width -- five of the ten
  API-by-architecture combinations do not. A folder holding both widths is reported rather than
  guessed at, and PCSX2 and RPCS3 stay named targets with their own guidance.
- Turn the preflight into a gate. It used to fill a panel and stop there, so an install into a
  read-only folder, or over a file the game still had open, went ahead and failed partway through a
  copy. Those checks now run before the journal exists, for both routes, and name the cause.
- Accept either shape of folder in the first field, and either a folder or an executable as the
  target.
- Add an opt-in x86 stage probe behind `AMDNR_X86BRIDGE_TIMING=1`, off by default. It splits the
  bridge into `input+prepare`, `host` and `output` and averages one line per 120 completed frames,
  naming the staging path measured. On classic D3D9 the input and output stages are a full frame
  crossing CPU-visible memory each way, so their sum is roughly fixed and does not shrink with
  Resolution Scale; only `host` does. The probe issues no query, flush or wait of its own and reads
  only boundaries the frame already crosses, keeping it out of the `IDirect3DDevice9::Reset`
  window. It exists so the classic-D3D9 cost is separated before any behaviour is changed.

## v0.4.2 — Native Vulkan compatibility and lifecycle stability

Tagged and published on 2026-09-12 without a section here, which is why this one is written from
its commits rather than from notes taken at the time: native Vulkan compatibility and add-on
lifecycle stability, resolution-scaling stability, and a MinHook linkage fix in the `framecheck`
build. The lifecycle work spans the v0.4.1 entry below, so read the two together rather than
assuming the boundary is clean.

## v0.4.1 — Native Vulkan game stability

This release hardens the experimental Vulkan route for native games, validated on Detroit: Become
Human and DOOM Eternal with ReShade 6.8.0.2155 and an AMD Radeon RX 9070 XT.

- Link the add-on with the static MSVC runtime (`/MT`). Detroit ships older `msvcp140.dll` and
  `vcruntime140.dll` files beside its executable; a `/MD` build registered with ReShade but failed
  during `DllMain`. The static build has no dependency on those private runtime copies.
- Validate the Vulkan present queue and immediate command list before use. An unsupported async
  queue now skips safely instead of dereferencing a null command-list pointer.
- Preserve both `DISABLE_VK_LAYER_reshade_1` and `_2` while creating the private Vulkan discovery
  instance. This supports the renamed kill switch needed by DOOM's launcher without recursively
  entering ReShade.
- Create the private D3D12 crossing texture in `COPY_DEST`, matching its first operation.
- Recreate a D3D12 allocator/command-list pair after `Reset` or `Close` failure so one bad recording
  cannot poison a work slot for the remainder of the process.
- Fully retire swapchain-sized Vulkan state: wait for the host device, release imported images,
  invalidate dimensions and readiness, and rebuild all crossings even when the new swapchain has
  the same size and format.
- Strengthen the Vulkan fast path so it also requires live imported images and D3D12 resources.

Live validation after the fixes:

| Host | Frames | Swapchain/toggle result | Skips | Result |
|---|---:|---|---:|---|
| Detroit: Become Human | 9,240 | repeated toggles and two full rebuilds | 0 | stable |
| DOOM Eternal | 3,600 | repeated alt-tabs and about ten full rebuilds | 1 | stable |

Both runs reported a non-zero, detail-correlated residual. Neither log contains command-list
`Close`/`Reset` failures, device loss or a bridge stand-down after the fixes. Vulkan depth remains
an explicit future task: the current route feeds colour and estimated motion.

## v0.4.0 — Vulkan, D3D12, and a trail that was not the network's fault

Requires the pinned **v0.2.17** runtime; v0.2.14 is refused. `dlssnr_amd_pass2.dll` and
`pass3.dll` are unused and can be deleted.

### One package

The separate Vulkan build is gone. The Vulkan transport is compiled into the single add-on and
does nothing on a game that does not import Vulkan — it patches one import-table entry, and
there is no entry to patch. The same binary was run on D3D11 (NFS 2015), D3D12 (GTA V Enhanced)
and Vulkan (RPCS3).

### Tested live, on all three routes

The previous preview shipped with its serialized multipass path validated only on saved frames.
It has now been run in games.

| Route | Host | Frames | Result |
|---|---|---|---|
| D3D11 | NFS 2015 | 1,205 | one skip, no failures |
| D3D12 | GTA V Enhanced | 23,663 | no failures; found the trail below |
| Vulkan | RPCS3 | 1,879 | bridge up, full round trip, no failures |

### A stale correction was being pasted onto moving frames

On a frame where the network is skipped — its previous evaluation is still on the GPU — compose
ran anyway, and the residual it read belonged to an older picture. On a slow scene nobody sees
it. GTA V Enhanced skipped **37% of frames** (13,921 of 37,584), because the network costs about
29 ms at full Resolution Scale and the game presents faster, and at speed those frames read as a
heavy trail behind everything.

A skipped frame now goes out as the game drew it, on all three routes. The gate had to be applied
to the final copy back to the game's image, not only to the intermediate one: skipping just the
intermediate copy would have sent back the previous composition whole, which is a held frame and
worse than the trail. The measurement that says dropping the correction is right rather than a
trade: its own mean in that scene is 0.003.

No prior test could have caught this. NFS 2015 skipped 1 frame in 1,205.

### Vulkan works on RPCS3 and not on PCSX2, structurally

The route needs a host that imports `vkCreateDevice` statically by name, because the interop
extensions have to be added while the VkDevice is created and cannot be added afterwards. RPCS3
has `vulkan-1.dll` in its import table. PCSX2 has zero occurrences of it and resolves through
`vkGetInstanceProcAddr`, so there is nothing to patch. It stands down, says so in the log, and
leaves the picture alone.

On Vulkan the network is fed colour and estimated motion and nothing else. There is no depth
path: both depth sources are behind D3D11 and D3D12 checks. Across 5,239 frames on RPCS3 not one
depth-stencil bind reached the add-on, which may be the emulator or may be ReShade's Vulkan
reporting; neither was separated and neither changes the result.

### Starting up, and the hotkey

- **StartOn** is a normal setting now, in the panel and written back. It was an ini line marked
  diagnostic-only that was deliberately never saved.
- **ToggleKey / ToggleMods** — `Ctrl+End` is only the default. The panel captures a real keypress
  and writes both; key names come from Windows, so a non-US layout reads correctly.
- **DisableOnAltTab** — switches the effect off when the game stops being the focused window, and
  leaves it off until the hotkey brings it back. Separate from the minimised-window handling,
  which is unconditional and does resume on its own.
- The add-on writes a **commented `amd-nr.ini`** when none exists. The panel saves through
  `WritePrivateProfileString`, which cannot carry a comment, so an install that had only ever
  been saved from the panel was a bare list of keys.

### Experimental tab

New, and everything in it is off by default. **Network Output** shows the network's answer
instead of composing it, which bypasses Highlight Guard, Colour Strength and both residual
limits. It is a proof of concept: preferred by eye on one game, on one route, and not measured
against the composition anywhere else.

### FSR: closed, with numbers

Running the network below full Resolution Scale costs 30–57% of its whole effect, but the bicubic
upsample accounts for 0.1%, 0.7% and −2.8% of that across three cases. The rest is the network
answering differently at a different scale: the half-resolution correction differs from the
full-resolution one by 31–61% of its own magnitude, 62–90% of that at low frequency, correlating
0.84–0.95. The receptive field is fixed in pixels, so at half resolution it covers twice the
scene. No upsampler recovers a correction that was never produced, and a global gain-and-offset
fit removes 2.9%, −0.7% and 0.2%, so it is not a fixed bias either.

## Preview 2026-09-10 — SDR and serialized inline passes

- Update both normal and Vulkan previews to the pinned v0.2.17 runtime.
- Treat Encoding=0 as SDR when resolving automatic tonemapping, including FP16 transport.
- Remove an invalid pointer write at 0x8d808: this location contains watchdog job counters.
  The write crashed the isolated full-resolution run after its first timeout.
- Submit and finish intermediate inline passes before changing shared runtime parameters.
  Wait for both the GPU queue and HIP worker. Async retains the legacy batch path.
- Add optional matched input/runtime captures and an isolated GPU framecheck executable.
- Verify per-pass Tone survives serial submission and reproduces exactly across processes.
- Refresh scale/pass documentation and distinguish the normal and Vulkan build configurations.
- FSR 2/3/4 integration remains research; no upscaler was added in this preview.

Validation and remaining limitations: [preview notes](docs/preview-2026-09-10.md).
Earlier entries below describe previous investigations, including hypotheses superseded by this run.

## Earlier unreleased work

### Pass Count, and why it was worthless

Raising Pass Count did not add detail, it added saturation, and at 3 it wrecked the picture. The
count was never the problem — **the composition was**.

The network's answer came back as a *difference*, added to the frame channel by channel and then
clipped per channel. Two passes is twice that difference, three is three times it, and a clipped
channel is a hue rotation, not a brighter pixel. So every extra pass bought more colour error and
less picture. The chain itself was already right: each pass reads what the last one wrote, and the
correction is measured against the picture the network was first shown, so compose receives the
whole chain's work rather than the last pass's difference from the one before it.

Replaced with the ratio composition the OptiScaler DLSS-NR fork uses, and RenoDX's DLSS 5 addon
before it. The answer is made into a complete picture of its own, its luminance is compared against
the frame's as a bounded ratio, and two finished pictures are blended. A bounded ratio cannot move
hue.

- **Composition** (Image) — *Ratio (bounded)* or *Additive (old)*. Ratio is the default; the old
  path is kept so both can be seen in one session.
- **Colour Strength** (Image) — whether the network's colour arrives with its light. At **0** every
  pixel keeps the game's exact hue and only its brightness carries the network's verdict. This is
  the control for "it changed the colours": at 0 it cannot, by construction.
- **Highlight Guard** (Image) — the most a pixel's luminance may move, in either direction. One
  scalar over the whole triple, so it bounds brightness without touching hue. 2.0x by default.
- **Guard follows Pass Count** — one extra multiple of headroom per extra pass. The guard bounds
  the finished composition while the passes compound the ratio inside it, so a fixed guard means
  the third pass spends most of its contribution against the clamp and costs frametime for nothing.
- **Per pass** (Image, under Pass Count) — Structure, Local Tone and Skin per run of the network.
  Pass 2 is editing pass 1's work, so the same numbers again ask it to sharpen its own sharpening.
  Off by default, in which case every pass gets the globals exactly as before.
- A correction that leaves the displayable range is now scaled as a whole triple instead of clipped
  per channel, and a near-black pixel takes a damped edit instead of an unbounded ratio — which is
  the crawling, boiling colour in dark scenes.
- `tools/compose_check.py` asserts the four properties the composition is built to have.

### The blown blocks, measured

A two-pass run on God of War reported a mean correction of **0.072 with a maximum of 4.16**, in a
picture whose own mean is 0.13. A correction four times brighter than white is not something the
network saw — it is a tile where it extrapolated, and every extra pass runs on top of that blown
tile. One pass measures 0.021, two measures 0.072: not twice, three and a half times.

- **Residual Limit now defaults to 0.25** and is a fraction of white rather than a raw linear
  value, so it means the same thing at every encoding. It was off by default, which let all of the
  above through.
- It **scales the whole correction** instead of clamping each channel. A per-channel clamp on an
  outlier is a hue rotation, which is what turned a blown block into a blown *coloured* block.

### Local Tone is a first-pass control again

`i == 0 ? tone : 0.0f` was removed last release as an asymmetry nobody had chosen. Somebody had:
upstream's `PassProfiles.h` makes exactly that choice, in one line —
`pass == 0 ? cfg.DlssNrLocalTone.value_or_default() : 0.0f`. Local tone is a tone decision about the
frame, and a second pass re-deciding the tone of a frame whose tone the first pass already moved is
how a chain runs away from the picture it started with. Restored. Structure and Skin still go to
every pass at full value, which is also what upstream does.

**Taper later passes** (off by default) halves Structure per pass on top of that. It is ours, not
upstream's, and nothing here has measured that it is the right answer — so it is offered and not
taken.

## v0.3.0 — 2026-09-10

Everything below is new since what is currently published. The short version: **D3D11 works, and
it is now the better path**, because it is the only one where the game's own depth and motion
vectors are reachable. Plus a pile of fixes, two of which were changing the picture without
anyone choosing it.

**If you are upgrading:** the add-on now **starts switched off** every run — turn it on in the
overlay or with `Ctrl+End`. And `dlssnr_amd_pass2.dll` through `pass10.dll` are no longer used;
delete them.

### D3D11

- **The add-on runs on Direct3D 11 games.** It builds its own D3D12 device on the same physical
  adapter as the game and carries textures between the two through shared resources and fences.
  Verified same-adapter, and measured to produce a residual identical to the D3D12 path within
  run-to-run variation, so the transport is not eating anything.
- **The game's own depth and motion vectors are fed to the network.** It counts which
  depth-stencil and which two-channel float render target the game binds most often, takes one
  copy of each per frame, and carries them over. Depth is converted to `R32_FLOAT` on the game's
  side first, because typeless depth formats cannot be shared between devices.
- **Need for Speed 2015** added as a target.
- **A guide probe.** A buffer can be handed over, bound and fed and still be a cleared constant,
  which looks identical from outside and is worth nothing. The overlay now reports what the
  guides actually *contain* — depth range, and what percentage of motion blocks are still.

### The overlay

- **Rewritten.** Descriptions moved into `(?)` tooltips, controls grouped, and the panel is a
  panel again instead of a wall of grey text. The tooltips carry the measured results, so they
  are worth reading once.
- **Risk colouring with a legend.** Red means the value the control is holding *right now* can
  take the display driver down; amber means past what has been measured. It tracks the value, so
  turning it back down clears it.
- **Save Settings / Reload Settings.** This did not exist: everything changed in the overlay was
  lost on exit, so every A/B test meant editing the ini by hand between runs.
- **English and Brazilian Portuguese**, switchable in the panel, tooltips included.
- **Timing** is now a named choice — *Same frame (inline)* or *Async (previous frame)* — instead
  of a checkbox called "Apply On Same Frame".
- **Starts switched off.** The add-on rewrites every presented frame and the settings that do
  that are the ones that have taken machines down. Not persisted, on purpose.
- **Removed ten controls that did nothing**: Options Mode, Hook Method, Require DLSS, UI
  Correction, Global Tone Strength, Model, Jitter, Exposure, Upscaling Ratio, and the old
  greyed-out Character Mask. They were disabled mirrors of the NVIDIA panel with one reachable
  value each.
- **Every control now says how well it is known.** A tag beside each one — `MEASURED`,
  `TRACED`, `UNKNOWN` or `INERT` — with a legend at the bottom of the panel. "Which of these
  actually does something in the game" previously had no answer short of reading the source, and
  the honest answer is not the same for any two controls.
- **Residual Limit** and **Edge Fade**, both off by default. The network works in tiles, and the
  tiles at the frame border have no neighbour on one side, so the correction there is
  extrapolated rather than seen; bicubic upsampling then rings on top of it. That is the specks
  and crawling colour in the corners. Limit caps how far one pixel of correction may go; Edge
  Fade rolls the correction off over a border band, and a corner sits inside two bands at once,
  so it gets both.
- **Character Mask turns red when off, with a warning.** Switching it off does not just disable
  the skin term — it removes the effect from the whole frame, because the engine derives its
  structure and tone parameters through that mask.
- **Engine Scale has a "Reset to 1/32" button.** A five-decimal slider cannot be dragged back
  onto exactly `0.03125`, and this is a field whose effect nobody has established, so leaving it
  a hair off its default is a way to change the picture and never find out why.

### New controls, from mapping the runtime

The runtime's option struct was mapped by decompiling its own ini reader rather than guessed.
That turned up fields nothing had ever written:

- **Character Mask** (`UseAutoMask`). The same control the NVIDIA add-on exposes. It defaults to
  1 and was never written, so it has always been on by default. It is the candidate explanation
  for the 1.5% that Skin Structure Strength measures.
- **Engine Scale** (`Scale`, default `0.03125` = 1/32). This is the `0.031` that earlier notes
  recorded as "a live value we never wrote". Identified. Unrelated to Resolution Scale.
- **Tone Channels** (`ToneChannels`). Effect unknown; exposed so it can be A/B'd.
- **Tonemap** and **Temporal** are now explicit instead of hardcoded.

### Fixed

- **Diffuse White ran backwards against its own label.** It computed `203 / white` — the
  reciprocal — and used 203 for both encodings, so raising the slider *dimmed* what the network
  saw. On Linear at 100 nits, which is the documented automatic for BT.709 and should give a
  scale of exactly 1.0, it multiplied the whole linear image by **2.03**. Now `white /
  reference`, with 100 for Linear and 203 for scRGB-nl. **This changes the picture.**
- **Depth Inverted defaulted off while both runtimes default it on.** The add-on inverted the
  engine's own default on every run. It is a real field — read from ten places — which earlier
  notes had marked as unverified. It is now written as 1 unconditionally; see below.
- **Tonemap was forced to 0 at init.** The engine's own default is -1. Nobody chose 0 and it was
  never measured against anything.
- **`0x76e1d` was mislabelled.** It is `Temporal`, not "the motion field is valid" — the
  engine's ini reader reads the key `Temporal` into that byte. This explains a measurement that
  had been sitting unexplained: `Temporal=1` was the only run where the engine reported non-zero
  motion.
- **Pass Count no longer takes the machine down.** It used to load a separate copy of the runtime
  per pass, so two passes meant two full engine bring-ups — 147 MB of weights plus activation
  buffers each — in VRAM next to the game's own working set. It now records one engine N times,
  so the cost is time rather than memory. The ceiling is 3, and the previous fix (capping the
  count) was never going to work, because two evaluations at 0.50 scale are ~32 ms against a 2 s
  driver timeout.
- **Pass Count did nothing at all on D3D11.** The loop that honoured the value existed only on
  the D3D12 path, so on every D3D11 target the setting was never read.
- **Pass Count still did nothing on a default install.** After the fix above, the count was
  forced back to 1 whenever Timing was *Same frame* — and *Same frame* is the default. The
  slider moved, saved to the ini, and changed nothing. The force is gone; the cost of a second
  pass in inline is framerate, which the panel colours and says. The log now prints one
  `pass N of M` line per pass with the engine's job id before and after, so an extra pass that
  is a no-op can be told apart from one that runs and changes nothing — two cases that had been
  producing the same reading.
- **Local Tone was zeroed on every pass after the first**, while structure and skin were written
  at full value on all of them. Nobody chose that, and it made a 1-vs-2 comparison read as two
  changes instead of one. All three fields now get the same value on every pass.
- **Motion Scale only ever applied to games that hand over a velocity buffer**, and it was
  hidden on every target without one. So the estimated motion field — the one that is a guess
  and the one most in need of turning down — had no control at all. Both fields go through it
  now, and the slider is visible whenever motion is on.
- **Depth Inverted is gone.** It was a switch that could only be set wrong: no run on either
  target ever produced a reading that separated the two settings, so it is now pinned to the
  engine's own default of 1 and not exposed. (The previous entry about it defaulting off still
  stands — that was a real bug, this removes the control that carried it.)
- **A bridge failure costs a rebuild, not the rest of the run.** It used to latch, after which
  the add-on returned from every present in silence and left the last image it wrote on screen.
- **The DXGI resize failure.** Loading the runtime pulled `d3d12.dll` into a D3D11 process, which
  made ReShade install its delayed D3D12 hooks and broke the game's next `ResizeBuffers`.
  Measured down to "the load alone triggers it, not any code we run". Fixed by resolving every
  graphics entry point by hand and loading D3D12 as a private copy under its own name.
- Access violation on frame 1 (a raw resource pointer held across frames), a freeze on frame 5
  (two fences sharing one auto-reset event), a permanent black screen (a global present gate
  plus a latching failure flag), and a hard machine hang from four inline passes.
- **A safety net.** If 600 presents go by without the bridge finishing a frame, the add-on
  switches itself off and gives the game's image back. The worst case is now "the effect does not
  appear" rather than a black screen or a freeze.

### Documented

- **The runtime's option struct**, field by field, in the README — so an offset that is written
  but never read can be told apart from one that matters.
- **Model A/B/C — closed, will not be implemented.** The NVIDIA add-on's Model combo is
  `DLSSNR.Style`, and it is now traced end to end: a table lookup, a bitmask, and three floats
  lerped into a 14-float block. It is not three networks — that DLL carries one weight set, so on
  paper it needs no data we don't already have.

  It still cannot be done, and the reason is not effort. **The AMD port is closed source.** Its
  kernels ship as precompiled GCN code objects, their argument metadata shows no room for the
  56-byte style vector, and adding a network input means recompiling kernels whose sources were
  never published. **Model A is the only model that exists on this side.** Written up in the
  README so the question stays closed rather than being rediscovered every few months.

### Fixed after the first outside review

Reported by @dumbjack, read straight out of the source. All five were confirmed present before
being fixed.

- **Two data races.** `historyValid` is cleared by the overlay's History checkbox and read and
  set by present, and the overlay does not hold the lock at that point. `depthEvents` is raised
  on the bind event, off the lock, and read from present and the overlay. Both are atomic now.
- **A depth target that could dangle.** The best depth-stencil candidate was kept as a bare
  pointer to a resource the *game* owns. A level load, a resize or a device reset frees it, and
  the depth path was still calling `GetDesc`, two barriers and a `CopyResource` against it on a
  later frame. It holds a reference of its own now -- **and it is released on swapchain
  teardown**, which the original report did not cover: holding a reference on a game resource
  across the game's own teardown is the shape of the DXGI resize bug this add-on already had
  once.
- **A wrong runtime DLL was read into memory before being rejected.** The runtime is one
  fixed-size binary, so the size settles it from the directory entry; the read and the hash are
  unchanged for a file that matches.
- **Two leaks on `Bridge::Ensure`'s failure paths**, in both the add-on and the session harness.
  `Ensure` calls `Destroy()` on the way in, so a retry would have reclaimed them -- but the depth
  path latches instead of retrying.

### New tools

Two standalone programs, built with `.uild.ps1 -Target <name> -Exe`. Neither needs a game.

- **`vkprobe`** asks the installed Vulkan driver whether it will let Vulkan import D3D12 textures
  and D3D12 fences, per format and per handle type, and prints a table. That is the precondition
  for ever running this under a Vulkan host such as RPCS3, and it is a question the driver
  answers before anything is created.
- **`vkbridge`** round-trips known bytes across that boundary in both directions and compares
  them.

On an RX 9070 XT both pass: `D3D12_RESOURCE` textures import, `D3D12_FENCE` imports as a
timeline semaphore, and every format the add-on carries survives the crossing byte for byte.
Notably `D3D12_HEAP` is *not* supported on that driver while `D3D12_RESOURCE` is — so the handle
type is not a free choice.

**This does not mean Vulkan works yet.** No Vulkan route is shipped in this release; what these
two prove is that the transport it would need is possible on AMD, which was not known before.

### The runtime moved to v0.2.17

The add-on was built against **DLSS-NR-on-AMD v0.2.14** and is now built against **v0.2.17**,
three releases on. Between them: a double-capture fix on FSR3 games that removes some flicker, a
Resident Evil Requiem startup crash, Nixxes ports, further RE Engine fixes on RDNA3, HDR exposure
handling for games that supply an exposure texture, multi-GPU black screens, and the stutter on
games with dynamic resolution scale — which is every emulator this add-on cares about. The two
performance releases, +6% and +2%, were already in v0.2.14.

`.hip_fat` is byte-identical in size across the two, so the network itself did not change. What
moved was everything around it: `.text` grew 28 KB, `.data` 7 KB, and **every offset this add-on
writes into moved with them.** All twenty-two were re-derived against the new build, none carried
over on faith, and the deltas are not uniform — the block splits into regions that shifted by
0x16A20, 0x16AE0, 0x16BA0 and 0x16BB0 respectively, so a single constant would have been wrong
for most of them.

Two entry points were relocated by matching prologues and call sites: the record function
0xa0b0 → 0xf600 and the init function 0x12380 → 0x19240, the latter 379 of its first 400 bytes
identical to the old one and reached from the same single call site.

**One binary patch is gone, replaced by a flag.** Against v0.2.14 this project edited the apply
shader so a timed-out frame kept its own input instead of pasting last frame's residual.
v0.2.17 writes that line as

```hlsl
if (tone & 4) { if (flags.Load(12) != 0) d = (tone & 2) ? prev[id.xy].rgb : float3(0, 0, 0); }
```

Bit 4 turns the guard on, bit 2 chooses the stale residual over nothing, and both are clear by
default — so out of the box a timed-out frame keeps a half-written buffer, which is worse than
either choice. The add-on now sets bit 4 and clears bit 2 when it writes ToneChannels, which is
the same outcome the patch forced and does not require touching the binary.

Also corrected: the patcher claimed the upstream installer applies five patches and that this
applies four of them. It applies none. `version.dll` on disk is byte for byte the payload
appended to `dlssnr_on_amd_setup.exe`; every change was always ours. `tools/extract_runtime.py`
now lifts that payload out without executing the installer, verified against v0.2.14 through
v0.2.17.

### The v0.2.17 port, and the one offset that got away

Everything above was written before any of it had run. It crashed on the first frame after
Enabled, twice, with the engine's own handler reporting `0xc0000005 at 0000000000000000` -- a jump
into nothing, on no module, before the first job.

The offsets were not the problem. The add-on read the engine's own defaults back correctly at the
new addresses (`8d9d0` LocalTone 0.0, `8d9d4` LocalStructure 1.0, `8d9d8` SkinStructure -1.0), and
both passes recorded. What got away was a **fourth entry point**: the add-on calls the runtime's
frame-notify function directly, and that call is written as `base + 0x4640` in three places rather
than through the `At<>` helper, so the sweep that relocated everything else never saw it. In
v0.2.17 that function lives at **0x9170**, and `base + 0x4640` lands in the middle of something
else.

The pairing is not a guess. Both functions reference the command-list marker twice, at exactly
`+0x98` and `+0xD5` from their own entry, and the second binary patch sits at exactly `+0x13` in
both. Three identical relative offsets.

Found by adding **`NullJumpProbe`** to the add-on, which stays. A vectored handler that fires only
on an access violation at address zero, reads the return address a `call` leaves at RSP, and logs
which module it points into and at what offset. One run, one line:

```
fault probe: jumped to null; stack+0 returns to amd-nr.addon64+0x6705
```

That named the culprit as the add-on rather than the runtime, and 0x6705 disassembles to the
instruction after `add rax, 4640h; call rax`. A project that drives a foreign binary through raw
offsets should own that probe permanently: the cost is a handler that returns CONTINUE_SEARCH, and
the alternative is guessing.

Two wrong turns on the way, recorded because the reasoning was plausible and still wrong:

- **Dropping the first binary patch.** The setup thread it kills grew from 1.3 KB to 3 KB and
  loads `d3d12.dll` and `dxgi.dll` by full system path, which read like proxy resolution. It is
  not: it builds a dummy device, window and swapchain purely to read five vtable slots and detour
  them. Killing it is still right, and the patch is back.
- **Blaming the offsets.** They were all correct. The measurement that settled it was cheap and
  should have come first: arm the engine but skip the record call, and see whether it still
  crashes. It did not, which put the fault inside one call and ended the speculation.

Measured after the fix, God of War II under PCSX2, 960x540, one pass, RX 9070 XT, 45 seconds
each:

| | v0.2.14 | v0.2.17 |
|---|---|---|
| jobs in 45 s | ~2500 | ~2500 |
| per job, reported | 15-16 ms, alternating with 0 ms | 9-10 ms, every sample |
| residual, 1 pass | 0.101091 | 0.000317 |
| residual, 2 passes | not taken | 0.000588 |

**This is not a speed-up, and the first draft of this entry wrongly said it was.** That claim came
from comparing v0.2.14's opening jobs against v0.2.17's steady state, which is not a comparison.
Read down the whole run and the throughput is identical: about 2500 jobs in 45 seconds either way,
roughly 18 ms of wall clock per job on both.

What did change is that the number stopped wobbling. v0.2.14 alternates between 15-16 ms and a
flat **0 ms** for half its samples, which is not a job that took no time, it is a job whose timing
was never captured. v0.2.17 reports 9-10 ms on every sample and splits out a figure v0.2.14 never
had, "network on the GPU", separately from the wait on the capture. Two builds measuring different
things, one of them badly. Steadier reporting is worth having and is not the same as being faster.

The residual is the thing to look at next and not to celebrate yet. On v0.2.14 it came back as
0.101091 against an input mean of 0.101140 -- the same number, which is what a correction measured
against an empty base looks like, not a correction. The new figures are small and they scale 1.85x
from one pass to two, which is much closer to honest accumulation than the 3.5x this project has
been chasing. Whether that is a better measurement or a weaker effect has to be settled on screen.

### Known, and not fixed

- The Pass Count machine hang has **not been reproduced or confirmed absent** since the rework.
  The probable cause was removed; that is not the same as a verified fix.
- The Portuguese accents have not been checked on screen. If the ReShade font lacks the Latin-1
  glyphs they will render as boxes.
- The `exposure` slot of the network's input packet is still never filled.
- Guide contents have not been measured during real gameplay, only on menus, where flat depth and
  zero motion are expected.
- **Residual Limit and Edge Fade are unmeasured.** They are off by default, so an untouched
  install behaves exactly as before, but neither has been compared against a `measure, residual`
  reading.
- **The between-pass barrier has never executed.** It sits behind `Pass Count > 1`, which until
  this release was forced back to 1 on the default timing. If a two-pass run loses the display
  device, that is the first thing to suspect.
- No Vulkan or OpenGL route. The add-on loads under a Vulkan host and then sits there.

## v0.2.0

The published release. See the repository at that tag.
