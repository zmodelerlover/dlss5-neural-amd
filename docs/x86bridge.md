# x86bridge overlay, protocol v2

Generic native D3D9/D3D11 x86 frontend to the original x64 neural engine, with an experimental D3D8 route through the official d3d8to9 translator. This source update adds the ReShade panel **AMD Neural Rendering** while keeping the 32-bit transport isolated from the existing x64 rendering routes.

## Build and install

On Windows with Visual Studio C++ x86/x64 tools and Windows SDK, from this folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-x86bridge.ps1
```

The script invokes native `Hostx64\x86\cl.exe` and `Hostx64\x64\cl.exe`, C++20, `/MT`; runs protocol tests for both architectures, checks PE/imports, and builds the current integrated addon64 from the same checkout. Output:

- `build-x86bridge/amd-nr.addon32` (x86)
- `build-x86bridge/amd-nr-host64.exe` (x64)

With the application closed, copy **both** files beside the ReShade proxy that the game actually loads, replacing the v1 pair together. Keep existing ReShade x86, `amd-nr.ini`, runtime DLLs, weights and runtime directories in place. Source-engine games commonly use a `bin` subdirectory recorded as `[INSTALL] BasePath` in the root ReShade INI. Do not mix protocol versions.

Open ReShade and the AMD Neural Rendering panel. Initial settings come from the host's original LoadSettings, not frontend defaults. If the effect starts disabled, opening the panel requests synchronization through the next D3D9 or D3D11 Present and the existing helper launch path.

The native x86/x64 build and protocol checks run in GitHub Actions. Live ReShade, GPU and game behavior still require manual validation on supported AMD hardware.

## Changes and ownership

| State | Owner / behavior |
|---|---|
| Enabled, StartOn, ToggleKey/Mods, DisableOnAltTab, Language | Frontend shadow; operational behavior changes locally. Mirrored into host before Save. |
| Engine/tuning settings | Original host `g.*` atomics. No parallel engine configuration. |
| Save | Pending flag; Present first sends latest SET_STATE, then host executes original SaveSettings. |
| Reload | Host executes original LoadSettings, forces inline for x86, increments revision and returns complete snapshot. Frontend replaces shadow, including hotkey/language/startup/enabled values. |
| History | Only a changed History switch clears original `g.historyValid`; no universal slider reset. |
| Residual measurement | One-shot command with monotonic ID; duplicates/unknown commands rejected. |
| Dynamic status | Sampled host counters/readiness/resolutions/guide probes, plus frontend candidate and last-capture validity. |

The panel is `core/ui/`, one implementation shared with the 64-bit add-on. `panel32.cpp` is this route's adapter: it fills the panel's settings from the shadow of the helper's `WireSettings` (`panel_wire.h`), reports the helper's `WireStatus`, and turns what the person did into flags the present path carries across. It reaches the frontend only through `frontend_port.h`. What differs from the 64-bit route is data in `PanelStatus`, never an `#ifdef` in `core/ui/`: Timing switches the bridge's own pipelining, Feed.fx is not offered, and Debug carries the protocol, pass and guide-candidate lines. A nonblocking try-lock avoids waiting on Present from ImGui; while busy, a short status replaces the panel for that invocation. No ReadFile/WriteFile/Request/host launch/GPU wait occurs in the panel or its adapter.

## Controls ported

- Top: Enabled; Enabled from the first frame; Disable on alt-tab; native virtual-key capture (Ctrl/Alt/Shift, Esc cancels); Language.
- Image: Encoding; Diffuse White; Overall Intensity; Composition; Colour Strength; Highlight Guard; Guard follows Pass Count; Residual Limit; Edge Fade; Structure Intensity; Skin Structure Strength.
- Performance: Timing; Resolution Scale; Pass Count; Taper later passes; all three per-pass overrides; Bicubic Residual Upsample.
- Guides: Read Guides From The Game; Depth; History; Motion Vectors; Motion Scale; Flow Contrast Gate; Flow Accept Ratio; actual host probes.
- Debug: Debug View; Measure Residual Again.
- Engine: Character Mask; Temporal; Tonemap; Tone Channels; Engine Scale; Reset to 1/32; original informational Model A/B/C text.
- Advanced: Local Tone Strength; read-only startup diagnostics.
- Experimental: Network Output, still strictly current-frame on this bridge.
- Status: connected/enabled/engine status, successful processed/skipped requests, dimensions, loaded/active passes, actual guide state, frontend candidates, protocol and transport mode.
- Save Settings; Reload Settings; original warning/tag legend.

Timing is a live control and selects the presentation mode: Same frame, or pipelined, which is the default. It reads and writes the frontend's own `Async` flag and saves it to `amd-nr.ini`; see the note on it below, which is the whole of what it does. It is a separate thing from inline composition, which is not a control on this route: host startup/reload force `g.inlineMode=true`, logging any Inline=0 override. Encoding and Tonemap retain restart warnings. Stage/Events/NoBridge/NoBackBuffer remain read-only INI diagnostics; no unsafe engine reinitialization is added. Transport-only keeps result=4 and disables engine widgets; control synchronization does not load HIP.

Incoming finite values clamp to widget ranges; non-finite values reject the whole update. Settings/revision and command checks live in `control_state.h`; exact field/range mapping is in `settings_fields.inc`. Initial host snapshot does not normalize or write the existing INI merely because the UI opened.

## Protocol v2

Packed fixed-width little-endian Windows wire data; uint32_t booleans. No COM pointers, HANDLE, size_t or native bool fields. Every struct has size/offset and standard-layout/trivially-copyable assertions.

| Struct | Bytes | Checked offset(s) |
|---|---:|---|
| Header | 16 | bytes=12 |
| Hello | 16 | luidHigh=8 |
| Texture | 24 | handle=16 |
| Build | 104 | motion=80 |
| Frame | 32 | resetHistory=24 |
| Ack | 48 | generation=24, luidHigh=44 |
| WireSettings | 204 | passOverride=156 |
| WireCommand | 16 | code=8 |
| WireStatus | 108 | depthMin=72 |
| StateSnapshot | 312 | status=204 |

Existing kinds 1..5 remain HELLO/BUILD/FRAME/DROP/QUIT. New kinds 6..11: GET_STATE, SET_STATE, SAVE_SETTINGS, RELOAD_SETTINGS, COMMAND, STATUS. SET_STATE body=204; COMMAND body=16; other controls have no request body. Every control response is the existing 48-byte Ack followed by a fixed 312-byte StateSnapshot, including rejected controls; FRAME replies remain exactly the original Ack. Status uses enum/flags/numbers, not strings. Protocol 1 rejects explicitly through header validation; no accidental compatibility.

Revision starts at 1 per helper session. SET_STATE must have a strictly greater revision; stale/equal/malformed updates are rejected without engine mutation. Reload increments the host revision. Measurement IDs must strictly increase in the session and cannot replay. Pending one-shots are cleared on host loss. No retry can replay a command implicitly.

Only OnPresent calls SyncControls: GET_STATE after HELLO, SET_STATE for dirty revision, pending Save/Reload/Measure, periodic STATUS (250 ms), then the existing frame path. This all runs under the existing frontend lock. Existing DROP/QUIT remain serialized lifecycle operations under that same lock. No worker thread or second launch path is introduced.

The D3D11 frame order remains capture -> D3D11 FlushAndWait -> FRAME -> original host engine -> WaitForWorkQueue -> ACK -> confirmed same frame copied back. Native D3D9 adds an adapter-matched auxiliary D3D11 device around that path. When legacy shared handles are available (normally D3D9Ex), the frame crosses through a private pair of shared GPU resources. Classic D3D9 instead resolves the back buffer, performs a bounded system-memory readback/upload into D3D11, and reverses that staging after ACK. The slower fallback favors compatibility and avoids relying on a translation wrapper. Both APIs use bounded CPU/GPU waits, the exact adapter LUID and no async or stale-output fallback. D3D9 currently transports colour only; game depth and motion-vector discovery remains D3D11-only.

## Local regression test

1. Compile both new binaries and install the pair. Open ReShade panel; confirm Protocol v2, LUID MATCH in log and `result=1 same_frame=1` when NR completes.
2. Change intensity/structure/scale and History individually. Verify visible changes without host restart. Rebind hotkey, test enable/off and alt-tab.
3. Change Language/StartOn/hotkey, Save; inspect existing INI. Change a value, Reload; confirm all displayed and operational values return to file values.
4. Test transport-only with existing `AMDNR_X86BRIDGE_TRANSPORT_ONLY=1`; panel must show transport mode and engine controls disabled, result=4. Test resize and helper termination as before.
5. Set `AMDNR_X86BRIDGE_TIMING=1` when a route feels slow. On Windows, `tools\run-with-timing.cmd "<game exe>"` sets it for that launch only; Steam launch options do not pass environment variables, and `setx` would leave the probe on for every process started afterwards. A game that re-launches itself through its own launcher, GTA IV among them, loads the add-on into a process that inherits none of that and reports `probe=off` however you start it; for those, set `Timing=1` under `[dlss5]` in `amd-nr.ini` instead, which travels with the install and does not care how the game was started. Either switch arms the same probe. The frontend then logs one averaged line per 120 completed frames splitting the bridge into `input+prepare`, `host` and `output`, and names the staging path it measured. The startup line reports `probe=on`/`probe=off` so a log says which it was.
6. Pipelined presentation is the default. The overlay's Timing control switches it while the game runs and saves the choice to `Async` under `[dlss5]` in `amd-nr.ini`, which can also be edited directly; `Async=0` is same-frame. Switching costs at most one frame either way and the log records it, and a timing window that spans a switch is discarded rather than averaged. Pipelined, the frame is posted and the previous present's answer is composed instead of waiting, so the helper works while the game builds its next frame. The startup line reports `present=pipelined` or `present=same-frame`. It is faster and it costs one frame of lag. It does not smear: the back buffer is replaced whole, so what you see is the previous frame finished and self-consistent rather than a mix of two, and three games were checked in both modes with no difference seen. It measured +16% to +41% across those three. In this mode the probe's `host` figure is the residual wait -- how much of the helper's work the game's own frame failed to cover -- so near zero means the network is fully hidden. A log line naming a dropped pipelined frame means a resize or a rebuild landed under an answer in flight and it was discarded rather than composed; a steady trickle of them is a bug, one at a resolution change is by design.
7. Send `amd-nr-x86.log`, `amd-nr-x86-host.log`, build/import/protocol logs from build-x86bridge, and any UI screenshot/error.

### Reading the stage probe

`input+prepare` covers the capture, the guide work and the D3D11 drain before the request; `host` is the bounded IPC round trip, which is the helper's inference; `output` is the finished frame's return to the back buffer. On classic D3D9 the input and output stages are a full frame crossing CPU-visible memory in each direction, so their sum is roughly fixed and does not shrink when Resolution Scale drops -- only `host` does. Compare the total against the frame budget the game is actually targeting: at 60 FPS that is 16.67 ms, and with v-sync a total just past it costs the whole half, not a proportional amount.

The probe only measures. It is off by default, adds no query, flush or wait of its own, and reads only boundaries the frame already crosses -- a wait added here would land inside the `IDirect3DDevice9::Reset` window this frontend deliberately keeps clear. Measure before changing anything: altering the requested raster without knowing which stage costs what is exactly how the reverted alignment experiment happened.

Native builds cover D3D9 and D3D11 x86. D3D8 is experimental and follows `game D3D8 -> d3d8to9 -> ReShade D3D9 -> native D3D9 frontend`; the neural bridge itself does not implement a second D3D8 renderer. Live D3D8/D3D9 interop, fullscreen transitions, MSAA behavior, classic-D3D9 staging performance and GPU/UI behavior still require manual game validation.


## Incremental update: Factory Defaults and additive installer

See [x86bridge-install.md](x86bridge-install.md). Factory Defaults restores captured upstream tuning in memory with x86 overrides while preserving operational preferences. The installer in `installer/` provides native D3D11/D3D9 presets, an experimental d3d8to9 preset and private/public sidecar layouts without dgVoodoo, alongside the x64 routes; the separate C++ installer it replaced was retired. Native builds are covered by CI; live game and GPU validation remains manual.
