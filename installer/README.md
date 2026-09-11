# installer

A terminal installer for the add-on, so nobody has to be told which folder the three files go in
and which of them is the wrong version. One screen, three fields, two keys.

Written in Rust with [ratatui](https://ratatui.rs). Everything Rust needs stays inside this
folder, and `target/` is ignored — **the repository never carries Rust build output**. The only
thing that ships is `dlss5-installer.exe`, attached to a release.

## What it does

- **Presets**: PCSX2, RPCS3, D3D11 game, D3D12 game. The preset decides which executable it
  expects to find, which ReShade arrangement is correct, and what warning to print. It never
  refuses an unknown folder — there is no whitelist anywhere in this project.
- **Installs** `dlss5-neural.addon64`, which is compiled into the executable, so the installer
  cannot hand out an add-on from a different release than the one it was built beside.
- **Verifies** the runtime and the weights against their known SHA-256 *before* copying them, and
  names the v0.2.14 runtime specifically rather than saying "wrong file". The add-on would refuse
  a mismatch anyway; catching it here means saying so before the game is opened.
- **Sweeps** `dlssnr_amd_pass2.dll` through `pass10.dll`, from the layout that kept one copy of
  the runtime per pass.
- **Uninstalls**: takes back everything it installed plus what the add-on writes by itself, and
  deliberately leaves `dlss5-neural.ini` alone, because that is the user's tuning.
- **On failure**, writes `dlss5-installer.log` beside itself and prints the path. Everything the
  installer saw is in that file; there is nothing it knows that the log does not say.

The runtime and the weights are **not** embedded and never will be: the weights are
NVIDIA-derived and the runtime is a third-party build. The user points at the folder they
unzipped them into.

## Building

```powershell
cd ..
./build.ps1 -Target neural      # the add-on has to exist: it is embedded
cd installer
./build.ps1                     # -> target/release/dlss5-installer.exe
```

`build.ps1` finds MSVC and the Windows SDK the same way the add-on's does, because `cargo` on the
`msvc` target shells out to `link.exe` and a plain PowerShell has neither on `PATH`. Running
`cargo build` from Git Bash does not work: its `/usr/bin/link` shadows the MSVC linker, and the
failure it produces (`link: extra operand`) does not say so.

## Testing

```powershell
cargo test --release
```

The TUI cannot be driven from a script, so the tests cover everything underneath it: what lands
in a folder, what is refused, what uninstall takes back, and that a quoted path — which is what
Windows' "Copy as path" gives you — still works.
