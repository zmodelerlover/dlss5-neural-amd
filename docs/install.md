# Installing, by hand

**Most people should not read this file.** Use **AMD-NR ReShade Installer**, from the Releases page:
it finds the games, works out which renderer each one uses, downloads and verifies the runtime and
the weights, installs ReShade and the add-on, and can take it all back out. There is a video of the
whole thing: <https://www.youtube.com/watch?v=L2v0b98wReQ>

What follows is the same install done by hand, for a machine that cannot run the installer, for
somebody who would rather not, and because it is the whole truth about what an install is. The
files are the same, the folder is the same, and the hashes are the same.

## What you need first

**ReShade with full add-on support**, from <https://reshade.me>. Run its installer against the
game's own `.exe` and pick the API the game uses. It has to be the build labelled *with full add-on
support*; the ordinary one cannot load add-ons at all.

**For 32-bit games it has to be ReShade 6.8.0.2156, 32-bit.** This installer verifies it and
refuses any other build, including newer ones, because that is the version the bridge was tested
against.

**The runtime and the weights** — `dlssnr_amd_pass1.dll` and `dlssnr_on_amd_weights.bin`. The
installer fetches these for you; by hand they are on the `files` channel of the
[Discord](https://discord.gg/wYhvS3JSHM). They are not in this repository: the weights are
NVIDIA-derived and the runtime is a third-party build with its own terms.

Check them against `tools/SHA256SUMS.txt` before using them. The add-on hashes the runtime at load
and refuses anything that is not the exact build it was written against, because every offset in it
is a hardcoded address into that one binary.

**This release moved to the v0.3.0 runtime.** If you are upgrading, replace
`dlssnr_amd_pass1.dll` — the old one is refused and the log says so by name. The weights did not
change, so leave `dlssnr_on_amd_weights.bin` where it is.

## Installing

Everything goes in **the folder the game renders from**, which is not always the folder the game
sits in: Source keeps it in `bin\`, Unreal in `Binaries\Win64\`. It is the folder ReShade landed
in — the one holding the proxy DLL you just installed.

1. Unzip this archive anywhere.
2. Copy `amd-nr.addon64` into that folder. That is the whole of a 64-bit install.
3. Copy `dlssnr_amd_pass1.dll` and `dlssnr_on_amd_weights.bin` in beside it.
4. Copy `AMD_Neural_Feed.fx` into `reshade-shaders\Shaders\`. Optional, and the only
   file that does not go in the game's root. It is what gives the network real motion vectors
   in a game that writes no velocity buffer of its own: it reads whichever optical-flow shader
   you already have -- iMMERSE Launchpad, VORT or LumeniteFX -- and hands the field over. None
   of them is bundled. Enable it in ReShade **below** the provider, and set its
   `AMDNR_MV_PROVIDER` preprocessor definition to 1, 2 or 3 to match. Without it the add-on
   estimates motion from consecutive frames instead, which works but is a guess.
5. **For a 32-bit game**, copy `files\amd-nr.addon32`, `files\amd-nr-host64.exe` and
   `payload.sha256` in as well. The `.addon32` is what ReShade loads; the `.exe` is the 64-bit
   helper it starts, and it has to be beside it.

Two things that are not files, and are the two ways a by-hand install goes wrong:

- **`DisabledAddons=` in `ReShade.ini`.** ReShade writes that line the first time an add-on is
  unticked, and from then on it never loads it again and says nothing anywhere. If the add-on does
  not appear in the overlay, look there first.
- **The renderer the game is actually set to.** Copying files cannot change it. A game on D3D12
  with the D3D11 files installed runs perfectly and does nothing.

## What it does to the game folder

It records what it installed, what it displaced and where the backup went, in
`amd-nr.install.json` (64-bit) or `amd-nr-x86bridge.install.json` (32-bit). Uninstall reads
that back: files it replaced are restored from their backup, files it created are removed, and
anything you changed afterwards is kept and reported rather than overwritten. `amd-nr.ini` is
your tuning and is never taken away.

If ReShade's `ReShade.ini` has an `[INSTALL] BasePath` pointing inside the game directory, the
installer follows it. That is what puts the files in `bin` for Source-engine games like Half-Life 2.

## Turning it on

It starts switched off. Open the ReShade overlay with **Home**, find **AMD Neural Rendering**,
and enable it — or press **Ctrl+End**. `StartOn=1` in `amd-nr.ini` makes it come up enabled.

## Taking it back out

By hand there is no manifest, so it is the files: delete `amd-nr.addon64` (or the
`.addon32` and `amd-nr-host64.exe` pair), `dlssnr_amd_pass1.dll`,
`dlssnr_on_amd_weights.bin`, `amd-nr-pass1.dll` and the `amd-nr-runtime\` folder. ReShade itself is
its own installer's business. `amd-nr.ini` is your tuning — delete it only if you want the
defaults back.

## If something goes wrong

Two logs, both in the folder the add-on loaded from: `amd-nr.log` is the add-on — what it
detected, the back buffer size and format, and the residual measurement — and `dlssnr_on_amd.log`
is the runtime, with staging formats, per-job timings and faults.

For a 32-bit game, the add-on and its helper write `amd-nr-x86.log` and
`amd-nr-x86-host.log` in the folder the add-on actually loaded from — which is `bin` on
Half-Life 2, not the game root. `ReShade.log` is there too and says whether the add-on was loaded at
all.

Setting `AMDNR_X86BRIDGE_TIMING=1` before launching adds a line every 120 frames splitting the
bridge into capture, network and return. `run-with-timing.cmd` in the repository does it for one
launch without leaving the variable behind.

## The 32-bit routes are experimental

They work — Half-Life 2, GTA IV and Silent Hill 3 all run — but a 32-bit game cannot load the
64-bit runtime, so the add-on runs as a pair: a frontend inside the game and a helper beside it.
On plain D3D9 the frame crosses CPU-visible memory in each direction, which costs a fixed few
milliseconds every frame that no Resolution Scale reduces.

Nothing about the 64-bit D3D11, D3D12 or Vulkan routes changed.
