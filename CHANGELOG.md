# Changelog

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
