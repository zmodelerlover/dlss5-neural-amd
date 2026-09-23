# Project handoff — 2026-09-14

This is the operational handoff for the work performed with the user on `dlss5-neural-amd`.
It is intended to let another developer or AI resume without reconstructing the long debugging
history from chat. Read this file first, then the repository README, changelog and x86 documents.

## 0. Errata, 2026-09-20

Four things below were true when this was written and are not true now. The rest of the
document stands, and `handoffs/PROJECT_HANDOFF_2026-09-20.md` is the current state.

| Where | It says | It is |
|---|---|---|
| §1, §2 | work is on `x86_testing`, uncommitted | all of it shipped: v0.5.0, then v0.5.3 for the pipelining. `master` is the state; that branch is gone |
| §3.1 | the pinned runtime is v0.2.17 | **v0.3.0** since v0.5.1. Every offset was re-derived against it, and v0.2.17 is now refused by hash |
| §3.3 | "Async mode is disabled on x86; same-frame inline behavior is the validated path" | pipelined presentation is the **default** on the bridge and the Timing control switches it live. §11 measured it at +16% to +41%. Inline composition, which is a different thing, is still forced on |
| §7 | `installer/` is a directory of this repository | it was removed in v0.5.1, as §11 says further down. Installing is AMD-NR ReShade Installer, in its own repository |

One more route exists that this document does not mention at all: **OpenGL**, shipped in v0.6.0.
`docs/opengl-route.md` is its record.

## 1. Read this before changing anything

- Work is on **`x86_testing`**, branched from `master` and not yet merged. An earlier branch of the
  same name was merged and deleted for v0.5.0; this is a new one. It carries the pipelined
  presentation work described in sections 11 onwards.
- Remote `origin` is `https://github.com/zmodelerlover/dlss5-neural-amd.git`.
- Every path in this document is relative: repository paths to the checkout root, game paths to
  wherever that game is installed. Keep a single checkout of this repository and work only in it.

**Updated later on 2026-09-14.** The work this handoff was written to preserve is committed,
merged and released as v0.5.0. The dirty tree the original text described was separated into these
commits, which are now in `master`:

| Commit | Contents |
|---|---|
| `b324d57` | D3D9 `Reset`/lost-device recovery and HRESULT diagnostics: the GTA IV Alt+Tab fix |
| `0346842` | experimental D3D8 preset through pinned d3d8to9, with `d3d8R.dll` chaining, licence and import script |
| `8515a57` | ignore `local-x86-mod-package/` |
| `5701dc0` | this handoff |
| `e6d4ddb` | ignore-rule audit: `release-v*/`, `*.zip`, installer manifests and backups, editor noise |
| `cfdd1af` | stop ignoring `tools/patch_runtime.py` and `installer/Cargo.lock`, which are not artifacts |
| `6b273e8` | opt-in stage probe behind `DLSS5_X86BRIDGE_TIMING=1` |
| `f84fc99` | arm the stage probe from the ini; subscribe `destroy_device`; settle the refcount warning |
| `c4a6743` | drop a banned game name from a comment, which the contract test forbids in bridge sources |
| `1e7532f` | pipelined presentation: post the frame, compose the previous present's answer |
| `67b528c` | give the overlay contract test's frontend stub the `async` member it now reads |

**In flight, uncommitted on `x86_testing`:** switching presentation mode from the overlay while the
game runs. Built, contract tests pass, installed in GTA IV, Half-Life 2 and Resident Evil 5, and
**not yet exercised in a running game** -- nobody has flipped the control with a game open.

`.claude/settings.json` is untracked and arrived with a local plugin install. Decide whether it is
shared configuration or personal noise before committing anything near it.

A feature goes on its own branch and reaches `master` through a PR, the way v0.5.0 did.


## 2. Last action and exact rollback state

The last work was pipelined presentation, in `1e7532f`, plus an uncommitted change that makes the
mode switchable while the game runs. Both are `x86_testing` only; `master` is unchanged and remains
the release.

To roll the feature back without touching anything else, set `Async=0` under `[dlss5]` in a game's
`dlss5-neural.ini`, or flip the overlay's Timing control. That restores same-frame presentation
exactly: the same code path runs, minus two clock reads. Nothing else in `1e7532f` changes
behaviour on its own.

An earlier experiment, a classic-D3D9 raster alignment policy, was tested after Silent Hill 3
showed a sharp 60-to-30 FPS transition between Resolution Scale 0.35 and 0.36. It reduced the 0.36
neural raster from the requested 691x389 to 683x384 when a dimension sat just above a 32-pixel
boundary, and added per-stage D3D9 timing logs.

**The transition it was chasing has since been explained, and it was not a rendering boundary at
all.** See section 4.2. Resolving it required no change to this project's code.

The user reported that it made the result worse, and it was fully removed from source. Do not
reintroduce any of these identifiers or behaviours without new evidence:

- `FrameFlagClassicD3D9`
- `EfficientClassicD3D9Scale`
- `x86bridge D3D9 timing avg`
- automatic trimming/quantization/alignment of the user's requested neural raster

The code was rebuilt after the removal. Native x86/x64 protocol and IPC tests passed, all 50
installer tests passed, and the integrated addon64 build passed.

Silent Hill 3 was restored to the exact binaries used before that experiment:

| File | Restored SHA-256 |
|---|---|
| `dlss5-neural.addon32` | `4A9401CAE8CC8573887915821C74E00E240E3B41040CC203C53FDE8E13F0DD82` |
| `dlss5-neural-host64.exe` | `29284B5F62C44C9C585BDFD2ED461FB4A45ED3A09593A29BEAD3613A3585C184` |
| existing PC Fix `d3d8.dll` | `4D35F4EE85C63FFE012731DC1A4E8EEE2D8D34EBF6384076044E79BE1A479F13` |

The recoverable copy is at:

`.dlss5-manual-backups\20260914-130458`, inside the Silent Hill 3 game directory.

The rebuild refreshed ignored local build/release outputs, but the game intentionally remains on
the restored pre-experiment binary pair until a better performance change is validated.

## 3. Project architecture learned during the work

### 3.1 Native x64 add-on

The normal `dlss5-neural.addon64` is a ReShade add-on that runs the AMD port of the DLSS neural
rendering network. It uses the pinned v0.2.17 runtime and pass-1 weights/runtime. Pass 2 and pass 3
DLLs are not used. The principal implementation is under `src/neural/`.

The add-on has D3D11, D3D12 and Vulkan paths. The single packaged x64 add-on includes the Vulkan
transport; there is no longer a separate Vulkan binary. Vulkan hooks stand down on processes that
do not expose the required imports.

All packaged game-facing binaries use the static MSVC runtime (`/MT`). This matters because some
games, especially Detroit: Become Human, ship old private copies of MSVC runtime DLLs beside the
executable. A `/MD` add-on can register in ReShade but fail during `DllMain` against those copies.

### 3.2 Vulkan route

The Vulkan route crosses the presented image into a private D3D12 device where the existing neural
engine runs. It requires device creation interception so the needed external-memory extensions are
enabled before `VkDevice` creation.

There are two discovery paths:

- normal ReShade/import-table path;
- dynamic device-discovery fallback using vendored MinHook for applications such as Red Dead
  Redemption 2.

Important behavior:

- the presentation queue must support graphics commands;
- an async-only present queue is rejected safely;
- private Vulkan discovery preserves both `DISABLE_VK_LAYER_reshade_1` and
  `DISABLE_VK_LAYER_reshade_2` to avoid recursive ReShade entry and to support DOOM's launcher;
- D3D12 crossing textures begin in `COPY_DEST`, matching the first operation;
- poisoned allocator/command-list slots are recreated after `Reset` or `Close` failure;
- swapchain teardown fully retires imported Vulkan images and D3D12 crossings, even if dimensions
  and format are unchanged;
- fast-path reuse requires all imported images and D3D12 resources to still be live;
- process detach must not remove Vulkan hooks while Windows holds the loader lock.

DOOM Eternal normally presents from an async-only queue. `r_presentFromAsync "0"` tells the game
to present from a graphics-capable queue that the bridge can use. This is what the earlier
`presentFromAsync` discussion referred to.

### 3.3 Experimental x86 bridge

A 32-bit game cannot load the 64-bit HIP/runtime stack directly. The x86 implementation is split:

- `dlss5-neural.addon32`: 32-bit ReShade frontend loaded by the game;
- `dlss5-neural-host64.exe`: 64-bit helper containing/reusing the neural engine;
- a fixed-width named-pipe protocol plus shared/staged frame resources between them.

The frontend and host must always be updated as a pair. Protocol v1 is intentionally rejected by
protocol v2. The pipe validates the client PID/process identity, generation, frame sequence,
adapter LUID and settings revisions. IPC and startup waits are bounded; an unresponsive helper
falls back to the original game image instead of hanging Present indefinitely.

The x86 UI is adapted in `src/x86bridge/overlay32.inc`. It does not perform file, IPC, host launch
or GPU work from the overlay callback. Control synchronization occurs from Present. Async mode is
disabled on x86; same-frame inline behavior is the validated path.

API routes:

| Game API | x86 route |
|---|---|
| D3D11 | native D3D11 frontend -> x64 host |
| D3D9Ex | D3D9 -> shared D3D9/D3D11 GPU textures -> x64 host |
| classic D3D9 | D3D9 -> bounded CPU-compatible readback/upload staging -> D3D11 -> x64 host |
| D3D8 | d3d8to9 -> native D3D9 frontend -> one of the D3D9 paths above |

Classic D3D9 is much more expensive than D3D9Ex because a full frame crosses CPU-visible staging
in each direction. D3D8 support is a compatibility layer, not a second neural renderer.

### 3.4 Depth and motion availability

| Route | Colour | Game depth | Motion |
|---|---|---|---|
| x64 D3D11 | yes | yes, candidate captured and converted to `R32_FLOAT` | game velocity target when found, otherwise estimated |
| x64 D3D12 | yes | no reliable game depth | estimated from colour |
| x64 Vulkan | yes | no | estimated from colour |
| x86 D3D11 | yes | candidate capture exists through the frontend | game candidate when available |
| x86 D3D9/D3D8 | yes | no, colour-only transport | host estimation |

Enabling `Depth=1` cannot manufacture a missing Vulkan or D3D9 depth buffer. Flat depth and zero
motion in a menu are not proof of failure; guide contents must be observed during real gameplay.
Reliable Vulkan depth and D3D9 depth/motion discovery remain open research tasks.

## 4. Fixes and lessons already validated

### 4.1 Vulkan lifecycle and compatibility

- Fixed the Detroit registration-without-panel problem by statically linking the CRT.
- Validated present queues and command lists before use so unsupported async queues skip safely.
- Fixed immediate activation crashes caused by invalid/null Vulkan command-list paths.
- Fixed disable/re-enable and Alt+Tab cases that left stale imported images or poisoned work slots.
- Added complete swapchain resource retirement and rebuild.
- Added the dynamic Vulkan device fallback used by Red Dead Redemption 2.
- Fixed CI `framecheck` linkage by compiling the vendored MinHook sources into that fixture too.
- The Eden emulator report where the panel disappeared was not a regression: Windows Defender had
  deleted/quarantined the addon. Restoring the file made the mod work.

### 4.2 Resolution Scale and VRAM

Dragging Resolution Scale used to apply every intermediate slider value, repeatedly rebuilding the
network raster and causing unnecessary VRAM growth. The accepted mitigation commits the new scale
only after slider editing ends and retires completed work slots safely.

Repeated completed scale changes can still raise the VRAM residency reported by the driver. The
remaining growth appears consistent with AMD HIP/runtime or driver allocation caching. An
experimental forced runtime purge using an undocumented internal function did not solve it and was
not retained because it added stability risk.

Do not automatically quantize or silently alter the user's scale. The Silent Hill 3 32-pixel tile
alignment experiment worsened the result and was reverted.

**Resolved 2026-09-14: the 0.35/0.36 transition was the game's frame-rate cap, not a raster
boundary.** Silent Hill 3's PC Fix was running a hard 60 FPS cap (`Silent_Hill_3_PC_Fix.ini`,
`FPSMode` 1 or 2). Under a hard cap, missing the 16.67 ms deadline does not cost a proportional
amount of frame rate, it costs the next divisor: 60 becomes a locked 30. Scale 0.36 asks for 5.8%
more pixels than 0.35, 268,799 against 254,016, which is enough to cross that deadline and nothing
more. Setting `FPSMode = 4` (unlocked) normalised the frame rate with no change to this project's
code at all.

Two consequences:

- There is no 32-pixel boundary and nothing special about 0.36. The alignment experiment was
  solving a problem that did not exist, which is why it cost image quality and bought nothing.
  Do not attempt raster alignment, quantization or trimming again on this evidence.
- `FPSMode = 4` disables the PC Fix's own `LimitFPSInStoreroom`, which requires `FPSMode` 1 or 2.
  That patch locks the hospital storeroom (Mirror Room) to 30 FPS so its visual and audio effects
  play correctly, so uncapping to fix the bridge's frame budget breaks a later scene. `FPSMode = 1`
  (ThirteenAG's FPS patch) is worth testing as the setting that keeps both.

The cost itself remains real: the classic D3D9 route pays a fixed CPU round trip that does not
shrink with Resolution Scale. Measure before changing behaviour, in this order:

- game frame pacing and any game or PC Fix frame-rate cap -- **check this first**; it explained the
  only performance cliff ever reported here;
- classic-D3D9 CPU readback/upload cost;
- neural inference cost;
- HIP/driver allocation caching.

Section 4.5 is the tool for the middle two.

### 4.3 D3D9 Reset and Alt+Tab

GTA IV exposed a crash in exclusive-fullscreen Alt+Tab. ReShade emits `destroy_swapchain` while
`IDirect3DDevice9::Reset` is already in progress. Submitting an event query, waiting for D3D9,
waiting for D3D11 or performing IPC from that callback can re-enter `amdxx32.dll` during reset and
crash.

The current uncommitted frontend fix:

- releases local/default-pool D3D9 resources immediately during resize/reset;
- performs no `DropRemote`, query, D3D11 wait, `ClearState` or `Flush` inside that reset branch;
- defers remote generation retirement until the next stable Present/`Bridge::Ensure`;
- returns HRESULTs from D3D9 staging operations instead of collapsing all failures to `false`;
- treats `D3DERR_DEVICELOST`/`D3DERR_DEVICENOTRESET` as a transient reset frame, keeps the host
  connected and resets history on recovery;
- logs other HRESULT failures precisely and falls back safely.

This removed the GTA IV Alt+Tab crash and the later retry/error state observed with NR enabled.
Half-Life 2 continued to work after installing the same build.

### 4.4 Installer safety

The x86 installer is additive and fail-closed:

- rejects PE32+ targets;
- validates SHA-256 for the bridge, host, runtime, weights, ReShade and optional d3d8to9 payload;
- never runs a ReShade installer executable;
- follows ReShade `[INSTALL] BasePath` only when it resolves inside the selected game directory;
- backs up conflicts under `.dlss5-x86bridge-backups/`;
- records ownership/hashes/backups in `dlss5-x86bridge.install.json`;
- preserves user configuration and files modified after installation;
- restores unchanged backups during uninstall;
- fails closed on unknown D3D8 wrappers rather than overwriting them.

The supported x86 ReShade sidecar is ReShade 6.8.0.2156 Full Add-on Support with its pinned hash.
The installer writes that payload as `dxgi.dll` for D3D11 or `d3d9.dll` for D3D9/D3D8.

### 4.5 Measuring the x86 route

`DLSS5_X86BRIDGE_TIMING=1` turns on an opt-in stage probe in the x86 frontend (commit `6b273e8`).
It is off by default; the frontend's startup line reports `probe=on`/`probe=off` so a log says
which it was. Every 120 completed frames it averages one line splitting the bridge into
`input+prepare`, `host` and `output`, and names the staging path measured.

The probe measures and never participates. It issues no query, flush or wait of its own: all three
boundaries are synchronisations the frame already performs, so an enabled probe measures the same
frame that would have run without it. **Preserve that property.** A wait added on this path would
land inside the `IDirect3DDevice9::Reset` window that section 4.3 exists to keep clear. The
contract test in `tools/test-x86bridge.py` pins it and also guards that the reverted experiment's
identifiers stay absent.

**Measured on Silent Hill 3 through the real D3D8 route**, 2,160 frames at 1920x1080 with no
failures, across 18 windows of 120 frames:

| | min | max | mean | spread |
|---|---|---|---|---|
| `host` | 9.93 ms | 40.76 ms | 12.98 ms | 30.83 ms |
| transport (`input+prepare` + `output`) | 5.30 ms | 5.82 ms | 5.56 ms | **0.52 ms** |

That is the finding: **`host` swung 4.1x while transport moved half a millisecond.** The transport
cost is independent of what the network costs. The swing came from warm-up at scale 1.00 settling
into steady state at 960x540, which covered a far wider range than deliberately stepping through
Resolution Scale values would have.

Steady state at scale 0.50: `host` 9.97 ms, transport 5.55 ms, total about 15.5 ms.

So the classic path has a floor. Transport is roughly 36% of a 60 FPS budget spent only moving
pixels, it never shrinks, and lowering Resolution Scale does not touch it -- even with the network
free, this route cannot go below about 5.5 ms per frame. Half-Life 2 reaches `shared GPU staging`
and does not pay it. That is the measured case for D3D9Ex promotion over saving neural pixels.

How much of the 5.5 ms D3D9Ex actually removes is still unmeasured; the probe labels the staging
path on its own line, so a promoted Silent Hill 3 answers it directly.

### 4.6 The device reference held past the last callback

ReShade reported `Reference count for IDirect3DDevice9 ... is inconsistent! Leaking resources` at
normal process exit on Silent Hill 3 and GTA IV, and not on Half-Life 2. It was recorded here as
unconfirmed, with the classic CPU staging path as the suspect. **Both of those were wrong, and the
evidence to settle it was already in the logs.**

`Log()` calls `fflush` on every line, so a line that is not in a log is code that did not run rather
than a buffer that was lost. `x86bridge retiring swapchain resize=0` is absent from Silent Hill 3
and GTA IV and present in Half-Life 2. The two games that leak are the two that never delivered
`destroy_swapchain`, and that is the whole correlation -- the staging path had nothing to do with it.

What leaks is not `readback9` and `upload9`. It is the device itself. `ComPtr` AddRefs the game's own
device on both routes: `g.game9` on D3D9, and on D3D11 `g.game11` is the game's device rather than a
private one. Every D3D9 staging texture and surface made from it holds a reference as well. All of
them were released in exactly one place, the `resize=0` branch of `OnDestroy`, and a game that
leaves through `ExitProcess` rather than shutting its renderer down never reaches it.

**Subscribing to `destroy_device` does not fix it, and nothing in an add-on can.** The handler is in
place and is correct, but it never runs. Measured 2026-09-16 with an unconditional `Log()` at the
very top of the handler, before the identity check, in a build proved live by `probe=on` in the same
log: **it printed nothing, in GTA IV or in Half-Life 2**, while both runs ended with the warning.
ReShade simply does not deliver the event when a game leaves through `ExitProcess`. Neither does it
deliver `destroy_swapchain` or `destroy_effect_runtime` on that path.

ReShade's own log shows why there is no workaround. At exit it releases the device, checks, and warns
**while the add-on is still loaded and still holding the reference**; it unloads the add-on
afterwards:

```
13:17:36:980  WARN  | Reference count for IDirect3DDevice9 object 240DAF08 is inconsistent! ...
13:17:38:861  INFO  | Unregistered add-on "dlss5 neural x86 bridge".
```

Two seconds separate them, in that order. There is no add-on-side moment between the last frame and
that check, and `DLL_PROCESS_DETACH` is later still, so it cannot help even setting aside the loader
lock. Half-Life 2 behaves identically once it exits the same way -- the earlier run where it stayed
clean was a clean renderer shutdown, not a property of D3D9Ex.

**This is cosmetic and should be left alone.** The warning is written as the process is dying and the
OS reclaims every handle regardless; nothing survives the exit and no session is affected. The
handler stays because it is free and correct for any game that does shut its renderer down, but it
should not be described as a fix, and the warning should not be chased further from this side. If it
is ever worth removing, that is an upstream request for ReShade to emit `destroy_device` on this
path.

The contract test pins the handler's shape: that the event is subscribed, that it releases both
devices, that it compares device identity before touching anything, and that it contains no
`FlushAndWait`, `ClearState`, `Flush`, `CopyResource` or IPC.

## 5. D3D8 and Silent Hill 3 details

The selected design is:

`SilentHill3.exe -> existing PC Fix d3d8.dll -> d3d8R.dll (d3d8to9) -> d3d9.dll (ReShade x86) -> dlss5-neural.addon32 -> dlss5-neural-host64.exe`

Why `d3d8R.dll`:

- Silent Hill 3 already has a PC Fix as `d3d8.dll`;
- inspection found that this wrapper explicitly searches for/forwards to `d3d8R.dll`;
- replacing it would break the PC Fix;
- the installer now recognizes only an explicit ASCII or UTF-16 `d3d8R.dll` marker and otherwise
  refuses to replace an unknown wrapper.

Production installer expectations:

- official d3d8to9 version: v1.15.1;
- source commit: `65870f2302e9c496cd6d873d6095961d5c777668`;
- official release asset SHA-256:
  `ab6bf7a9a9f4b3e66a75ca038d8d10289c88acbfe8d52c3b5a8a9a259cb26cd5`;
- imported private filename: `release/files/d3d8to9.dll`;
- import script: `tools/import-d3d8to9.ps1`;
- license: `docs/third-party/d3d8to9-LICENSE.md`;
- d3d8to9 may require the legacy `d3dx9_43.dll` DirectX runtime.

**The official release asset has since been imported.** It was fetched from the upstream release,
verified as a 124,416-byte PE32/i386 image whose SHA-256 matches the pinned constant, and imported
through `tools/import-d3d8to9.ps1`. It now sits at `release/files/d3d8to9.dll`, which is git-ignored:
the payload stays out of the repository and out of public packages, and only the BSD-2-Clause
attribution is tracked.

The earlier manual Silent Hill 3 test used a d3d8to9 compiled locally from the pinned source commit.
That local build has SHA-256
`EE9B4916304592A31F0882F339BCBEAC7133439A297FBD5274E503C0147D209E`, which intentionally does not
satisfy the production installer's official-release hash, and it is still what the game directory
carries as `d3d8R.dll`. Installing through the installer replaces it with the official binary. Do
not weaken the production hash check to accept arbitrary local builds.

**Installed through the installer and verified live.** The D3D8 preset was run against the real
game with the official translator present. A read-only `plan()` rehearsal first confirmed the file
map, then the install ran and was checked by hash:

- the PC Fix `d3d8.dll` was left byte-identical, chosen by the chaining rule reading the real
  wrapper rather than a synthetic one;
- the official translator replaced the locally built `d3d8R.dll`;
- ReShade, runtime and weights reported `IDENTICAL` and were not rewritten;
- `dlss5-neural.ini` and `ReShade.ini` were preserved;
- the manifest records the replaced files as owned, including the pre-experiment pair
  `4a9401ca`/`29284b5f`, so that rollback state is now held by the installer's own backup scheme.

The game then ran 2,160 frames with `LUID MATCH`, `probe=on` and no failures, confirming the whole
chain `sh3.exe -> PC Fix d3d8.dll -> official d3d8R.dll -> ReShade d3d9.dll -> addon32 -> host64`
in the configuration a user would actually receive. Note the installer executable is `wWinMain`
only; its Install button calls `app.install(target, preset)` and that call is what was exercised,
so the GUI itself remains an unvalidated manual test.

Silent Hill 3 live result before the rejected performance experiment:

- ReShade loaded through `d3d9.dll`;
- the addon registered and the panel appeared;
- more than 10,000 frames completed with `result=1 same_frame=1` and no host errors/timeouts;
- native D3D9 used the classic CPU-compatible staging path, not D3D9Ex shared handles;
- at 1920x1080, scale 0.35 requested 672x378 and ran near 60 FPS;
- scale 0.36 requested 691x389 and the game dropped to a locked 30 FPS, later traced to the PC Fix
  frame-rate cap rather than to anything about the raster; see 4.2;
- the attempted raster trimming made the actual result worse and was reverted;
- one startup `IDirect3DDevice9::Reset` returned `D3DERR_INVALIDCALL`, immediately retried and
  recovered;
- ReShade reported an inconsistent D3D9 reference count at normal process exit; no crash/minidump
  was associated with it.

## 6. Live test matrix

| Application | API | Result and key lesson |
|---|---|---|
| Detroit: Become Human | Vulkan x64 | Stable after `/MT` and Vulkan lifecycle fixes; 9,240-frame validation, repeated toggles and two rebuilds, no skips. Later regression check also passed. |
| DOOM Eternal | Vulkan x64 | Works with proper ReShade Full Add-on installation and graphics present queue; use `r_presentFromAsync "0"`; 3,600-frame validation, repeated Alt+Tab, one recovered skip. |
| Red Dead Redemption 2 | Vulkan x64 | Initially panel appeared but effect did not activate; dynamic Vulkan device fallback/MinHook route fixed it. User confirmed correct operation. |
| Eden Nintendo Switch emulator | Vulkan x64 | Works. Missing panel was Windows Defender quarantining/deleting the addon, not a code regression. |
| Half-Life 2 | D3D9 x86 | Works through native D3D9 bridge. ReShade BasePath points to `bin`; binaries/logs that matter are in `Half-Life 2\bin`, not only the root. Host64 packaging/availability was corrected. Revalidated on the committed tree: 21,600 frames, every frame `result=1`, no faults, one reset handled. Uses `shared GPU staging`, so it exercises the D3D9Ex path the other two titles do not. |
| GTA IV | D3D9 x86 | Works after Reset/Alt+Tab lifecycle fix. Avoid waits/queries/IPC during `IDirect3DDevice9::Reset`. Transient device-lost frames must not permanently fault the bridge. Revalidated on the committed tree: 15,480 frames, every frame `result=1`, no faults, 20 disable/enable cycles, one reset handled by the deferred path. Classic CPU-compatible staging. |
| Silent Hill 3 | D3D8 x86 | Works through PC Fix -> `d3d8R.dll` d3d8to9 -> ReShade D3D9 -> x86 bridge. The reported 0.35/0.36 cliff was the PC Fix frame-rate cap, not the bridge: unlocking it (`FPSMode = 4`) normalised the frame rate with no code change. Attempted raster alignment was rejected and reverted. See 4.2, including what `FPSMode = 4` costs in the Mirror Room. Installed through the installer with the official pinned translator and revalidated: 2,160 frames, no failures, PC Fix preserved. Stage-probe figures in 4.5. |
| NFS 2015 | D3D11 x64 | Existing documented validation: 1,205 frames, one skip, no failures. |
| GTA V Enhanced | D3D12 x64 | Existing documented validation: 23,663 frames, no failures; demonstrated stale-residual trail on skipped frames, later fixed by outputting the untouched game frame on skips. |
| RPCS3 | Vulkan x64 | Existing documented validation: full bridge round trip. Static `vkCreateDevice` import is compatible. |
| PCSX2 Vulkan | Vulkan x64 | Structurally incompatible with current import patch because it resolves Vulkan dynamically through `vkGetInstanceProcAddr`; D3D11/D3D12 remain the useful PCSX2 routes. |

## 7. Complete directory inventory used in this work

### Repository layout

Paths are relative to the repository root.

| Directory | Purpose |
|---|---|
| `src/neural` | Main x64 neural addon and Vulkan/D3D routes. |
| `src/x86bridge` | x86 frontend, x64 host, protocol, overlay and tests. |
| `src/framecheck` | Integrated frame/lifecycle test fixture; must link MinHook. |
| `src/probe` | API/resource diagnostic probe. |
| `src/vkbridge` | Vulkan bridge components. |
| `src/vkprobe` | Vulkan diagnostics. |
| `src/vkshared` | Vulkan shared code/resources. |
| `installer` | The installer, Rust, covering both the x64 routes and the x86 bridge. The separate C++ `installer-x86` it replaced was retired; see `docs/installer-merge.md`. |
| `external/reshade` | Vendored ReShade headers. |
| `external/minhook` | Vendored MinHook source/license. |
| `tools` | Build, validation, import and packaging scripts. |
| `docs` | Design/release documentation. |
| `docs/third-party` | Third-party attributions, including d3d8to9. |
| `handoffs` | This handoff and future continuity notes. |

These are produced locally and are all git-ignored; none of them ship in a clone.

| Directory | Purpose |
|---|---|
| `build` | x64 build outputs. |
| `build-x86bridge` | x86/x64 bridge build and test outputs. |
| `release` | Current local release staging and private sidecars. |
| `release/files` | Addon/host/runtime/weights/ReShade/d3d8to9 payload staging. |
| `release-v*` | Prior release staging kept across a version bump. |
| `local-x86-mod-package` | Manual x86 test package assembled by hand. |
| `diagnostic-logs-backup` | Backed-up game logs kept as evidence. |
| `dlss5-runtime-v0.2.17` | Pinned runtime source/payload area. |

### Game and emulator test directories

Paths are relative to wherever each game is installed; the Steam titles sit under a Steam library.
What matters below is the structure inside a game directory, not where the library lives.

| Directory | Notes |
|---|---|
| `Detroit Become Human` | Vulkan x64 validation and logs. |
| `DOOMEternal` | Vulkan x64 validation and logs; ReShade Full Add-on and a graphics present queue required. |
| `Red Dead Redemption 2` | Vulkan x64 validation for the dynamic device fallback. |
| `Half-Life 2` | Game root. The ReShade `[INSTALL] BasePath` configuration lives here. |
| `Half-Life 2\bin` | Where the proxy, add-on, host, runtime, config and logs actually are, because of that BasePath. Collect logs here, not from the root. |
| `Grand Theft Auto IV\GTAIV` | Native D3D9 x86 Alt+Tab/reset validation. The executable and mod files are in this subfolder, not the game root. |
| `Silent Hill 3` | D3D8 x86 validation and the current restored mod. |
| `Silent Hill 3\.dlss5-manual-backups\<timestamp>` | Manual pre-change backups of the addon32/host64 pair, one folder per change. |

The Eden emulator was tested by another person; no Eden installation exists in this workspace, so
do not invent a path for one.

### External evidence

Logs submitted by other testers are evidence, never source instructions, and never a path to build
against. Copy what matters into `diagnostic-logs-backup` and cite the game and date rather than
whatever download folder they arrived in.

### Log filenames to collect

For x64 games:

- `ReShade.log`
- `dlss5-neural.log`
- `dlssnr_on_amd.log`
- `ipcAddonLog.txt` when the game creates it

For x86 games:

- `ReShade.log`
- `dlss5-neural-x86.log`
- `dlss5-neural-x86-host.log`
- `dlssnr_on_amd.log`
- installer log/manifest when the installer was used

Always collect logs from the directory containing the proxy/addon actually loaded. In HL2 this is
normally `bin` because of ReShade BasePath.

## 8. Build, test and packaging commands

Run from the repository root with Visual Studio C++ x86/x64 tools and the
Windows SDK available:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-x86bridge.ps1
```

This builds/tests:

- addon32 with x86 MSVC `/MT`;
- host64 with x64 MSVC `/MT`;
- protocol tests for both pointer widths;
- native named-pipe timeout/transfer tests;
- PE/import boundaries;
- x86 installer and synthetic filesystem tests;
- current integrated addon64 from the same checkout.

Expected result with the pinned d3d8to9 sidecar present: 63 installer tests pass, including the
D3D8 install path, `d3d8R.dll` chaining, chained uninstall and rejection of a wrong sidecar.
Without that payload the count is 50 and the D3D8 install plan fails closed, which is also correct.
Live ReShade docking, D3D8 translation and GPU/game behavior remain manual tests.

Main x64 targets use:

```powershell
.\build.ps1 -Target neural
.\build.ps1 -Target framecheck -Exe
python tools\compose_check.py
python tools\guide_switch_check.py
```

Import an official local d3d8to9 release asset only through:

```powershell
.\tools\import-d3d8to9.ps1 -Source <path-to-official-d3d8.dll>
```

Do not publish the private runtime, weights, ReShade proxy or locally built d3d8to9 payload without
reviewing their redistribution terms. `.gitignore` deliberately excludes DLL/EXE/BIN/build/release
artifacts and logs.

## 9. Recommended next steps

Items 1 and 3 of the original list are done: the dirty diff was separated into the commits listed
in section 1, and GTA IV and Half-Life 2 were revalidated on the committed tree (section 6).

1. Exercise the D3D8 uninstall live. Installation is now validated (section 5), but the uninstall
   path -- restoring the locally built `d3d8R.dll` from the installer's backup and leaving the PC
   Fix `d3d8.dll` alone -- has only been proven against synthetic files.
2. Investigate D3D9Ex promotion/shared-handle feasibility for true D3D8/D3D9 games. This is now the
   highest-value performance item: section 4.5 puts about 6 ms of fixed CPU round trip on the
   classic path, Half-Life 2 already proves the shared path works, and no amount of neural-pixel
   shaving reaches that cost. Compatibility must still be proven, and for D3D8 it depends on what
   d3d8to9 creates.
3. Quantify before optimising. With `DLSS5_X86BRIDGE_TIMING=1` and no frame-rate cap in the way,
   run one title at several Resolution Scales: `input+prepare` and `output` should stay flat while
   only `host` grows. That turns the D3D9Ex decision into a number.
4. Do not silently change the user's scale or raster. Section 4.2 is the record of why.
5. Confirm or dismiss the D3D9 reference-count observation in section 4.6.
6. Everything that does not block a release now lives in section 11, so it stays visible instead of
   being rediscovered.
7. Before committing, run `git diff --check`, the full x86 build above, the main x64 checks and
   `cargo test` in `installer/`. Put feature work on its own branch and merge it through a PR.

## 10. Primary repository references

- `README.md` — user-facing architecture, installation and current supported routes.
- `CHANGELOG.md` — detailed history and measured rationale.
- `docs/x86bridge.md` — protocol v2, ownership, control and frame flow.
- `docs/x86bridge-install.md` — installer safety, payload rules and manual regression plan.
- `docs/installer-merge.md` — decisions for folding the two installers into one; not implemented.
- `build-x86bridge.ps1` — authoritative integrated x86 build/test entry point.
- `src/x86bridge/frontend32.cpp` — native x86 capture/staging/lifecycle code.
- `src/x86bridge/host64.cpp` — x64 helper and engine integration.
- `src/neural/vk_route.inc` — native Vulkan transport/discovery/lifecycle.

## 11. Roadmap: known work that does not block a release

None of this stops a game from running. It is collected here so it stays visible instead of being
rediscovered, and ordered by what the evidence says is worth doing first rather than by how easy it
would be.

### The classic D3D9 transport cost, and why D3D9Ex does not fix it

**The cost is real and measured.** Section 4.5 puts the transport at 5.55 ms a frame on the classic
D3D9 path, independent of the neural cost: `host` swung 4.1x across one run while transport moved
half a millisecond. That is roughly 36% of a 60 FPS budget spent only moving pixels, and it does not
shrink when Resolution Scale drops. Half-Life 2 does not pay it, because Source creates a D3D9Ex
device and the bridge takes the shared-GPU path there.

**Promoting the game's device to D3D9Ex was the obvious fix. It does not work.** Measured on this
machine, RX 9070 XT, with `tools/d3d9ex-probe.cpp`:

| | plain D3D9 | D3D9Ex |
|---|---|---|
| `CreateTexture` `D3DPOOL_MANAGED` | OK | **`D3DERR_INVALIDCALL`** |
| `CreateTexture` `D3DPOOL_DEFAULT` | OK | OK |
| `LockRect` on a DEFAULT texture | `D3DERR_INVALIDCALL` | `D3DERR_INVALIDCALL` |
| `D3DPOOL_DEFAULT` + `D3DUSAGE_DYNAMIC`, then `LockRect` | OK | OK |
| `CreateTexture` with a shared handle | **`D3DERR_INVALIDCALL`** | OK |
| `QueryInterface` for `IDirect3DDevice9Ex` | `E_NOINTERFACE` | OK |

Both halves are confirmed. A plain D3D9 device cannot produce the shared texture the fast path
needs, so there is no way to reach it without promotion. And a D3D9Ex device refuses
`D3DPOOL_MANAGED`, which is what a legacy D3D9 or translated D3D8 game creates nearly all of its
textures in.

So promotion means intercepting device creation *and* every resource creation, translating MANAGED
to DEFAULT, and dealing with what that breaks. The probe shows the shape of that: a DEFAULT texture
cannot be locked, so every translated texture the game intends to lock has to become DYNAMIC, which
changes where the driver places it and what it costs to sample. That is a resource-translation
layer with a per-resource policy — what dgVoodoo and DXVK are — and this project deliberately
removed its dgVoodoo dependency.

**Do not attempt blanket promotion.** It fails in the games it is meant to help, and it fails after
the install rather than at it.

### Why lowering Resolution Scale drops GPU load without raising FPS

Measured in GTA IV, 2026-09-16, classic D3D9 staging, one session that walked Resolution Scale
1.00 -> 0.50 -> 0.25 -> 0.50 -> 1.00. The user's report was that FPS did not move while GPU
utilisation fell. Both halves of that are explained, and the scale control is not at fault.

**The scale change reaches the network.** `dlssnr_on_amd.log` rebuilds its staging at each step --
`staging ready: colour 1920x1080`, then `960x540`, then `480x270` -- and the per-job cost follows:

| Network raster | network, avg of 200 jobs | game queue `spin waiting on us` |
|---|---|---|
| 1920x1080 | 26.2 - 29.5 ms (41.6 late, heavy scene) | matches within 0.3 ms |
| 960x540 | 16.6 - 16.7 ms | 16.8 ms |
| 480x270 | ~10.0 ms | (segment too short for an average) |

**The game is blocked for the whole of it.** In every single timing line `spin waiting on us` equals
`network` to within 0.3 ms. Nothing overlaps: the neural cost is fully serialized into the frame, so
frame time can never fall below it. That is the same serialization the async item further up
describes, seen from the host side.

**But the transport does not scale, and it is measured.** The bridge log names one raster for the
whole session and never rebuilds: `bridge: private staging 1920x1080`. Resolution Scale resizes what
the network chews on; it does not resize the CPU round trip, which keeps moving a full 1920x1080
frame down and back every frame. A second run with the probe armed shows exactly that -- six
averages of 120 frames, with `host` swinging six-fold while the two transport stages do not move:

| input+prepare | host | output | bridge total |
|---|---|---|---|
| 5.83 ms | 24.18 ms | 2.92 ms | 32.93 ms |
| 5.16 ms | 18.72 ms | 2.92 ms | 26.79 ms |
| 5.37 ms | **10.04 ms** | 2.92 ms | 18.33 ms |
| 5.40 ms | 21.92 ms | 2.95 ms | 30.27 ms |
| 5.67 ms | **59.59 ms** | 3.04 ms | 68.29 ms |
| 5.93 ms | 45.17 ms | 3.10 ms | 54.20 ms |

`input+prepare` stays inside 5.16-5.93 and `output` inside 2.92-3.10 across all of it. **Neither
follows the network, so no Resolution Scale setting reaches either.** In that session they summed to
8.3-9.0 ms. A later session measured 6.0 ms in the same game, so do not quote a single figure: see
"What the game costs on its own", which separates the part that is genuinely fixed from the part
that tracks the game's GPU load. The 5.55 ms in section 4.5 is a different game and does not apply
here either.

The numbers that survived re-measurement, and what they imply for the ceiling, are in the next
section.

**The first explanation offered here was wrong, and the measurement disproves it.** This section
originally argued that observed FPS did not rise because GTA IV is CPU-bound and carries its own
limiter. The period probe says otherwise: with the effect off, standing still, the game runs its own
frame in **7 ms, about 140 FPS**. It is nowhere near a limiter of its own.

**The real answer came from a scale sweep with the probe armed**, 2026-09-16, standing still outside
the starting apartment, effect toggled between steps:

| Resolution Scale | frame | FPS | network | bridge total | frame outside the bridge |
|---|---|---|---|---|---|
| 1.00, 1920x1080 | 40.46 ms | 24.7 | 27.50 ms | 33.55 ms | 6.91 ms |
| 0.50, 960x540 | 29.67 ms | 33.7 | 16.84 ms | 23.88 ms | 5.79 ms |
| 0.25, 480x270 | 29.56 ms | 33.8 | 9.98 ms | 17.85 ms | 11.71 ms |
| 0.25, later | 26.38 ms | 37.9 | 6.44 ms | 14.36 ms | 12.02 ms |

**Scale works, down to a point, and then stops.** 1.00 to 0.50 is a real 24.7 to 33.7 FPS. From 0.50
to 0.25 the network gives up 6.9 ms and **the frame does not move at all** -- 29.67 to 29.56. That is
the reported complaint, captured with instrumentation.

**Where the 6.9 ms went is in the last column.** The frame outside the bridge doubles, 5.79 to 11.71
ms, exactly as the network shrinks. The `enabled=0` windows on either side read 14.7-14.8 ms, so by
that point in the session the game's own frame had grown from 7 ms to about 14.8.

The mechanism is that **the game's GPU work already overlaps the network wait**. Its draw calls were
submitted before `Present`, so they run while the CPU sits in the IPC round trip. With the network
at 16.84 ms there is enough wait to hide roughly 9 ms of the game's own work. At 9.98 ms there is
not, and the rest becomes exposed. Once the network drops below what the game needs anyway,
shrinking it further buys nothing, because the game is the long pole.

So the control is not broken and the scale change is not being ignored. It stops paying at the point
where the network stops being the longest term, and on this hardware in this scene that is around
scale 0.50.

Two limits on this run: the game's own cost drifted from 7 ms to about 14.8 ms across the session,
so the rows are not perfectly comparable; and the last row's network fell to 6.44 ms at the same
480x270 raster as the row above, which is unexplained.

**The practical consequence.** On the classic D3D9 path, Resolution Scale has a hard floor of
roughly 5.5 ms that it cannot reach, and past the point where the network drops under the game's
own frame time it buys image quality back for nothing. Half-Life 2 does not have this floor,
because D3D9Ex puts it on the shared path. This is a further argument for pipelining over any
further tuning of the scale control.

**Arming the probe in this game needs the ini, not the environment variable.** GTA IV re-launches
itself through its own launcher, so the process that loads the add-on inherits nothing from whoever
ran `tools\run-with-timing.cmd`, and the first log line reads `probe=off` however you start it. The
frontend now also accepts `Timing=1` under `[dlss5]` in `dlss5-neural.ini`, which travels with the
install and does not care how the game was started. The environment variable still works where it
already worked.


### What the game costs on its own, and what pipelining would buy

The stage probe says what the bridge costs but not what the game costs, and the pipelining estimate
needs both. The frontend now also samples the present-to-present period, before the effect's
early-outs, so it keeps measuring while the effect is off. Toggling the effect in a fixed scene
supplies the missing term.

Measured 2026-09-16 in GTA IV, standing still outside the starting apartment, scale 1.00. Adjacent
windows, effect off then on:

| | present period | bridge total |
|---|---|---|
| effect off | 7.05 - 7.19 ms (~140 FPS) | -- |
| effect on | 40.30 - 40.72 ms (~24.7 FPS) | 33.44 - 33.58 ms |

The two agree: 7.05 measured off, plus 33.5 of bridge, predicts 40.6, and 40.5 was measured. The
model `frame = game + transport + network` holds, with **game 7.05 ms, transport 6.02 ms
(input+prepare 3.09 + output 2.93), network 27.52 ms**.

**Transport is not one fixed number.** `output` was 2.93 here and 2.92-3.10 in the earlier session,
so the return leg really is fixed. `input+prepare` was 3.09 here against 5.16-5.93 earlier, in the
same game at the same raster. It covers the D3D11 drain before the request, so part of what it
charges is the game's own outstanding GPU work being waited on, not transfer. That matters twice:
quoting a single transport figure is wrong, and the variable part is work pipelining would also
hide.

**A projection was made here from the sweep, and the live A/B disproved it.** It claimed +47% at
scale 1.00 rising to +77% lower down, by replacing a serial frame with
`max(everything but the network, the network)`. That formula assumes the rest of the frame overlaps
the network perfectly once the wait is moved. **It does not, and worse, the same-frame path was
already overlapping most of it**, so the projection counted the same benefit twice. The measured
result is in "What pipelining actually did" below. Do not reuse the formula.

**It would also make the scale control behave.** Pipelined, the network is the only term Resolution
Scale moves and it stays the binding one for longer, so lowering it keeps paying further down than
it does today.

**The costs are unchanged and are not small**: one frame of latency, and the correction computed
at the measured 62 FPS, about 16 ms. **The cost is latency and nothing else**, and the image was
checked in both modes in all three measured games: identical.

**That was predicted wrongly here several times, and the code says why.** The warnings above about
ghosting and smearing describe a residual from one frame being applied to a different one. This
implementation does not do that. `DownloadD3D9Frame` copies the whole surface, so the back buffer is
replaced outright by the helper's finished output. Pipelined, that output is frame N-1 complete and
self-consistent: nothing from frame N is mixed into it, so there is nothing to ghost against. Every
frame is still shown exactly once, one present later than it was rendered.

A residual-based composition would smear, and `ResidualLimit` suggests the runtime works in those
terms internally. It does not reach this seam: what crosses the bridge is a finished frame.

**So the trade is one frame of lag for +16% to +41%, with no measured image cost.** An early argument
for keeping `Async` off by default was that a game gaining only 7% should not pay a frame of latency.
That 7% came from a mis-sampled measurement and does not exist. On the evidence, the remaining case
for the current default is conservatism about a guarantee that was given up deliberately, not a
measured downside.

### What pipelining actually did, measured

Four subjects, 2026-09-16, each an A/B on the `Async` flag alone with the ini compared before and
after so nothing else differed. **Final numbers:**

| game | path | same-frame | pipelined | gain |
|---|---|---|---|---|
| GTA IV | D3D9 classic | 43.8 FPS | 61.6 FPS | **+41%** |
| Resident Evil 5 | D3D9 classic | 46.6 FPS | 62.2 FPS | **+33%** |
| Half-Life 2 | D3D9Ex shared | 77.0 FPS | 89.3 FPS | **+16%** |
| Silent Hill 3 | D3D8 -> D3D9 classic | not measurable | | |

**No dropped frames and no faults in any of them.** Resident Evil 5 alone produced five swapchain
resets across its two runs, which exercised the stale-answer guard without one spurious drop. Silent
Hill 3 ran correctly but could not be measured: its own frame is 1.8 ms against a 9.4 ms network, and
its frame lands on a display-sync boundary, so the periods quantise to 16.6 and 33.4 ms and the
1.8 ms at stake is invisible between steps of 16.67.

Resident Evil 5's figures are the game's own benchmark, which is the measurement of record; the
others are the frontend's period probe over adjacent windows. The detail for each follows.

**GTA IV, classic CPU staging, 960x540, Scale 0.5, A/B on the `Async` flag alone.** The ini was
snapshotted before the first run and compared after it, so nothing but the flag differed. Each
effect-on block is paired with the effect-off windows **immediately adjacent to it in the log**,
which is the only way to know the two describe the same scene:

| run | game alone | with effect | FPS | bridge | host residual | network |
|---|---|---|---|---|---|---|
| same-frame | 7.36 ms | 22.83 ms | **43.8** | 15.71 ms | 10.02 ms | 9.0 ms |
| pipelined | 7.29 ms | 16.24 ms | **61.6** | 9.19 ms | 3.64 ms | 9.9 ms |
| pipelined, second run | 7.65 ms | 16.19 ms | **61.8** | 13.76 ms | 7.86 ms | 9.7 ms |

The game-alone baselines agree to within 0.36 ms, so this is the same spot. **43.8 to 61.6 FPS,
+41%**, reproduced by two independent pipelined runs.

**The model that fits every block.** Same-frame serializes, so the frame is
`game + transport + network`: 7.36 + 5.74 + 9.0 = 22.1 against 22.83 measured. Pipelined, the game's
own frame and the network run concurrently, so it is `max(game, network) + transport`:
max(7.29, 9.9) + 5.9 = 15.8 against 16.24 measured.

**Pipelining removes `min(game, network)` from the frame.** Here the game is 7.3 ms and the network
9.9, so it removes the game's own time. That also explains the residual, which is not a constant:
inside one block it grew from 2.93 ms to 8.19 ms while the frame stayed pinned at 16.0, because a
lighter game frame covers less of the network and the wait absorbs the difference. The frame does
not move, because it is the network setting the pace, not the game.

**An earlier reading of this same data concluded that pipelining bought nothing here, and it was
wrong.** The same-frame effect-on block was paired with effect-off windows taken from the tail of
that log, 13.40 ms, which came from a heavier part of the session rather than from beside the
measurement. That inflated the same-frame baseline and made the effect look 9.40 ms cheap instead of
15.47. The adjacent windows are 7.36. **Pair against adjacent windows, never against a median or a
tail**: the game's own cost drifts by a factor of two across a session as traffic and time of day
move.
it already.

**Half-Life 2, shared GPU staging, Scale 0.5, same build, A/B on the `Async` flag alone.** The
effect-off baseline is 3.46 ms in one run and 3.47 in the other, so the scene is the same and the
two are directly comparable:

| | effect off | effect on | bridge total | host |
|---|---|---|---|---|
| same-frame | 3.46 ms, 289 FPS | 13.00 ms, **77.0 FPS** | 11.14 ms | 10.10 ms |
| pipelined | 3.47 ms, 288 FPS | 11.20 ms, **89.3 FPS** | 8.3 - 10.3 ms | 7.1 - 9.2 ms |

**+16%, and that is the whole win available here.** The network costs 10.1 ms and the game's own
frame is 3.47 ms. **Pipelining can hide at most the game's own frame time**, because that is all the
game gives it to work behind. Half-Life 2 finishes in 3.5 ms and comes back asking for an answer
that needs 10, so most of the wait survives: the residual is 7.1 to 9.2 ms against 10.1 serial. The
measured saving is 1.8 ms against a 3.47 ms ceiling, which is the "half to two thirds of the stated
gain" caveat holding exactly.

Its transport was already trivial on the shared path -- 0.70 in, 0.35 out -- so there was nothing
else to win either.

Its same-frame numbers are worth noting for their stability: 12.94 to 13.05 ms across eighteen
windows, a spread of 0.1 ms. Pipelined presentation is noisier by construction, because each frame
depends on how much of the previous one's work the game happened to cover.



### Resident Evil 5, and how to read a benchmark

Resident Evil 5 has a built-in benchmark: a fixed camera path, `VSYNC=OFF`, `FrameRate=VARIABLE`,
and an average FPS reported at the end. A/B on the `Async` flag alone, 960x540, Scale 0.5, ini
compared before and after:

| | benchmark result | frame |
|---|---|---|
| same-frame | **46.6 FPS** | 21.46 ms |
| pipelined | **62.2 FPS** | 16.08 ms |

**+33.5%**, a saving of 5.38 ms. Zero dropped frames, zero faults, five swapchain resets across the
runs, which exercised the stale-answer guard without one spurious drop.

**Use the game's own benchmark figure, and never the tail of the log.** A first pass at this
reported 60.5 against 64.5 FPS, a +6.6% that was wrong twice over. The benchmark's path is not
uniform: the same-frame windows span 16.25 to 24.29 ms, with 24 of 54 between 20 and 22 and only the
closing stretch near 16.5. Averaging every window gives 50.3 FPS, within a few percent of the 46.6
the benchmark reported. Reading the last six gives 60.5, which is 30% optimistic and describes the
easiest part of the run.

The same mistake had already been made twice this day, both times by sampling the end of a log
instead of aggregating it. **Aggregate every window; where a game ships a benchmark, its number is
the measurement and ours is at best a cross-check.**

### The rule

**saving = min(game, network), less whatever already overlapped**, where the overlap is
`game + bridge - frame` measured in same-frame mode: what the bridge spent that never reached the
frame because the game's GPU work ran while our CPU sat blocked in the IPC wait.

| | min(game, network) | already overlapping | ceiling | measured saving |
|---|---|---|---|---|
| GTA IV | 7.36 ms | -0.73 ms | 8.09 ms | 6.59 ms |
| Resident Evil 5 | 8.37 ms | 1.47 ms | 6.90 ms | 5.38 ms |
| Half-Life 2 | 3.47 ms | 1.62 ms | 1.85 ms | 1.80 ms |

Every game lands between 66% and 97% of its ceiling. The overlap term is small everywhere measured,
between -0.73 and 1.62 ms, so **`min(game, network)` alone is a good first estimate** and the
correction is a haircut rather than the story.

An earlier version of this table put Resident Evil 5's overlap at 6.12 ms and its gain at 6.6%, and
concluded that existing overlap was the dominant term. Both came from the tail-sampled frame time of
16.52 ms instead of the true 21.46. With the benchmark's own figure the game falls in line with the
other two.

Measured gains: **+16% to +41%**, and the largest is in the case that needs it most, a heavy game on
the classic D3D9 path.


### The inline GPU wait, a second serialization pipelining does not touch

The helper still reports the game's GPU queue spinning for the whole network duration, in the same
run where the frontend's CPU-side residual is 0.08 ms:

```
game queue: capture copies 0.02, spin waiting on us 9.9, residual copy+apply 0.02
```

That is `Inline=1`: the composition waits on a flag from the helper **on the game's own queue**, so
the frame is finished in place. Pipelining removed the CPU wait and left this one standing. It costs
less than it looks, because the spin overlaps the game's own CPU work -- GTA IV measured 11.5 ms
with the effect off against 18.3 ms with it on, an 6.8 ms delta against a 9.9 ms spin -- but it is
real, and it is the next lever of that size on this path.

It is untested whether `Inline=0` composes correctly alongside pipelined presentation. Both defer
the result by design and nothing has yet checked that they defer it by the same frame.
### What is actually left for the classic D3D9 path

The 5.55 ms is 8.3 MB crossing PCIe down, 8.3 MB copied between two API allocations on the CPU, and
8.3 MB crossing back, every frame at 1920x1080. None of those three is removable while the transport
is a CPU round trip:

- The intermediate `StretchRect` before `GetRenderTargetData` costs almost nothing by comparison —
  a GPU-local blit of 8 MB is microseconds on this card — so removing it is not the win it looks
  like. It also resolves multisampling, which the direct read cannot.
- The CPU memcpy between `readback9` and the D3D11 staging texture exists because they are separate
  allocations in separate APIs. Nothing merges them short of sharing.
- Less data cannot be sent. The full frame is needed both as network input and to compose the
  corrected result at full resolution.

**Pipelining is the one option that hides the wait, and it is now implemented and on**, behind
`Async` under `[dlss5]` in `dlss5-neural.ini`, and **it is now the default**. `Async=0` restores
same-frame presentation.

**The mode switches while the game runs**, from the overlay's Timing control, which reads and writes
the frontend's own flag rather than a shadow field and needs no round trip to the helper. That is
safe in both directions for a reason that predates the feature: `CollectPending` runs unconditionally
at the top of every present, before anything else touches the pipe, so whichever mode the next
present picks, it starts with nothing outstanding. Turning pipelining off collects the answer in
flight and drops it, costing one corrected frame; turning it on leaves the first present with no
previous answer to compose, so that frame shows the game's own image. Both callers hold `g.lock` for
their whole body, so the flag cannot change underneath a present already running.

The choice is persisted one key at a time, the way the helper persists its own settings. Rewriting
the whole ini from the frontend would drop `Timing` and everything the helper owns.

A timing window that spans a switch is discarded rather than averaged, the same treatment a window
spanning an effect toggle already got. Both log lines now name the mode they were measured in, which
matters more than it sounds: half the wrong conclusions in this document came from comparing numbers
whose conditions were not written down beside them.
Async does not hide the transport -- the capture and the return still happen in `Present` either
way. **It hides the wait.** The frame is posted without waiting for it, the answer to the previous
present's frame is composed instead, and the helper works while the game builds its next frame.

How it is built, and the rules that keep it safe:

- `bridge_io.h` splits `Request` into `Post` and `Collect`. `Request` is now defined as the two
  together, so the wire rules are written once and cannot drift between the paths.
- **The pipe carries one conversation.** An uncollected answer is delivered into whatever reads
  next, so `CollectPending` runs before `SyncControls`, which talks every present, and before
  `OnDestroy` reaches `DropRemote` or `Quit`. Collecting there is a bounded pipe read, not a GPU
  wait, so it does not re-enter the display driver that `OnDestroy` is written to stay clear of.
- **One output texture**, so the helper is not given new work until the last result has left it:
  the post happens after the compose, at the very end of the present. No extra copy and no second
  allocation were introduced to allow the overlap.
- `StopHost` clears the outstanding frame, which is what makes every `Fault` path safe without each
  one remembering to.
- An answer is dropped rather than composed if the generation or the raster moved under it.
  `Confirmed` cannot catch either, because the answer does agree with the frame that asked; it is
  the world underneath that changed. The raster case is a resize. The generation case is a rebuild
  in `BuildRemote`, which runs between the post and the compose and replaces the very output texture
  the result was written into. That one is easy to miss and would read a retired surface.

The stage probe measures both modes. The reported `host` figure is the sum of the wait before
`SyncControls` and the wait at the request, so the two modes stay comparable; in pipelined mode it
is the residual, meaning how much of the helper's work the game's own frame failed to cover. A
figure near zero means the network is fully hidden.

**The costs are unchanged.** One frame of latency, and the correction computed from frame N landing
on frame N+1, which is the `same_frame=1` guarantee being given up deliberately. Standing still it
is one frame of lag and no image cost: the back buffer is replaced whole, so the displayed frame is
N-1 complete rather than a mix, and it was checked in both modes in three games with no difference
seen. Reprojection would only be needed if the seam ever became residual-based.

**Quantify before optimising anything else.** With `DLSS5_X86BRIDGE_TIMING=1` and no frame-rate cap
in the way, run one title at several Resolution Scales: `input+prepare` and `output` should stay
flat while only `host` grows. Section 4.2 is the record of what guessing cost last time.

### Open questions, not yet defects

**The D3D9 reference count at process exit** is closed, not open. It is diagnosed, it is cosmetic,
and no add-on can prevent it: ReShade never delivers `destroy_device` on that exit path and warns
two seconds before it unloads the add-on. See 4.6. This entry used to claim it was fixed and awaiting
live confirmation; the confirmation happened and disproved the fix.

**Pipelining with `Inline=0` is untested.** Inline composition waits on a helper flag on the game's
own queue, and pipelining defers the result by a frame. Both defer, and nothing has checked that
they defer by the same frame. Everything measured so far ran `Inline=1`.

**The inline GPU wait survives pipelining.** In a run where the frontend's CPU-side residual was
0.01 ms, the helper still reported the game's queue spinning for the whole network duration. It is
the next lever of the size just removed, and it is on the GPU rather than the CPU.

**Pipelining on 32-bit D3D11 is untested.** The bridge serves it and the code path is shared, so it
should behave like the D3D9Ex case, but no game has exercised it.

**Reliable depth on Vulkan.** A separate capture and discovery project. Do not enable a switch that
has no real resource behind it.

### Installer: not this repository any more

`installer/` was removed in v0.5.1. Installing is **AMD-NR ReShade Installer**, which lives in its
own repository, and `docs/installer-merge.md` is kept only for the reasoning behind what an
installer has to check before it writes anything.

The items that used to sit here are resolved or moved:

- **Externalising the embedded add-on** is done. The new installer fetches every payload, including
  the add-on, from Hugging Face or a manual download. Nothing is compiled into the installer any
  more, so the coupling is proved by hash for all of it rather than by `include_bytes!` for one file.
- **Streaming payloads instead of loading them whole** is done, by the same route. The 147 MB no
  longer passes through memory in one piece.
- **Tightening what the manifest proves** moved with the code. Editing `owned` or a recorded hash
  survived the round-trip check, because both are re-encoded faithfully; the blast radius was bounded
  by the allowed-name set and by uninstall re-hashing before it acted. If the new installer kept that
  manifest format, it kept that property too.
- **The Win32 GUI** went with the C++ installer and is the new installer's question, not this one's.

Translating the installer is likewise its own repository's work. The **add-on overlay** already has
a `Language` control, English or Brazilian Portuguese, written to `dlss5-neural.ini` as `Language=`.

### How to measure this, and three ways it went wrong

Every wrong conclusion recorded in this document came from the measurement, not the code. The
frontend's probe is sound; reading it carelessly is what produced the errors.

**Pair against adjacent windows, never a median and never the tail.** GTA IV's own frame cost drifts
by a factor of two across one session as traffic and time of day move. An effect-on block paired
with effect-off windows from elsewhere in the same log made pipelining look worthless when it was
worth +41%.

**Aggregate every window, and where a game ships a benchmark its number is the measurement.** The
Resident Evil 5 benchmark path is not uniform: same-frame windows span 16.25 to 24.29 ms. The last
six average 16.52; all 54 average 19.88, and the benchmark itself reported 21.46. Reading the tail
understated the gain by five times.

**State the conditions beside the number.** Both probe log lines now name the presentation mode and
the effect state they were measured in. Several days of confusion came from comparing figures whose
conditions were not written down next to them.

A game whose frame lands on a display-sync boundary cannot measure a small change at all: Silent
Hill 3 quantises to 16.6 and 33.4 ms, so the 1.8 ms available to it is invisible between steps.
Check for `VSYNC=OFF` and an uncapped frame rate before trusting any subject.

### The test suite needs a C++20 compiler, and this machine does not have one

`tools/test-x86bridge.py` and `tools/test-x86bridge-v2.py` compile real translation units. The
development machine used for this work had only MinGW.org g++ 6.3, which rejects `-std=c++20`, and
both scripts pick it up through `shutil.which('g++')` unless `CXX` says otherwise.

The failure is quiet in the worst way. `test-x86bridge.py` prints `PASS static boundaries` first,
then dies at the `protocol_test.cpp` compile and never reaches the line that invokes the v2 suite.
A local run therefore looks like it mostly passed while the entire v2 suite -- the overlay syntax
check and the generic-source rules -- never ran at all. Twice during this work that let something
reach CI that a local run should have caught.

Treat a compiler failure in these scripts as missing coverage rather than environment noise, and say
which checks actually ran. Installing a recent g++ or clang and pointing `CXX` at it makes the whole
suite runnable locally, and is worth doing before the next change to `src/x86bridge/`.

### Housekeeping

`patch_*.py` in `.gitignore` is anchored to the root now, but a file recreated under a name it
matches would still vanish silently. The rule exists for scratch scripts that no longer exist.

