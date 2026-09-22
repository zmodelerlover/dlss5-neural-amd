# Additive x86 installer + Factory Defaults

Build from the project directory on Windows with Visual Studio C++ and Windows SDK:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-x86bridge.ps1
```

The script compiles addon32 x86 /MT, host64 x64 /MT and the new installer x64 /MT, tests the protocol on both architectures, checks PE/import boundaries, and builds the current integrated addon64 from the same checkout. It places the new pair and installer in `release/`. With the private sidecars present, it also runs installer filesystem tests there. Those tests use synthetic PE targets, never the real game folder.

User-facing install instructions are in [install.md](install.md); this document is the build and
regression side of the same thing.

Open `release/amd-nr-installer.exe` -- one installer for both architectures since the separate x86 build was retired. Point field 2 at the game: it reads the PE header, says which width it found, and offers only the presets that exist for it. For a 32-bit target that is D3D11, D3D9 or the experimental D3D8 preset, and it wants the executable rather than the folder because it verifies the header before it plans. Field 1 takes the release folder, or the folder holding the runtime and weights. PE32+ targets are rejected. API selection is manual; import-based autodetection was not added. Close the game before install/uninstall. If ReShade's root `ReShade.ini` contains an `[INSTALL] BasePath` inside the selected game directory, the installer safely follows it; this supports Source-engine layouts that load the proxy and add-ons from `bin`.

## Defaults and UI

- Fresh install only: ColourStrength=0.25, Structure=1, Skin=1, Passes=1. Other values follow upstream; Scale=1 matches original EnsureNeuralIni. Existing amd-nr.ini stays byte-identical.
- Host captures constructed upstream settings before LoadSettings. Factory Defaults restores that memory snapshot with the five x86 overrides, including inline=1; it writes nothing itself, and the overlay's autosave carries it to the ini on the next frame like any other change. Constructed Scale is 0.5, whereas upstream fresh INI sets Scale=1; this original distinction is preserved.
- Factory preserves Enabled, StartOn, hotkey/modifiers, alt-tab preference and language. It leaves restart-only diagnostics alone and invalidates history once when temporal/guide switches change.
- Save and Reload retain their previous semantics. Factory uses CommandCode=2 in protocol v2; all wire sizes and frame messages are unchanged. Only SyncControls in Present sends it; overlay sets a pending flag.
- Autosave arms the same pending Save flag, from the overlay, when `shadow.settings_revision` has moved past `savedRevision` and no ImGui item is active. No new wire message: the write is the Save the host already implements, requested by the overlay instead of by a button. `savedRevision` follows `sentRevision` on the first sync, on a successful Save and on Reload, and deliberately does not on SetState or Factory — those change memory only.

## Docking

Installation merges only `[OVERLAY] Docking/Window`. A saved panel entry, even floating, is never changed. When Home is already docked, its actual DockId is reused with no position/size copied. For a completely fresh layout, the ReShade 6.8 dock tree is seeded using the installer's current monitor work area, not fixed 4K coordinates. ReShade resizes its dockspace at runtime. Other INI values remain unchanged.

If there is an unusual existing layout without a docked Home, the installer preserves it and leaves docking manual. No addon startup loop re-docks windows. Manual installation without running the installer does not seed a layout. Runtime behavior still needs Windows validation; passing serialized-layout tests is not proof of live ImGui docking.

## Files, safety and payloads

- D3D11 uses the native DXGI ReShade proxy. D3D9 uses the native D3D9 ReShade proxy and a private in-process D3D9/D3D11 stage: shared GPU resources where supported and a CPU-compatible staging fallback for classic D3D9.
- Experimental D3D8 installs the pinned official d3d8to9 v1.15.1 binary as `d3d8.dll`, then installs ReShade as `d3d9.dll` and reuses that same native D3D9 frontend. If an existing x86 `d3d8.dll` explicitly advertises the standard `d3d8R.dll` forwarding name, it is preserved and the translator is installed under that sidecar name instead. Unknown wrappers fail closed rather than being replaced. The expected d3d8to9 SHA-256 is `ab6bf7a9a9f4b3e66a75ca038d8d10289c88acbfe8d52c3b5a8a9a259cb26cd5`. No dgVoodoo archive, executable, DLL or configuration is downloaded or installed.
- SHA256 validates the runtime, weights, ReShade and bridge pair before changing target files. No third-party or neural payload is embedded in the installer EXE; release sidecars remain separate.
- Existing conflicts are backed up under `.amd-nr-x86bridge-backups/`; `amd-nr-x86bridge.install.json` records hashes/ownership/backups/versions. Reinstall of same content is idempotent. Changing preset requires uninstall first.
- Uninstall restores unchanged backups and removes owned unmodified binaries. Personal configs and files modified after installation are retained with warnings and manifest records. It never removes unrelated files. Installation journals target writes; interrupted installs can be recovered via uninstall; do not delete the manifest/backups.
- Existing HIP installation is a prerequisite, unchanged from the validated setup. No HIP installer or new runtime was introduced.

The installer never launches a ReShade Setup executable. Install ReShade Full Add-on Support manually for the API route, or provide the separately obtained x86 ReShade DLL as `files/dxgi.dll`; the installer writes it as `dxgi.dll` for D3D11 or `d3d9.dll` for D3D9/D3D8. Its SHA-256 must match the supported 6.8.0.2156 build. Other versions fail closed.

For D3D8, separately obtain the official v1.15.1 `d3d8.dll` release asset and import it as `release/files/d3d8to9.dll` with `tools/import-d3d8to9.ps1`. The import script accepts only the pinned PE32 build and never downloads or executes it. d3d8to9 uses `d3dx9_43.dll` for shader conversion, so older systems or games may also need Microsoft's legacy DirectX End-User Runtime. Its BSD-2-Clause attribution is retained under `docs/third-party/`. Public packaging excludes all third-party payloads.

## Tests and manual regression

Portable tests cover the protocol, control flow, IPC timeout behavior, overlay compilation and host Factory methods with engine-state doubles. Installer filesystem tests cover SHA validation, native proxy selection, the optional D3D8 sidecar, safe BasePath handling, transactional recovery and docking merges when a private pinned-payload fixture is supplied.

MSVC build/PE imports are covered by `build-x86bridge.ps1` and CI. Windows installer UI, live docking and GPU/game runtime still require live validation.

Local checklist (no game-specific production code):
1. Half-Life 2, RE1 HD Remaster and RE5: use the native D3D9 preset; test resize, fullscreen/windowed transitions and RE5 4K. Verify LUID MATCH and `result=1 same_frame=1`.
2. Silent Hill 3 or another true D3D8 x86 title: select D3D8 and verify that the game loads `d3d8.dll`, ReShade loads through `d3d9.dll`, the add-on panel appears and the log reports the native D3D9 frontend. Test fullscreen/window transitions and Alt+Tab.
3. Check 1/2 passes and live controls. Fresh install Colour Strength=0.25; new panel docked. Undock, restart/reinstall and verify personal layout remains.
4. Change tuning; Save. Change again; Factory Defaults. Confirm INI unchanged by Factory and preferences preserved. Reload must recover the last Save.
5. Test uninstall in a copied game folder first; inspect retained config/backups. Send the installer log, frontend/host logs and build/import/protocol logs.
