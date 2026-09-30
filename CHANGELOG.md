# Changelog

## v0.7.4 - 2026-09-30 - A game that replaces its window's swapchain keeps the network

Built on v0.7.3 with the same runtimes and the same bridge protocol (v5): an `amd-nr.ini` you already
have keeps every setting in it.

- **A game that makes a new swapchain after a failed resize no longer loses the network.** Where Winds
  Meet starts in a 1334x750 window, asks D3D11 to resize it with a flag the swapchain was not made with
  (refused, `E_INVALIDARG`), and makes a new swapchain at its own size instead. ReShade frees the old
  one without a second destroy, so the add-on stayed latched to it: the played window went out raw
  ("no frames yet"), and reopening the game crashed when the effect runtime called into the freed
  object. The add-on now moves to the swapchain made in its place (`core/addon/lifecycle.inc`), and
  `tools/transport_check.py` holds it. Reproduced and checked on a D3D11 host that does exactly that
  resize; not yet confirmed by the player who reported it.
- When the D3D11 route cannot make its depth view, the log now says why: the error and the device
  removed reason, which tells a GPU reset from a bad description.
- The 32-bit pair is rebuilt from the same core; nothing in it changes behaviour.

## v0.7.3 - 2026-09-30 - No more crash on D3D11 games when the optical flow starts

Built on v0.7.2 with the same runtimes and the same bridge protocol (v5): an `amd-nr.ini` you already
have keeps every setting in it, and the 32-bit pair is v0.7.2's, byte for byte.

- **D3D11 games no longer crash as soon as the network starts.** On D3D11 the add-on brings D3D12 in
  privately, as `dx12p.dll`, and the FidelityFX SDK looks up `D3D12.dll` by name to build its root
  signatures. The lookup failed, the optical flow came up with no pipelines, and its first dispatch
  called `SetPipelineState(null)` inside D3D12Core: the crash three players sent from D3D11 games.
  The SDK now finds the add-on's own copy (`core/temporal/ffx_d3d12_module.h`, forced into the SDK's
  sources only). Checked on a D3D11 host that crashed on the first frame before: 2,520 frames with
  the optical flow on, no crash.
- Nothing changes on D3D12, Vulkan or OpenGL, where `D3D12.dll` was always the one loaded.
- If you set `OpticalFlow=0` in `amd-nr.ini` to get around the crash, you can take it out again.

## v0.7.2 - 2026-09-28 - danielblnc's 0.5.1 supporter build

Built on v0.7.1 with the same bridge protocol (v5) and nothing else changed: an `amd-nr.ini` you already
have keeps every setting in it. Not tested in game; the runtime was driven without a game on an RX 9070 XT.

- **danielblnc's 0.5.1 supporter build runs**, beside 0.5.0, v0.4.3, v0.4.2 and v0.4.1. Like 0.5.0 it
  is not distributed: whoever has it supplies their own `version.dll` or setup, and
  `tools/patch_runtime.py` patches it (`493b4a3b…` in, `af67f066…` out). Its addresses were mapped
  from 0.5.0 by aligned instructions, one target per field, agree with the OptiScaler fork's
  `kAmd051`, and pass `tools/runtime_offsets_check.py` against the patched file.

## v0.7.1 - 2026-09-28 - Motion measured by optical flow wherever a game gives none

Built on v0.7.0, with the same runtimes (DLSS-NR-on-AMD v0.4.3, v0.4.2 and v0.4.1, and the 0.5.0
supporter build from your own files) and the same bridge protocol (v5). Nothing else changes: an
`amd-nr.ini` you already have keeps every setting in it. Tested in game on the D3D12 route.

### What changes when you update

- **On D3D12, Vulkan and OpenGL, and on D3D11 games that render no velocity buffer, the motion is
  now measured by AMD FidelityFX optical flow** instead of the add-on's block estimator. The
  estimator matched nine samples of a frame shrunk to an eighth, one sample per 8x8 block, so on a
  textured scene a camera pan came out as noise -- neighbouring blocks pointing opposite ways. The
  runtime, handed motion that did not line its history up with the picture, dropped the history:
  the effect faded and flickered while the camera moved and came back once it stopped. The optical
  flow is what the reference itself runs where a game has no vectors. A game's own vectors and the
  companion effect's still come first, and `OpticalFlow=0` in `amd-nr.ini` goes back to the estimator.
- **Each block's vector is spread over its pixels along the depth**, on routes that have depth: a
  pixel takes the vectors of the neighbouring blocks at its own distance, so a character's motion no
  longer bleeds onto the background behind it, nor the background's onto the character. Without
  depth it is plain bilinear, as in the reference.
- **32-bit games are unchanged.** The bridge's 64-bit helper has no optical flow built in and keeps
  the estimator.
- A very high Intensity or Structure still multiplies whatever the network does from one frame to
  the next; if the picture shimmers in motion, bring those two back towards 1 first.

### Logs

- The route line says `motion by optical flow` where it said `motion estimated`, the guide probe
  names the optical flow as the motion's source, and the D3D12 line after 600 frames no longer says
  the game is a PS2 that never computed motion.

### For developers

- The release build of the 64-bit add-on is `build.ps1 -Ffx <FidelityFX SDK>` (the AMD FSR SDK
  2.3.0); `tools/release-assets.ps1` refuses an `amd-nr.addon64` without the optical flow in it,
  because a plain `build.ps1` -- the one `gates.ps1` runs -- drops it without a word. The SDK's MIT
  notice is in `docs/third-party/fidelityfx-LICENSE.md`.
- `RecordNetwork` fills this frame's depth before the motion, which the densify reads;
  `tools/transport_check.py` holds that order.

## v0.7.0 - 2026-09-28 - DLSS-NR-on-AMD v0.4.3, steadier by default, Passes that apply live, and a long list of fixes

Built on v0.6.9. It moves to **DLSS-NR-on-AMD v0.4.3** (patched
`f3d9f2e5…`) and still runs v0.4.2 (`f9aa21a2…`) and v0.4.1 (`c8808716…`), with the same weights
for all three, so a by-hand update that keeps an older `dlssnr_amd_pass1.dll` still works. On 32-bit games `amd-nr.addon32` and
`amd-nr-host64.exe` go together (bridge protocol v5), and a mismatched pair is refused at the
header, as before. Both runtimes, danielblnc and mochizuki, go through everything below.

**Released together with AMD-NR-ReShade-Installer v0.6.7**, which moves its add-on, bridge and runtime
pins to this release and to the patched v0.4.3 build (12,749,824 bytes, `f3d9f2e5…`), offers the 0.5.0
supporter build from the person's own files, and no longer lists add-on releases before this one: they
refuse the v0.4.3 runtime. Tested in game on the Vulkan route (RPCS3) with the patched v0.4.3.

### What changes when you update

- **DLSS-NR-on-AMD v0.4.3, with v0.4.2 and v0.4.1 still accepted.** Upstream measures v0.4.3 20%
  faster than v0.4.2 in Reference quality and 18% faster in Fast, the default; v0.4.2 had been 15%
  faster than v0.4.1 at 1080p on RX 9000 in Fast, and brought the temporal history filter and one
  network block closer to NVIDIA's. The data block moved again on both; every address was mapped by
  its own anchor in each binary (v0.4.3 twice, from v0.4.2 and from 0.5.0, agreeing everywhere) and
  agrees with the OptiScaler fork's own map on every field both name, and the ones only this add-on
  uses (CpuWait, the network's GPU time, the watchdog's counters) were derived on their own. The two
  patches sit at `0x655d` and `0x9622` on v0.4.3 (`0x655d` and `0x91d2` on v0.4.2). Of v0.4.2's two
  new ini keys, `Quality` is set by the add-on now (**Network precision**, below), and
  `NoiseHandoff` is left as the person has it (off by default; with it on, Fixed seed still holds
  the noise still, at one more kernel launch per job). v0.4.3's new `OverlayKey` is for the
  runtime's own overlay, which stays off: it is drawn only from the Present hooks the first patch
  keeps the runtime from installing. The status column names the build that runs ("danielblnc
  0.4.3: network X ms"), on the 32-bit panel too.
- **danielblnc's 0.5.0 supporter build runs too, if you have it.** It is not distributed, by this
  project or by the installer: danielblnc gives it to his supporters, so you supply your own
  `version.dll` or its setup. The installer checks it, patches it and installs it for you; by hand,
  `tools/patch_runtime.py` does the same (`tools/extract_runtime.py` first, for the setup). Its
  addresses were mapped the same two ways as v0.4.2's and agree. It adds RDNA3 (RX 7000) kernels
  beside the RDNA4 ones; on RDNA3 it keeps a half-precision copy of the weights, about 280 MB more
  per pass, and on RDNA4 it allocates nothing new. Its own overlay stays off: it draws only from
  the Present hooks the first patch keeps the runtime from installing.
- **New: Fixed seed and Output smoothing, and with Temporal On they are the defaults** on every
  route, the steadiest set measured on recorded play. **Fixed seed** (Engine) holds the network's
  noise pattern still, as the reference does: on a still frame with nothing temporal on,
  consecutive answers used to differ by 0.44/255 on average and up to 15/255, and now a still
  frame gets the same answer every time. **Output smoothing** (Image) blends each pixel toward the
  previous answer, moved by the motion, by up to 0.80 where the two agree and less as they differ,
  and leaves differences of 10/255 or more alone (`OutputSmoothLimit`, ini only). A fresh
  `amd-nr.ini` is written with `Temporal=2`, `FixedSeed=1`, `OutputSmooth=0.8` and
  `OutputSmoothLimit=10`, and Factory Defaults restores them on both panels. An `amd-nr.ini` from
  an earlier release that holds `Temporal=0`, the default then, is moved once to `Temporal=2`, with
  `FixedSeed=1`, `OutputSmooth=0.8` and `OutputSmoothLimit=10` written as they already read, and the
  log says so; one that holds another Temporal keeps it, and reads Fixed seed and Output smoothing
  as on, since no earlier release wrote them. A `Temporal=0` set after that stays. To go back, set
  Temporal to Auto, untick Fixed seed and slide Output smoothing to off, or put `Temporal=0`,
  `FixedSeed=0` and `OutputSmooth=0` in `amd-nr.ini`.
- **New: Network precision** (Engine, one tick away under More settings), on danielblnc v0.4.2,
  v0.4.3 and 0.5.0: the runtime's `Quality`, on both panels. **Fast**, the default and the runtime's own, uses
  cheaper arithmetic (f32 accumulation, approximate square roots and reciprocals) and is about 13 to
  15% faster on RX 9000, with a difference that is barely visible; **Reference** is NVIDIA's exact
  arithmetic. A change applies from the next frame and starts the temporal history over. It is
  saved as `Quality` in `amd-nr.ini` (1 fast, 0 reference), Factory Defaults puts it back to Fast,
  and it is used instead of the `Quality` in the runtime's own `dlssnr_on_amd.ini`. On v0.4.2 a GPU
  without the fast kernels runs Reference whatever it says. v0.4.1 has no such setting and
  mochizuki is another network, so neither shows the control.
- **D3D12 games: every frame goes through the network.** Same-frame only made the GPU wait for the
  previous evaluation, so with frames in flight every present during that wait went out as the
  game drew it: 77 to 85% of the frames in a game that keeps three frames in flight, a strobe. The
  CPU now waits for the previous evaluation as well, as on the other routes, so every frame gets
  the effect and the frame rate follows the network (about 28 to 30 fps at Scale 1 in 1080p on an RX 9070 XT). For
  more frames, lower Scale.
- **`Passes` applies live in both directions**, on both routes. Each extra pass loads its own copy
  of the runtime (about 150 MB of VRAM) the first time a count needs it, one copy a frame at one
  long frame each, so going from 1 to 3 runs 2 for a frame on the way, and a 32-bit game never waits
  on two copies in one frame, which could run past the 5 s it gives the helper. A copy stays loaded
  until the game exits. A copy that fails to load is no longer tried again on every frame: the count
  is held at the passes that did load, the log says once what was asked for, how many run and which
  pass did not load, and the panel names the pass. A change waits until no copy has a job in flight,
  up to 2 s, and then starts history again, so a pass that sat out is no longer handed a frame from
  before. If the runtime does not drain three times running, the count is kept and the next change
  tries again.
- **Removed: Async timing on 64-bit games.** The network runs same-frame on every route. In async
  the runtime never reports a job as finished, so the effect only ran about twice a second. An
  `Inline=0` left in `amd-nr.ini` is ignored and the log says so, and Timing leaves the 64-bit
  panel. Where the runtime cannot run same-frame (its zero-copy path or flag pipeline did not come
  up) the effect now switches off with that reason, instead of running twice a second with the
  frame's own input handed on as its history. The 32-bit bridge's Timing, which pipelines
  presentation and is a different thing, is unchanged.
- **Removed: `GlHoldFrames`**, the OpenGL route's option to repeat the last result instead of
  waiting for the network (v0.6.0, off by default). The route now always waits, which is what the
  default did, and an ini that still sets it is ignored.
- **Removed: reading the D3D12 depth buffer live** when there is no pre-clear copy. It measured all
  zeros; the network now gets no depth there, which is what the panel already showed (None).
- **The panel only offers the guide controls the route can use**, and the log says once what the
  route reaches, e.g. `Vulkan route: no depth path on this API; motion estimated: no game MV path
  on this API; Feed.fx does not reach the network here`. **Use Feed.fx** is on 64-bit D3D11 games
  only (it used to be offered on D3D12, Vulkan and OpenGL too, where it did nothing). **Read from
  the game** is on D3D11 games, 64-bit and 32-bit. **Depth**, **Depth inverted** and **Stretch
  depth** are there on D3D11 and D3D12 games, not on Vulkan, OpenGL or 32-bit D3D9 ones. The same
  line is under **Debug** on the 64-bit panel; a 32-bit D3D9 game's shows `colour only` in place
  of the guide candidates. The ini keys are kept either way.
- **The 32-bit panel says why the helper switched off**: the device was removed, the engine or its
  textures could not be brought up, its GPU work did not finish within `FenceWaitCapMs`, the network
  was too slow even at the lowest scale, the engine could not run same-frame, or otherwise to look
  in `amd-nr-x86-host.log`. Bridge protocol v5: replace `amd-nr.addon32` and `amd-nr-host64.exe`
  together.

### Crashes, hangs and the effect switching itself off

- **On Vulkan and OpenGL, a route that stands down says so on the panel**, with its reason (the
  device lacking an entry point, the textures not crossing, a work slot that could not be recovered,
  and so on), where the panel went on reading Ready and only the log knew. An engine that did not
  come up keeps the reason it gave, mochizuki's own words or the `dlssnr_amd_pass1.dll` that was
  refused, as on D3D11 and D3D12, instead of `could not bring the engine up on the Vulkan bridge`.
- **When the game destroys the device the add-on runs on**, as emulators do when they switch
  game or renderer, the effect comes back on its own on the new device on D3D11, Vulkan and OpenGL:
  what was built on the old device is dropped, the history starts again, and the log says so once.
  If a D3D11 game's device is removed, its frames go out as the game drew them until it makes a new
  one. On D3D12 the effect switches off and the panel says to restart the game, because the network
  runs on the game's own device there. The add-on's own device being removed is checked on every
  present, on every route, and still switches the effect off. A present from a device or queue other
  than the one the add-on runs on goes out as the game drew it, said once in the log. The D3D12
  pre-clear depth copy is only ever taken on the add-on's own device.
- **A wait on the add-on's own GPU work no longer lasts for ever.** After `FenceWaitCapMs` (new,
  ini only, 10000 ms by default, at least 1000; `0` waits for ever as before) the effect switches
  off, the panel says to restart the game, and whatever the GPU may still be reading is kept
  instead of freed. Later resizes and the exit do not wait the whole cap out again. On
  Vulkan and OpenGL a depth bind no longer waits on the add-on's lock, except the eight it logs.
- **A network that keeps outlasting `InlineWaitMs` lowers the Scale, and at the lowest switches the
  effect off**, on every route. When the runtime's watchdog has had to stop 8 evaluations in a row
  (`WatchdogStandDown`, new, ini only; `0` turns this off), the scale is held a step lower, as three
  jobs over 250 ms already did; a network between `InlineWaitMs` and 250 ms used to hold the game's
  queue that long on every frame. Only at the lowest scale does it switch the effect off, and the panel
  says to restart the game; three jobs over 250 ms there do the same, which used to be only a log
  line, but only ones the watchdog stopped too, so a loading screen never counts. The log names the
  `InlineWaitMs` the runtime is using and warns when it is over its own 200 ms default; the add-on
  never changes it.
- **32-bit games: a slow frame no longer takes the helper down.** Its waits on the GPU now end after
  4 s, before the 5 s the game waits for an answer, and that frame goes out as the game drew it with
  the helper switched off, instead of the helper being killed mid-wait. With same-frame presentation
  a frame whose answer misses the 5 s goes out as the game drew it and the late answer is collected
  on the next frame; only a second one in a row stops the bridge. The D3D11 drain, on the 64-bit
  route too, and the D3D9 one no longer keep a CPU core spinning while they wait: past the first
  millisecond they wait a millisecond at a time on a high-resolution timer.
- Every route decides whether to run the network in one shared place, and asks the runtime itself
  whether a job is still in flight. No route records on top of an unfinished job any more: those
  frames go out as the game drew them until the runtime has finished it, and a job still waited on
  is named in the log every 600 skipped frames. Before, every route gave up on a job after 500 ms
  and recorded the next one behind it, and the runtime, which finishes jobs in order, refused
  frames once four were in flight. A command list that fails to close after a swapchain rebuild
  still tells the runtime, so its job is retired rather than waited on for ever. A skipped frame is
  counted and logged the same way everywhere. Vulkan and OpenGL now also time each job, and on them
  and the 32-bit bridge the scale cap after three long evaluations really lowers the scale; the
  32-bit panel used to show that cap while the network still ran at the slider's scale.
- Switching the effect off and on again (hotkey, panel or alt-tab), or a 32-bit game coming back
  from a pause, no longer counts the time it was off as one long evaluation, which counted toward
  the three that lower the scale.
- **OpenGL: imported fences that do not work no longer hang the game on the first frame.** GL's
  first signal is now seen on the CPU before the work queue waits on it, so the fall-back to the
  CPU stall can no longer wait for ever. With `Passes` above 1 the first pass no longer reads the
  frame before GL has finished writing it, and a new GL context gets its fences imported and
  proven again, and our queue drained, before anything is rebuilt.
- On Vulkan and OpenGL a work slot is no longer reused while the GPU still runs it after a 2 s wait
  ran out (alt-tab), and no fence wait ends early on a wake another wait left behind.
- An add-on unloaded mid-game (a game that drops its device unloads it) no longer leaves its crash
  probe or the OpenGL fault filter pointing into freed code, and nothing the add-on holds on the GPU
  is released at exit after the runtime and the driver have gone.
- `Events` without bit 8 no longer drops the drain a resize waits on: destroy-swapchain is always
  subscribed.
- **Vulkan on a machine with two GPUs** (a laptop, or a desktop with the CPU's graphics on) now
  works on the GPU the game renders on, found by its LUID, instead of always the first one, which
  stood the route down at the first import. If the add-on loaded after the game made its device,
  the first is still taken and the log says why. A resize or fullscreen switch no longer waits for
  every queue of the game's Vulkan device, only for the add-on's own work.
- **A second window no longer switches the effect off, resets it or rebuilds it at its own size.**
  Only the first swapchain the add-on takes is processed, until the game destroys it for good: a
  resize keeps it, and a Vulkan swapchain rebuilt or an OpenGL window restored from minimised takes
  it back even if another window presented meanwhile. Another window's frames go out as the game
  drew them, said once in the log, and its ReShade effects no longer run into the played window's
  frame or feed it their depth and motion. Before, with Disable On Alt-Tab on, a second window in
  the background switched the effect off; one minimised dropped the history on every frame, and one
  of another size rebuilt everything on every frame. The 32-bit route already kept to one swapchain;
  the other one's frames no longer count in its frame timing or clear its depth and motion tallies.
- On a 32-bit game with pipelined presentation, an answer still in flight when the game is
  minimised or the effect switched back on is dropped, not shown after the restore.

### Image

- **The runtime is handed the whole packet it reads.** Since v0.4.1 its record entry reads 0x60
  bytes, 16 more than the add-on passed, among them a jitter pair it reads on every frame; those
  came from whatever was on the stack. They are zero now, as the OptiScaler fork passes them.
  Whether the stack bytes ever moved the picture was not measured.
- **Lighting no longer flickers where the buffer taken for the game's motion is not really
  motion**, on every route. The motion handed to the network is checked on every frame: a vector
  longer than the raster, inf or NaN becomes no motion. A guide could take a buffer of the right
  format and size that holds something else, 65504 px (the largest half-float) on every pixel, and
  then the runtime's history and Output smoothing were fetched from off the frame and the network
  was left alone, flickering; a buffer like that is now given up on and the estimator takes the
  motion over. Measured on such a game, the lighting's frame-to-frame variation fell 2.6 to 4.8
  times; a game with real vectors is unchanged.
- **32-bit games with 2 or 3 passes no longer blink.** The helper told only the first pass's
  runtime copy that the frame was submitted, so the last pass's job never ran and the effect came
  in 500 ms stretches of frames drawn as the game drew them, with 1 to 3 frames processed in
  between: 33 of 481 frames at 2 passes. Every copy is told now, and every frame is processed.
  Each pass adds its own GPU time.
- A frame the network did not answer is shown by one rule on every route: the game's own frame,
  graded when a style is selected, or the debug view or Network Output when one is on. On OpenGL it
  no longer repeats the last result, and on the 32-bit bridge it no longer loses the style, the
  debug view and Network Output. The 32-bit panel's skipped percentage is counted as on the 64-bit
  routes, so a graded skipped frame still counts as skipped.
- On D3D12, and on OpenGL with its fences, a frame that arrives while the last one is still on the
  GPU after the 500 ms hold ran out no longer rewrites the views that one is still reading, which
  could compose it from another back buffer: an old frame flashing up. The frame goes out as the
  game drew it, without the style or Network Output, and the log counts such frames every 600.
- A NaN or inf the network returns for a pixel is taken as no correction, where it used to darken
  that pixel (black with the additive composition). On a frame Output smoothing blends, the pixel
  becomes the previous frame's, or the game's own where that one is bad too, so it is not stored in
  the history the next frame is smoothed against. The log's residual measurement counts them
  (`measure, non-finite`); with Output smoothing on it reads after the smooth, so only a run with
  OutputSmooth=0 says whether the network returns any.
- **A window resized for good now gets a raster of its own size.** On every route the raster kept
  the size it started at until Scale moved, so a game windowed from 1080 to 1017 lines ran the
  network on a frame stretched to the old shape. A new size is now followed once it has lasted 120
  presents and is more than 2% off; a game that keeps changing size (common under emulators) still keeps
  one raster. Following it starts history again, as a Scale change does.
- The guide probe looks again, 120 presents later, whenever the game's depth or motion moves to
  another buffer or is taken back after a resize, on D3D11, D3D12 and the 32-bit bridge. It used to
  stop for the session after one good reading or five bad ones, so the depth range the guide is
  scaled by, and the check that motion really is velocity, only ever covered the first buffer (with
  DepthNormalise on, a move from a small range to a buffer that already fills 0..1 still keeps the
  small range's scale). On a 32-bit game a motion buffer given up on as not velocity no longer
  costs the game's motion for the rest of the session: the next buffer the game moves to is used,
  and probed in turn. The same buffer taken back after a resize is used again for about 2 s, until
  the probe gives up on it again.
- **Depth the guide probe reads as JUNK, or as FLAT twice while something moves, is no longer
  handed to the network**, on every route. It used to be logged and fed anyway, so a game that
  clears depth before present gave the network a constant plane. A reading
  that varies hands it back, and so does every new look after a guide is taken, even the same
  buffer taken back after a resize (until two readings withhold it again). A flat menu with nothing moving
  never withholds it; one that moves (a spinner, a video) can, so while depth is withheld as FLAT
  the probe does not stop after five readings but keeps looking, less often each time (at most
  every 4800 presents), and the game's scene in the same buffer gets depth back once it is read.
  The log says each change (`guide depth: ... handed depth`), the probe's arm line says
  `depth fed 1 (handed 0)`, and the Status column reads `depth unusable` on both panels while it
  is withheld.
- **Depth a game clears before present is copied just before the clear instead**, on the 64-bit
  D3D11 route and the 32-bit bridge, and the guide probe looks at that copy.
  Only once the probe has withheld the copy taken at present, and the first time only on trial: an
  engine that clears at the start of a frame would hand over the last frame's depth there, and a
  menu that moves is withheld the same way before its scene arrives in that buffer. So the first
  copy before a clear that reads right goes back to the copy at present, once, and is probed again,
  and depth that reads right at present stays this frame's. Withheld there a second time, the copy
  before the clears stays until the depth moves to another buffer; such a game pays one more probe
  cycle for it (some 14 s at 60 fps). A copy that reads no better is withheld as before. The log
  says `withheld as copied at present, so it is copied just before the game clears it`, `reads right
  as copied just before the game clears it, so copied at present again, once`, and `the snapshot
  taken just before the game's last clear` once such a copy is used; the 32-bit panel's Depth
  candidate line says `before_clear=1`. On D3D12 the pre-clear copy is handed over only while it is
  of the buffer the depth pick holds and at most two presents old: a game that stops clearing, or a
  newly picked buffer, now gives no depth instead of an old frame's, and the Status column no
  longer names the snapshot as the depth source then. `GameGuides=0` (ini only on D3D12) now stops
  that copy and the depth pick behind it, as it stops the D3D11 route looking at all.
- **The temporal history is dropped by one rule on every route**, and the log says why
  (`temporal history dropped: <reason> (<n> so far)`, a line each time the reason changes). It goes
  when a raster of a new size is built, when Scale, DepthInverted, History, Temporal, Motion, Depth,
  GameGuides, MotionScale, FlowGate or FlowRatio change on either panel (the 32-bit one used to
  react to History alone), on every ini reload, when the effect is switched back on (the 64-bit
  route used to hand the first frame a history from before a hotkey or panel switch-off), when a
  game's depth or motion buffer takes the place of one the network was fed, when the guide probe
  withholds the game's depth or hands it back, on a new OpenGL context, and, for the passes that
  sat a frame out, when fewer passes ran than were asked for. A swapchain rebuild alone no longer
  drops it on the 32-bit route, where a game that rebuilds every few frames (common under emulators)
  lost its accumulation each time; neither route drops it there now, nor when the game's depth and
  motion are found again a few presents after it, unless the guide probe had withheld that depth:
  finding it again hands it back, which drops the history, and again when the probe withholds it.
- A frame the network skipped now drops the history when the motion handed with the next
  evaluation is measured present to present (the game's own vectors, Feed.fx): a history a frame
  older than that motion no longer lines up with it. It used never to be dropped on a skip. With
  the motion the estimator measures between evaluations, which spans the same gap as the history,
  it stays: dropping it there cost the whole temporal gain on still content.
- **The 32-bit route picks the game's depth and motion buffers the way the 64-bit one does.** Both
  now run one copy of the D3D11 guide code, and the 32-bit bridge gains what it was missing: a depth
  buffer under 256 pixels is no longer a candidate, ReShade's own effect targets are never taken for
  the game's, `GameGuides=0` stops it looking at all, and the first guide is taken over three
  presents instead of being decided by the third alone.
- On D3D11, an `_SRGB` back buffer no longer fails the 64-bit bridge ten times over and stands it
  down with "the bridge kept failing", nor fails a texture creation in the 32-bit helper, logged, on
  every frame. An MSAA back buffer on the 64-bit route goes out as the game drew it, as on D3D12 and
  the 32-bit helper, and is said once in the log, instead of counting as processed with nothing
  changed. While the effect is off, the 64-bit route no longer keeps collecting references to the
  game's render targets.
- On D3D12, the finished image is copied to the back buffer from the state a copy needs, a network
  failure still returns the back buffer to the game ready to present, and the runtime hears of a
  frame only once it has been submitted, as on every other route.
- On D3D12, a depth buffer that changes format gets a new pre-clear copy, and the old copy, like a
  probe readback the GPU has not finished in 2 s, is kept until the next resolution change instead
  of being freed while the GPU may still use it.

### Settings and runtimes

- **One settings table for every route** (`core/x86bridge/settings_fields.inc`): each setting's
  ini key, default and range, from which the ini is read and written, both panels are filled and
  the 32-bit bridge's wire is laid out. The same ini now gives the same settings on both routes:
  - The 32-bit panel no longer clips what the 64-bit route keeps as soon as a control moves: Edge
    Fade up to 0.49, a per-pass Skin at its -1 automatic, and Intensity, Structure, Tone, Skin,
    Motion Scale, Diffuse White and the other keys the 64-bit route never bounded.
  - A value typed into a 64-bit slider is clamped to the same ranges.
  - Output smoothing and Fixed seed are on the 32-bit panel, and Factory Defaults resets them there
    too.
  - A fresh 32-bit `amd-nr.ini` and the 32-bit Factory Defaults are the 64-bit ones: Colour
    Strength 1.0 instead of 0.25, and Scale 1.0 instead of 0.5 for Factory Defaults. On both
    routes an `amd-nr.ini` without a `Scale` key reads 1.0, as a fresh one is written.
  - A key missing from `amd-nr.ini` reads as its default on a Reload too, where it used to keep the
    value from before; one set to nan or inf reads as its default instead of being taken.
  - `SerialPasses` is saved.
- **The mochizuki runtime shares the danielblnc path's rules:** a new pass count starts the history
  again, a game that keeps changing size (common under emulators) keeps one raster, where with mochizuki
  every size it flapped through rebuilt the raster and started history again, the panel offers every
  pass, and it runs through the same run-or-skip decision, device checks and stand-downs on every
  route.
- **The danielblnc runtime is recognised from a table of the builds this add-on knows**
  (`core/addon/runtime_offsets.h`), by the hash of `dlssnr_amd_pass1.dll`, and every address goes
  through the build found. A file that is none of them is refused as before, and the log names the
  builds it takes. `tools/runtime-patches.json` lists each build's patches and patched hash;
  `tools/patch_runtime.py` picks the build from the file it is given and checks what it writes, and
  `tools/runtime_offsets_check.py` proves the build it is handed, and without one refuses a build
  that leaves an address out.

### Logs and the Debug section

- **A line a second says what the network did with that second's frames**, on every route and in
  the 32-bit helper's log, with `Diagnostics=2` in `amd-nr.ini`: `stats: 1.00 s | presents 61 eval
  58 skip 3 (heap 1) refused 0 | bind waits 2 | job max 18 ms`. That is the frames the network ran
  on, the ones it sat out because the GPU still had the last one (of those, `heap` were held because
  a list was still reading the add-on's views), the passes the engine refused, how often one of the
  game's binds or clears waited for the add-on's lock, and the longest evaluation. The same line is
  under **Debug** on the 64-bit panel whatever `Diagnostics` says. `Diagnostics` is bits now: 1 is
  still the Ctrl+Home and Ctrl+PageDown keys, so 3 is both. A refused pass no longer counts as a
  skipped frame, and the skipped share on both panels is of the frames presented; it counted each
  skipped frame twice, so it read low.
- **A `temporal:` line says what the engine was handed**, on every route, when it changes and at
  most once a second: `temporal: byte 1 (Temporal=2), engine same-frame, history handed 1/1,
  smoothed 1/1 at 0.80 under 10/255, seed pinned, motion estimated, depth not handed`. It is read
  back out of the runtime rather than from the settings, so it says whether history, Output
  smoothing and depth really reached the network, and which motion did. It is under **Debug** on
  the 64-bit panel too, and the Status column's motion source is now the one the network last got:
  `none` with Motion off, where it said `estimated`, and `optical flow` in a lab build.
- **The stats line times the hold that makes each present wait for the last evaluation** on D3D12
  and on OpenGL's fences: `| hold 3.1/9.4 ms, deadline 0, spin 0` is its mean and longest, how often
  it ran out at 500 ms with the last frame still on the GPU, and how often the runtime's job count
  was still behind after it. `D3D12Wait=0` in `amd-nr.ini` (new, ini only, never saved) turns the
  hold off, to set what it costs against the frames it saves, and the log says so at load.
- **A refused pass says why**: `pass 2 refused (1 total): four jobs already in flight: job 816, 812
  retired`, or that the engine was not ready, or that the runtime's own state shows no reason. The
  stats line also adds up how often the runtime's watchdog let the game's queue go (`timeouts`, over
  every pass's copy of the runtime) and ends on each copy's work (`module 1 job 816 retired 816`),
  and after a `Passes` change the log gives each pass's job ids again, for the new count.
- **The log says what will not be used.** The `settings:` line carries the lab keys (`lab flow 0
  maxpx 0 guard 0`: OpticalFlow, MotionMaxPx and HistoryGuard) where it said `inline 1`, which it
  always is now. The route's line says when the ini sets `Stage` or `NoBridge` on a route that does
  not read it: `Stage` stops only D3D11, OpenGL and the 32-bit helper part-way, and D3D12 has no
  bridge to keep down. The guide probe says what share of the motion the feed is about to zero,
  as longer than the raster (or `MotionMaxPx`), inf or NaN. `NoBackBuffer=1` now works on D3D12
  too, where it was ignored: the frame goes out as the game drew it.
- **The residual measurement measures flicker too.** A full measurement (at frame 240, on **Measure
  residual**, or Ctrl+PageDown) now also reads the next evaluated frame, and says how far the
  network's answer moved where the picture did not: `measure, flicker between two evaluations where
  the input held still (89% of the samples): mean 0.00064, p99 0.0039, over 1/255 12.0%, over 4/255
  0.30%`, the same ruler the recorded-play measurements use, on every route and in the 32-bit
  helper's log.
- **`tools/host_check.py --cross`** holds one build against itself across the APIs, at a fresh
  ini's settings: each API's host presents the same still frame, `--frames` of them are kept
  (`HOSTCHECK_FRAMES` in the capture add-on), and it prints each API's still-frame flicker and
  whether each pair of APIs agrees within their own noise. `--addon32` adds the 32-bit bridge's
  D3D9 and D3D11. `--selftest` checks its verdicts without a GPU.
- The Debug view's depth entry is **Depth (auto-scaled)**, which it has been since the guide probe
  started setting its scale from the range it measured; it still said x500, the PS2's. The 32-bit
  frontend's frame line says `same_frame=0` when presentation is pipelined, where it always said 1,
  and `result=-1` when no answer came back with that present.

### For developers

- **`tools/gates.ps1`** is in the repository and runs every check in one pass (`-All` adds the
  diagnostic builds). CI runs the checks that need no GPU and no runtime binary: the Python ones and
  the raster pin on Linux, the compiled fence-wait, watchdog, ini-migration, bridge and factory ones
  on Windows.
- **Recorded sequences** can be played through the whole pipeline and measured in time
  (`tools/seqgen.py`, `seq_run.py`, `seq_player.py`, `seq_table.py`, `temporal_metrics.py`, with
  framecheck), which is how the new defaults were chosen.
- A lab build (`build.ps1`) can run the reference's optical flow and history filters ahead of and
  around the network (`OpticalFlow`, `MotionMaxPx`, `HistoryGuard`); a release build reads
  `OpticalFlow` as 0. lmxxf's MIT notice is in `docs/third-party/lmxxf-LICENSE.md`.
- Most of `core/addon/neural.cpp` moved into includes of their own (the job gate, the guide and flow
  probes, the motion sources, the 64-bit panel glue, the D3D12 depth pick, the scale cap, the HIP
  device pick, the fault probe), with no change in behaviour. The line-limit check keeps new files
  under 500 lines and never lets one already over it grow.

## v0.6.9 - 2026-09-26 - DLSS-NR-on-AMD v0.4.1, and the runtime in the status column

Requires the pinned **DLSS-NR-on-AMD v0.4.1** runtime; v0.4.0 is refused by hash. The weights are
unchanged, so an upgrade replaces one DLL and downloads nothing else.

**Released together with AMD-NR-ReShade-Installer v0.6.3**, which moves its runtime and add-on pins
to this release and to the patched v0.4.1 build (9,916,928 bytes, `c8808716…`, as in
`tools/SHA256SUMS.txt`) in the same step. Tested in game in Euro Truck Simulator 2 (64-bit) and
GTA IV (32-bit bridge).

- **Move to DLSS-NR-on-AMD v0.4.1.** Upstream runs the network on a high-priority GPU queue
  (`QueuePriority`, on by default) and measures 8% faster than v0.4.0, 9% more under heavy load.
  The data block moved again (+0x2020 up to the history fields, +0x2038 from the ready byte on), so
  every address in `core/addon/runtime_offsets.h` was mapped from v0.4.0 twice: instruction
  windows, and the aligned references of every matched function. The two agree on every address,
  and with the OptiScaler fork's own map. The two patches moved to `0x65bd` and `0x91a2`. framecheck
  on its 960x540 frame gives byte-identical output to v0.4.0.
- **The status column names the danielblnc runtime and its cost**: "danielblnc 0.4.1: network
  13.3 ms", the network's GPU time for the last frame with every pass added up, read from the
  runtime (the "ms network on the GPU" of its own log). A `dlssnr_amd_pass1.dll` of another
  release is refused with a panel note that names the one this add-on needs.
- **The 32-bit bridge shows the runtime line too**, danielblnc's and mochizuki's, which v0.6.8 left
  out. Bridge protocol v4: `amd-nr.addon32` and `amd-nr-host64.exe` from this release go together,
  and a mismatched pair is refused at the header, as before.

## v0.6.8 - 2026-09-26 - the mochizuki runtime

**Released together with AMD-NR-ReShade-Installer v0.6.2**, which pins this add-on and, when its
mochizuki box is ticked, installs `MochizukiNrRuntime.dll` and `dlssnr-amd\` beside it: the build
OptiScaler 0.4.1-amd-nr carries. The danielblnc runtime stays the default and is unchanged.
Approved after an in-game try in Euro Truck Simulator 2 (D3D11).

- **A second runtime: mochizuki.** **NR runtime** in the panel (`NrBackend` in `amd-nr.ini`, applied
  on the next launch; danielblnc stays the default) can run the network through
  `MochizukiNrRuntime.dll` (mochizuki0323's Vulkan port, as the OptiScaler AMD NR fork builds it)
  instead of `dlssnr_amd_pass1.dll`. It runs on a Vulkan device of its own and meets our D3D12
  device through shared buffers and a shared fence; the frame's command list is submitted in two
  halves around it. Scale, Passes, Structure, Tone, Skin, the mask, the per-pass profiles and every
  compose control apply; the Engine controls that write into the danielblnc runtime do not. The
  status column shows the build and the network's GPU time. framecheck on its 960x540 frame: 6.8,
  11.7 and 17.3 ms for one to three passes, mean correction 0.029 (0.030 on the danielblnc
  runtime), with a colour cast that Colour Strength 0 takes back to the OptiScaler fork's default.
  With `NrBackend=mochizuki` and its files missing (the installer took them out) it runs
  danielblnc, and the panel says mochizuki is not installed. On the 32-bit bridge the status line
  is not shown yet; `mochizuki_nr.log` has it. **Export logs** takes `mochizuki_nr.log` too.
- **framecheck drives either runtime**, and waits for the mochizuki network to be built.

## v0.6.7 - 2026-09-26 - DLSS-NR-on-AMD v0.4.0

Requires the pinned **DLSS-NR-on-AMD v0.4.0** runtime; v0.3.0 is refused by hash. The weights are
unchanged, so an upgrade replaces one DLL, now about 10 MB, and downloads nothing else. Upstream
describes v0.4.0 as 42% faster than v0.3.3; here it has only been measured against v0.3.0, on a
synthetic frame, below.

**Released together with AMD-NR-ReShade-Installer v0.6.1**, which moves its runtime and add-on
pins to this release and to the patched v0.4.0 build (10,027,008 bytes, `ff6feffa…`, as in
`tools/SHA256SUMS.txt`) in the same step. This add-on refuses the v0.3.0 `dlssnr_amd_pass1.dll` by
size and says "a different build" in the panel, so a by-hand install needs the new runtime too.

- **Move to DLSS-NR-on-AMD v0.4.0.** Every offset this add-on writes into moved again, and every
  v0.3.0 address lands in `.rdata` on v0.4.0; see [below](#the-runtime-moved-to-v040).
- **The network's output is not v0.3.0's, and why is not known yet.** framecheck on its synthetic
  960x540 frame, same settings, RX 9070 XT: the mean |residual| on v0.4.0 is 1.18x v0.3.0's on the
  first frame, 1.15x at frame 12 and 1.24x at frame 20 (0.0353 against 0.0300, 0.0361 against
  0.0315, 0.0381 against 0.0307), with the same structure (correlation 0.985 to 0.989). It is there
  on the first frame, which has no history. It is not the new gfx12 kernels -- `DLSSNR_NO_REG=1`
  gives a bit-identical output -- nor the new keys: `UseGameExposure=0` leaves it as it is and
  `Residual=0` moves it by under 0.1%. Every value this add-on writes was seen to reach the output
  (EngineScale x2 doubles the residual). The same runs put a frame at 8.9 ms on v0.4.0 against
  15.5 ms on v0.3.0. In game (GTA IV, 32-bit bridge) it was compared with v0.3.0 before this
  release and approved.
- **Keep the system `d3d12.dll` out of D3D11, Vulkan and OpenGL games, and out of the 32-bit
  bridge's helper, on the new runtime.** Since v0.3.1 the runtime delay-loads d3d12 rather than
  importing it, and the private copy's rename only read the import table, so on v0.4.0 it would have
  found nothing, loaded the runtime as it is, and the first evaluation would have pulled the system
  `d3d12.dll` in -- the NFS 2015 resize crash the private copy exists to prevent. The rename covers
  the delay-import table now, and `tools/runtime_offsets_check.py` fails a runtime that names
  `d3d12.dll` in neither table.
- **Pin four of the runtime's new ini keys to what v0.3.0 did.** On this route the runtime reads
  `dlssnr_on_amd.ini` once, when it loads (its re-read every 120 presents sits behind the setup
  thread the first patch removes), and the add-on now overwrites these right after, so a file left
  in the game folder cannot reach past them:
  - `CpuWait` to 0. At 1 the notify entry holds the thread presenting the game's frame until the
    network finishes, up to twice `InlineWaitMs`, on every frame. Its default of 2 never waits on
    this route.
  - `Style` to 0. It goes to the network as `Style`/128, into the input v0.3.0 held at zero, and
    the standalone runtime's own overlay writes it back into that ini, so a folder that once had
    the standalone can carry 1 or 2. Without the pin that would feed the network behind the
    add-on's back, and a Model would no longer be its grade alone.
  - `ToneCurve` to reinhard and `ToneLift` to 0, which the same overlay also writes. They reshape
    the apply pass's tonemap when Tonemap is on.

  Measured with framecheck on its synthetic frame, Tonemap on: with the pins taken out, `Style=2`
  moves the output by a mean 0.008 and `ToneCurve=aces` with `ToneLift=0.25` by 0.009; with them,
  an ini carrying all of that plus `CpuWait=1` gives the same bytes as the add-on's own ini.
  `UseGameExposure` is left alone: the runtime honours it only when it is handed an exposure
  texture, and this add-on hands it none.
- **Two runtime patches, not three.** The dropped one rewrote the runtime's timeout log line from
  "previous residual shown" to "current input kept", and the original was the true one: on this
  route a timed-out inline frame after the first is shown with last frame's residual, on v0.3.0 as
  on v0.4.0. The same reading retires the claim that ToneChannels bits 4 and 2 choose that policy;
  the runtime builds that word from its own state. ToneChannels is still written non-zero, because
  at 0 the runtime zeroes LocalStructure, and the panel's help now says only that.
- `tools/extract_runtime.py` reads the setups from v0.3.3 on, which carry the DLL as a byte array
  in the installer's `.rdata` instead of appended after it.

The source is laid out by layer, one transport per graphics API, and both routes draw the same
panel. The output was checked against v0.6.6 byte for byte in 26 settings that move the picture,
with the temporal path (motion, history, depth) switched off for that check. Fixes a player would
notice:

- **A 32-bit D3D9 game no longer crashes on exit** (`0xC0000409`) with the effect on. Present in
  v0.6.6.
- **A resolution change in Async mode no longer switches the effect off for the rest of the
  session** when the runtime is still busy.
- **A resize no longer clears the game's D3D11 state**, which left an emulator that caches its own
  state (PCSX2) drawing with nothing bound. On both the 64-bit and the 32-bit route.
- The 32-bit helper lifts its scale cap when the slider is let go unchanged.
- **Factory Defaults** is on the 64-bit panel too, as it was on the 32-bit one. It keeps the
  language, the hotkey and which controls are shown, and sets Scale back to the 1.0 a fresh
  `amd-nr.ini` starts with.
- With the additive composition, the Guard help no longer leaves its `(?)` on its own.

### The runtime moved to v0.4.0

`dlssnr_on_amd_weights.bin` is the file v0.3.0 used, and the runtime's own table of the 153
tensors it expects is the same in both builds, names, sizes and order, so the network did not
change. Everything around it did: `.text` grew by 49 KB, `.hip_fat` from 6.3 MB to 8.9 MB and from
34 kernels to 86, and `.data` moved from 0x93000 to 0xA3000 -- so **every v0.3.0 address this
add-on writes lands in `.rdata` on v0.4.0**, the read-only page the v0.3.0 port already crashed on
once. None carried over on faith. The option struct keeps its shape but moved by 0x10AE8 up to
ToneChannels and by 0x10AF8 after it, where v0.3.3 inserted `Style`, `ToneCurve`, `ToneLift` and
`UseGameExposure`; the device block, the job block and the watchdog each moved by their own delta.

The method was the one that worked for v0.3.0, done twice and compared. Once mechanically: every
function of v0.3.0 matched to its v0.4.0 counterpart by its strings and imports, the two
instruction streams aligned, and every aligned reference to `.data` voting for where an address
went -- run through v0.3.1 and v0.3.3 as well, where it reproduced the OptiScaler fork's own
independently read v0.3.1 layout. Once by hand, from an anchor in the new binary for each address:
the ini reader's key string beside its store, a log format string that labels the argument, the
record entry's three opening tests, the init call site storing its result into the ready byte.
They agree on every address.

| | v0.3.0 | v0.4.0 |
|---|---|---|
| notify | 0x9460 | 0x9e10 |
| record | 0x12640 | 0x14cd0 |
| weights loader | 0x1fe80 | 0x26110 |
| device, queue, engine object | 0x96f68 / 0x96f70 / 0x96f78 | 0xa78c0 / 0xa78c8 / 0xa78d8 |
| job counter / job id | 0x977d4 / 0x97a6c | 0xa824c / 0xa8554 |
| watchdog job counters | 0x97950 / 0x97954 | 0xa83e0 / 0xa83e4 |
| option struct (Enabled) | 0x97b1c | 0xa8604 |
| effective HIP device | 0x97c30 | 0xa8728 |

The two patches kept moved too -- `0x60a6` to `0x667d` and `0x8873` to `0x9232` -- and are the
same two: kill the runtime's own setup thread, and remove the doubled `ExecuteCommandLists` from
the notify entry this add-on calls.

**What v0.4.0 brings that reaches this route.** On RDNA4 (gfx12), and only there, the network runs
new register-tiled kernels by default (`DLSSNR_NO_REG` turns them off); that is inside the record
function this add-on calls, so it applies here. On every GPU, the inline wait is recorded as 1-pixel
draws on the command list the add-on hands over, instead of a compute spin the OS cannot preempt,
which the runtime's own log blames for GPU watchdog resets every few minutes (`SpinDraw`, on by
default). The runtime reads four new keys from `dlssnr_on_amd.ini` -- `Style`, `ToneCurve`,
`ToneLift`, `UseGameExposure` -- and the add-on pins the first three to their defaults, as above;
the fourth has no effect without an exposure texture. As read in the binary, each default is what
v0.3.0 did without the key: `Style` 0 is the zero v0.3.0 held that network input at. That is a
statement about the keys, not about the output, which does not match v0.3.0's; see *The network's
output is not v0.3.0's* at the top.

**What does not.** Everything behind the setup thread the first patch removes: the FSR and
frame-generation detection, the wait for a game to load d3d12 and dxgi, and the one thing that
would make `CpuWait=2` wait.

## v0.6.6 - 2026-09-22 - The 32-bit panel is the rebuilt one

Same pinned **DLSS-NR-on-AMD v0.3.0** runtime, same weights, and **the 64-bit add-on is
unchanged by this release**: `amd-nr.addon64` is the v0.6.5 binary, still pinned by the same
hash. What ships here is the 32-bit pair, `amd-nr.addon32` and `amd-nr-host64.exe`.

The panel rebuild in v0.6.5 landed on the 64-bit route only. The 32-bit bridge kept its own
overlay -- forty controls, eight headers, and the MEASURED/TRACED/UNKNOWN/INERT tags -- so any
D3D8/D3D9/D3D11 game showed the old menu over the new image. The backend was never old: the helper
compiles the same engine, the composition there has been the bounded ratio since v0.6.5, and the
bridge already carried `RatioGuard`, `ColourStrength`, `ResidualLimit`, `ResidualFade`,
`Intensity`, `Structure`, `Skin`, `Tone` and `Scale`. Only the panel could not reach them.

- **The same fifteen controls, the same cascade, the same wording as the 64-bit panel.** Status
  runs down the right-hand column, the tags are gone, red and amber are the only marks a control
  carries, and **More settings** reveals the rest one tick at a time under the header it will
  appear beneath. The `HiddenShown` bits are the same bit numbers on both routes.
- **Five settings joined the wire**: `Style`, `StyleStrength`, `HiddenShown`, `DepthInverted` and
  `DepthNormalise`. The protocol is **v3**; the two sides are built and shipped together, and a
  mismatched pair is refused at the header rather than misread. `FeedEffect` deliberately did not
  join it -- the companion effect is read through ReShade's effect runtime, which lives in the
  game's process, while the network lives in the helper.
- **The helper reports its scale cap.** When one evaluation takes long enough to risk the display
  driver, the helper lowers the scale on its own; the panel now compares the network raster against
  what is really running instead of against the slider, so a capped-but-working configuration
  stops reading as "not applied yet". Moving the slider asks for the full scale again.
- **Export logs to desktop**, the button the 64-bit panel got, with this route's two logs: it
  collects `amd-nr-x86.log`, `amd-nr-x86-host.log`, `dlssnr_on_amd.log`, `ReShade.log` and
  `amd-nr.ini` into a dated folder. It looks in both directories, because the add-on writes beside
  itself and the helper writes beside the game.
- **A fresh 32-bit ini no longer writes `Skin=1`.** -1 is the engine's automatic and the value it
  boots with; writing 1 switched that off before anybody had touched a control, which is the bug
  the 64-bit default had until v0.6.5. Factory Defaults restores -1 too, and leaves the panel
  arrangement alone -- which controls are on screen is a preference, like the language and the
  hotkey.

## v0.6.5 - 2026-09-22 - AMD Neural Rendering

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
