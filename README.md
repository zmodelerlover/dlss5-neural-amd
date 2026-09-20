# dlss5-neural-amd

A ReShade add-on that runs the DLSS 5 neural rendering network on AMD GPUs.

NVIDIA's DLSS 5 tools call `nvngx_dlssnr.dll`, which does nothing on a Radeon. This add-on drives
the AMD port of the same network instead. The port is
[DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD) by danielblnc. This project does not
reimplement the network. See [Credits](#credits).

This is a proof of concept. It works, but it is not finished software.

Discord: <https://discord.gg/wYhvS3JSHM> - for DLSS 5 in general, not a support channel for this.

## What you need

| | |
|---|---|
| GPU | AMD RDNA3 or RDNA4 with the HIP 7 runtime (`amdhip64_7.dll`). HIP 6 does not work. A current Adrenalin driver includes it. Does nothing on NVIDIA or Intel. |
| Renderer | Direct3D 11 works best. Direct3D 12 works but gets less information. Vulkan and OpenGL are experimental. 32-bit games are experimental, and 32-bit OpenGL is not supported at all. |
| ReShade | The build labelled "with full add-on support", version 6.x. The normal build cannot load add-ons. |
| Disk | About 150 MB for the network weights. |

## Install

Download **AMD-NR ReShade Installer** from the
[Releases](https://github.com/zmodelerlover/dlss5-neural-amd/releases) page. It is one `.exe` and
needs nothing installed to run.

The installer finds your games, works out which renderer each one uses, downloads the runtime and
the weights, checks every hash, installs ReShade and the add-on, and can uninstall all of it. It
writes a record of what it installed, so uninstall restores what it replaced.

Two things it cannot do:

- It cannot set the renderer inside the game. For emulators this setting decides whether the
  add-on works at all. The installer tells you which renderer to pick.
- It cannot install ReShade for Vulkan. On Vulkan, ReShade is a system-wide layer instead of a DLL
  next to the game, so run ReShade's own installer for that.

The renderer detection is usually right but not always. One folder can contain both a D3D11 and a
D3D12 executable. A wrong choice installs fine and then does nothing in the game. Each game card
links to that game's PCGamingWiki page. Check it before installing.

There is a [video of the whole install](https://www.youtube.com/watch?v=L2v0b98wReQ).

## Turn it on

Press **Home** to open ReShade, go to the **Add-ons** tab, and find **DLSS Neural Rendering (AMD)**.

The add-on starts switched off. Tick **Enabled** or press **Ctrl+End**.

It starts off on purpose. The add-on rewrites every frame, and some settings can crash the display
driver, so nothing happens until you have looked at the panel.

You can change this. **Enabled from the first frame** turns it on when the game opens. The toggle
hotkey can be rebound. **Disable the effect on alt-tab** turns it off when the game loses focus.

## Settings

Every control has a `(?)` tooltip that explains what it does. The panel is in English or Brazilian
Portuguese; use the **Language** control to switch.

Settings are saved to `dlss5-neural.ini` as soon as you release a control. Nothing is lost by
closing the game.

The controls are colour-coded:

| Colour | Meaning |
|---|---|
| Red | This value can crash the display driver. |
| Amber | Beyond what has been tested. Not known to break, not known to work. |

The main controls:

- **Resolution Scale** — the size of the network input relative to the frame. 0.50 uses a quarter
  of the pixels. Smaller is faster and loses fine detail.
- **Pass Count** — one to three evaluations. More passes strengthen the effect and can add grain.
- **Residual Limit** — caps how much the image is changed. Default 0.25.
- **Timing** — this control means different things on the two routes, and the defaults are
  opposite. On a 64-bit game it chooses when the network's answer is composed: Same frame
  waits for it, and Async is older and is not the tested path. On a 32-bit game it chooses
  presentation: pipelined is the default, because it measured 16% to 41% faster in three
  games and costs one frame of lag and nothing else. Both write `Async` to
  `dlss5-neural.ini`, so a value copied from one route's ini means the other thing in the
  other's.

The defaults are a reasonable starting point.

Each control carries a tag saying how well it is understood: `MEASURED`, `TRACED`, `UNKNOWN` or
`INERT`. The legend is at the bottom of the panel.

## Troubleshooting

| What you see | What it means |
|---|---|
| The add-on is not in the Add-ons tab | `ReShade.ini` has `DisabledAddons=` listing it under `[ADDON]`. ReShade writes that line if you ever untick the add-on. Delete the line. |
| The status says the API is wrong | Only D3D11, D3D12, Vulkan and OpenGL are supported. Check for a per-game renderer override. |
| `HIP: amdhip64_7.dll failed to load` | HIP 7 is not installed. HIP 6 does not count. |
| `hash mismatch; refused` | The wrong `dlssnr_amd_pass1.dll`. Compare with `tools/SHA256SUMS.txt`. |
| `missing:` followed by a file path | That file is not where the add-on looks. Put it at exactly that path. |
| The game crashes with `887A0005` | A Windows driver reset. Lower the Resolution Scale. |
| The colours change but textures look the same | Try Resolution Scale 0.75 or 1.00 and compare the same scene. |
| Vulkan says the present queue is not graphics-capable | The game presents from an async queue. For DOOM Eternal set `r_presentFromAsync "0"`. |

### Reporting a problem

Screenshots rarely help, because flicker is frames alternating and a still image looks fine.

Two things are needed:

1. **The status line** in the panel. It reads `Running: X processed, Y skipped (Z%)` with the
   buffer sizes. Copy that line.
2. **The logs**, both next to the game's `.exe`:
   - `dlss5-neural.log` — what the add-on detected and what it measured.
   - `dlssnr_on_amd.log` — what the runtime did.

If the problem is the installer rather than the add-on, use its own **Report a problem** button. It
collects everything into one `.zip` and opens the folder. Nothing is sent anywhere.

## Building

You do not need to build anything to use this. If you want to:

```powershell
.\build.ps1 -Target neural
```

You need Visual Studio with the C++ tools and the Windows SDK. The ReShade and Dear ImGui headers
are already in `external/`.

## Limits

- D3D11 is the only route where the game's own depth and motion vectors reach the network. On D3D12
  and Vulkan the add-on only receives the final image.
- FSR upscaling is not implemented and is not planned.
- On 32-bit D3D9 without D3D9Ex, each frame crosses system memory twice. That costs a few
  milliseconds per frame regardless of the Resolution Scale.
- This is tested by one person on one card.

## Support this project

This is one person, one graphics card, and evenings. There is no company behind it and nothing
here is sponsored. Every measurement in this README came from a single RX 9070 XT, which is why
"tested on one card" appears as often as it does.

[![Support this project on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/T6T213OVFE)

**[ko-fi.com/T6T213OVFE](https://ko-fi.com/T6T213OVFE)**

What it pays for, in order:

- **Research tokens.** Decompiling a closed runtime and chasing one field through a binary is
  metered work. This is the main thing deciding whether the next question gets answered.
- **Other hardware.** Everything is verified on one RDNA4 card. RDNA3 is untested.
- **Time.** Most of the useful work produces one number rather than a feature.
- **Games to test.** Adding a game to the list means owning it and playing it.

Nothing is gated behind a donation. The code is MIT either way.

## Credits

This project is downstream of
**[DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** by **danielblnc**. That project
produces the runtime and the weights, which is everything the network needs to run. Neither is
reimplemented or redistributed here. This repository adds the ReShade add-on around it: the D3D11,
D3D12 and Vulkan routes, the 32-bit bridge, the guide capture and the overlay.

It has its own terms. The MIT licence below covers only the code in this repository.

Thanks to everyone who ran a build and sent back a log.

## License

MIT, in `LICENSE`. Third-party headers in `external/` keep their own licences, listed in
`external/reshade/NOTICE.md`.
