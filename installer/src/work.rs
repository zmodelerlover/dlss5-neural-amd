//! What the installer actually does to a folder. No terminal, no drawing -- so the whole of it
//! can be reasoned about, and tested, without a TUI in the way.

use sha2::{Digest, Sha256};
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

/// The add-on, built into this executable. One file to hand out, and the installer can never
/// install an add-on from a different release than the one it was built beside.
pub const ADDON: &[u8] = include_bytes!("../../build/dlss5-neural.addon64");
pub const ADDON_NAME: &str = "dlss5-neural.addon64";

pub const RUNTIME_NAME: &str = "dlssnr_amd_pass1.dll";
pub const WEIGHTS_NAME: &str = "dlssnr_on_amd_weights.bin";

/// The add-on hashes the runtime at load and refuses anything else, because the integration is
/// fixed offsets into one specific binary. Checking here as well means the installer can say so
/// in words, instead of leaving the add-on to fail later with the game already open.
pub const RUNTIME_SHA: &str = "ddd82d313aa74c2e7602d17dfb7e7cd90cca9bfc0306f581684d35d75d1b350b";
pub const WEIGHTS_SHA: &str = "6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab";

/// Known-bad: the runtime this release replaced. Recognised by name so the message can be
/// "you have the old one" instead of "this file is wrong".
pub const RUNTIME_SHA_0214: &str =
    "e145ff963b1ef614000000000000000000000000000000000000000000000000";

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Preset {
    Pcsx2,
    Rpcs3,
    Dx11,
    Dx12,
}

impl Preset {
    pub const ALL: [Preset; 4] = [Preset::Pcsx2, Preset::Rpcs3, Preset::Dx11, Preset::Dx12];

    pub fn label(self) -> &'static str {
        match self {
            Preset::Pcsx2 => "PCSX2",
            Preset::Rpcs3 => "RPCS3",
            Preset::Dx11 => "D3D11 game",
            Preset::Dx12 => "D3D12 game",
        }
    }

    /// An executable whose presence says the folder is the right one. Absent means "warn", never
    /// "refuse": there is no whitelist anywhere in this project and there is not going to be one
    /// here either.
    fn expected_exe(self) -> Option<&'static str> {
        match self {
            Preset::Pcsx2 => Some("pcsx2-qt.exe"),
            Preset::Rpcs3 => Some("rpcs3.exe"),
            Preset::Dx11 | Preset::Dx12 => None,
        }
    }

    pub fn note(self) -> &'static str {
        match self {
            Preset::Pcsx2 => {
                "Set the renderer to Direct3D 11 -- it is the only one where the emulator's depth \
                 reaches the network. Watch for a per-game override: it beats the global setting \
                 silently, and it is the most common way this looks broken when it is not."
            }
            Preset::Rpcs3 => {
                "EXPERIMENTAL. ReShade on Vulkan is a global layer, not a proxy DLL: run the \
                 ReShade installer against rpcs3.exe and pick Vulkan, or nothing will load. The \
                 network gets colour and estimated motion only -- there is no depth on Vulkan."
            }
            Preset::Dx11 => {
                "The best case. D3D11 is the only route where the game's own depth and motion \
                 vectors reach the network."
            }
            Preset::Dx12 => {
                "The degraded case. On D3D12 an add-on is shown nothing but the swapchain, so the \
                 network gets colour and guesses at the rest. It works; expect less from it."
            }
        }
    }
}

/// A ReShade proxy, by the name it has to be loaded under.
const PROXIES: [&str; 4] = ["d3d11.dll", "dxgi.dll", "d3d12.dll", "opengl32.dll"];

/// Files an older layout left behind. One copy of the runtime per pass, which did not fit in
/// VRAM and has not been used for two releases.
fn dead_files() -> Vec<String> {
    (2..=10).map(|n| format!("dlssnr_amd_pass{n}.dll")).collect()
}

pub struct Report {
    pub lines: Vec<(Level, String)>,
    pub failed: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Level {
    Ok,
    Warn,
    Err,
    Info,
}

impl Report {
    fn new() -> Self {
        Report { lines: Vec::new(), failed: false }
    }
    fn ok(&mut self, s: impl Into<String>) {
        self.lines.push((Level::Ok, s.into()));
    }
    fn info(&mut self, s: impl Into<String>) {
        self.lines.push((Level::Info, s.into()));
    }
    fn warn(&mut self, s: impl Into<String>) {
        self.lines.push((Level::Warn, s.into()));
    }
    fn err(&mut self, s: impl Into<String>) {
        self.lines.push((Level::Err, s.into()));
        self.failed = true;
    }

    /// The report as a file, for the user to hand over when something went wrong. Everything the
    /// installer saw is in here; there is nothing it knows that this does not say.
    pub fn to_log(&self, header: &str) -> String {
        let mut out = String::new();
        let _ = writeln!(out, "dlss5-neural-amd installer log");
        let _ = writeln!(out, "{header}");
        let _ = writeln!(out, "{}", "-".repeat(70));
        for (level, line) in &self.lines {
            let tag = match level {
                Level::Ok => "ok  ",
                Level::Warn => "warn",
                Level::Err => "ERR ",
                Level::Info => "    ",
            };
            let _ = writeln!(out, "{tag} {line}");
        }
        out
    }
}

fn sha256(path: &Path) -> std::io::Result<String> {
    let bytes = fs::read(path)?;
    let mut hasher = Sha256::new();
    hasher.update(&bytes);
    Ok(format!("{:x}", hasher.finalize()))
}

/// Accepts either the folder holding the two files or one of the files themselves, because both
/// are things a person reasonably pastes.
fn resolve_source(raw: &str) -> PathBuf {
    let p = PathBuf::from(raw.trim().trim_matches('"'));
    if p.is_file() {
        p.parent().map(Path::to_path_buf).unwrap_or(p)
    } else {
        p
    }
}

fn check_reshade(dir: &Path, preset: Preset, report: &mut Report) {
    let found: Vec<&str> = PROXIES.iter().copied().filter(|n| dir.join(n).is_file()).collect();

    if preset == Preset::Rpcs3 {
        if found.is_empty() {
            report.info(
                "No ReShade proxy DLL here, which is correct for Vulkan: ReShade loads as a \
                 global layer instead. Make sure you ran its installer against rpcs3.exe and \
                 picked Vulkan.",
            );
        } else {
            report.warn(format!(
                "Found {} here. On Vulkan ReShade loads as a global layer, and a proxy DLL as \
                 well means two ReShade instances in one process. Remove it if Vulkan is what \
                 you run.",
                found.join(", ")
            ));
        }
        return;
    }

    match found.len() {
        0 => report.warn(
            "No ReShade proxy DLL found here (d3d11.dll, dxgi.dll, d3d12.dll). The add-on cannot \
             load without ReShade, and it has to be the build with full add-on support. Files \
             were still copied, so installing ReShade afterwards is enough.",
        ),
        1 => report.ok(format!("ReShade found: {}", found[0])),
        _ => report.warn(format!(
            "More than one ReShade proxy here ({}). Only one is loaded, and which one depends on \
             the game. Keep the one that matches the renderer.",
            found.join(", ")
        )),
    }
}

fn check_exe(dir: &Path, preset: Preset, report: &mut Report) {
    if let Some(exe) = preset.expected_exe() {
        if dir.join(exe).is_file() {
            report.ok(format!("{exe} is here, so this is the right folder."));
        } else {
            report.warn(format!(
                "{exe} is not in this folder. That is only a warning -- nothing checks which \
                 program it is -- but it is usually a sign the path is wrong."
            ));
        }
    }
}

fn copy_verified(
    src_dir: &Path,
    dst_dir: &Path,
    name: &str,
    want_sha: &str,
    report: &mut Report,
) -> bool {
    let src = src_dir.join(name);
    if !src.is_file() {
        report.err(format!("{name} is not in the runtime folder you gave."));
        return false;
    }
    let got = match sha256(&src) {
        Ok(h) => h,
        Err(e) => {
            report.err(format!("could not read {name}: {e}"));
            return false;
        }
    };
    if got != want_sha {
        if name == RUNTIME_NAME && got.starts_with(&RUNTIME_SHA_0214[..16]) {
            report.err(format!(
                "{name} is the old v0.2.14 runtime. This release requires v0.2.17 and the add-on \
                 refuses anything else. Get the current one from the files channel."
            ));
        } else {
            report.err(format!(
                "{name} does not match the expected SHA-256.\n      expected {want_sha}\n      \
                 got      {got}\n      The add-on hashes the runtime at load and will refuse it."
            ));
        }
        return false;
    }
    // Same file already in place: copying it over itself would be a no-op that can still fail on
    // a locked handle, so say so instead.
    let dst = dst_dir.join(name);
    if dst.is_file() && sha256(&dst).map(|h| h == want_sha).unwrap_or(false) {
        report.ok(format!("{name} already correct, left alone."));
        return true;
    }
    match fs::copy(&src, &dst) {
        Ok(_) => {
            report.ok(format!("{name} copied and verified."));
            true
        }
        Err(e) => {
            report.err(format!(
                "could not write {name}: {e}. If the game is open, close it and try again."
            ));
            false
        }
    }
}

fn sweep_dead(dir: &Path, report: &mut Report) {
    let mut removed = Vec::new();
    for name in dead_files() {
        let p = dir.join(&name);
        if p.is_file() {
            match fs::remove_file(&p) {
                Ok(()) => removed.push(name),
                Err(e) => report.warn(format!("could not remove {name}: {e}")),
            }
        }
    }
    if !removed.is_empty() {
        report.ok(format!(
            "removed {} unused file(s) from the old per-pass layout: {}",
            removed.len(),
            removed.join(", ")
        ));
    }
}

pub fn install(game_dir: &str, runtime_dir: &str, preset: Preset) -> Report {
    let mut report = Report::new();
    let dir = resolve_source(game_dir);
    let src = resolve_source(runtime_dir);

    if dir.as_os_str().is_empty() {
        report.err("No game folder given.");
        return report;
    }
    if !dir.is_dir() {
        report.err(format!("{} is not a folder.", dir.display()));
        return report;
    }
    report.info(format!("target: {}", dir.display()));
    report.info(format!("preset: {}", preset.label()));

    check_exe(&dir, preset, &mut report);
    check_reshade(&dir, preset, &mut report);

    match fs::write(dir.join(ADDON_NAME), ADDON) {
        Ok(()) => report.ok(format!("{ADDON_NAME} written ({} bytes).", ADDON.len())),
        Err(e) => report.err(format!(
            "could not write {ADDON_NAME}: {e}. If the game is open, close it and try again."
        )),
    }

    if src.as_os_str().is_empty() {
        report.warn(
            "No runtime folder given, so the runtime and weights were not installed. The add-on \
             does nothing without them. Re-run with the folder you unzipped them into.",
        );
    } else if !src.is_dir() {
        report.err(format!("{} is not a folder.", src.display()));
    } else {
        copy_verified(&src, &dir, RUNTIME_NAME, RUNTIME_SHA, &mut report);
        copy_verified(&src, &dir, WEIGHTS_NAME, WEIGHTS_SHA, &mut report);
    }

    sweep_dead(&dir, &mut report);

    if !report.failed {
        report.info(preset.note());
        report.info(
            "It starts switched off. Open the overlay with Home, or press Ctrl+End. StartOn=1 in \
             dlss5-neural.ini makes it come up enabled.",
        );
    }
    report
}

pub fn uninstall(game_dir: &str, _preset: Preset) -> Report {
    let mut report = Report::new();
    let dir = resolve_source(game_dir);
    if !dir.is_dir() {
        report.err(format!("{} is not a folder.", dir.display()));
        return report;
    }
    report.info(format!("target: {}", dir.display()));

    // Everything the add-on installs or writes. The ini is deliberately not in this list.
    let mut names: Vec<String> = vec![
        ADDON_NAME.into(),
        RUNTIME_NAME.into(),
        WEIGHTS_NAME.into(),
        "dlss5-pass1.dll".into(),
        "dlss5-neural.log".into(),
        "dlssnr_on_amd.log".into(),
        "dlssnr_on_amd.ini".into(),
    ];
    names.extend(dead_files());

    let mut gone = 0usize;
    for name in &names {
        let p = dir.join(name);
        if p.is_file() {
            match fs::remove_file(&p) {
                Ok(()) => {
                    gone += 1;
                    report.ok(format!("removed {name}"));
                }
                Err(e) => report.err(format!("could not remove {name}: {e}")),
            }
        }
    }

    for folder in ["dlss5-runtime", "dlss5-captures"] {
        let p = dir.join(folder);
        if p.is_dir() {
            match fs::remove_dir_all(&p) {
                Ok(()) => {
                    gone += 1;
                    report.ok(format!("removed {folder}\\"));
                }
                Err(e) => report.err(format!("could not remove {folder}: {e}")),
            }
        }
    }

    if gone == 0 {
        report.warn("Nothing of ours was in that folder.");
    }
    if dir.join("dlss5-neural.ini").is_file() {
        report.info(
            "dlss5-neural.ini was left in place: it is your tuning, not ours. Delete it by hand \
             if you want a clean slate.",
        );
    }
    report.info("ReShade itself was left alone. Use its own installer to remove it.");
    report
}

// The TUI cannot be exercised from a script, so what is checked here is everything underneath
// it: what lands in a folder, what is refused, and what uninstall takes back out. The real
// weights are 147 MB, so the tests use stand-ins and assert on the paths that do not need the
// genuine bytes -- a wrong hash, a missing file, the dead-file sweep, the round trip.
#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("dlss5-installer-test-{name}"));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn has_err(report: &Report, needle: &str) -> bool {
        report.lines.iter().any(|(l, t)| *l == Level::Err && t.contains(needle))
    }
    fn has_any(report: &Report, needle: &str) -> bool {
        report.lines.iter().any(|(_, t)| t.contains(needle))
    }

    #[test]
    fn addon_is_embedded_and_looks_like_a_dll() {
        assert!(ADDON.len() > 100_000, "add-on looks too small: {}", ADDON.len());
        assert_eq!(&ADDON[..2], b"MZ", "embedded add-on is not a PE image");
    }

    #[test]
    fn install_writes_the_addon_even_with_no_runtime_folder() {
        let game = temp("addon-only");
        let report = install(game.to_str().unwrap(), "", Preset::Dx11);
        assert!(!report.failed, "should not fail without a runtime folder");
        assert!(game.join(ADDON_NAME).is_file());
        assert_eq!(fs::read(game.join(ADDON_NAME)).unwrap().len(), ADDON.len());
        assert!(has_any(&report, "does nothing without them"));
    }

    #[test]
    fn a_runtime_with_the_wrong_hash_is_refused_and_not_copied() {
        let game = temp("bad-hash-game");
        let src = temp("bad-hash-src");
        fs::write(src.join(RUNTIME_NAME), b"not the runtime").unwrap();
        fs::write(src.join(WEIGHTS_NAME), b"not the weights").unwrap();

        let report = install(game.to_str().unwrap(), src.to_str().unwrap(), Preset::Dx11);
        assert!(report.failed);
        assert!(has_err(&report, "does not match the expected SHA-256"));
        assert!(!game.join(RUNTIME_NAME).exists(), "a rejected file must not be copied");
        assert!(!game.join(WEIGHTS_NAME).exists());
    }

    #[test]
    fn a_missing_runtime_file_is_named() {
        let game = temp("missing-game");
        let src = temp("missing-src");
        let report = install(game.to_str().unwrap(), src.to_str().unwrap(), Preset::Dx11);
        assert!(report.failed);
        assert!(has_err(&report, RUNTIME_NAME));
        assert!(has_err(&report, WEIGHTS_NAME));
    }

    #[test]
    fn the_old_per_pass_files_are_swept() {
        let game = temp("sweep");
        for n in 2..=10 {
            fs::write(game.join(format!("dlssnr_amd_pass{n}.dll")), b"x").unwrap();
        }
        let report = install(game.to_str().unwrap(), "", Preset::Dx11);
        assert!(has_any(&report, "old per-pass layout"));
        for n in 2..=10 {
            assert!(!game.join(format!("dlssnr_amd_pass{n}.dll")).exists(), "pass{n} survived");
        }
    }

    #[test]
    fn uninstall_takes_back_what_install_put_there_and_keeps_the_ini() {
        let game = temp("round-trip");
        install(game.to_str().unwrap(), "", Preset::Dx11);
        assert!(game.join(ADDON_NAME).is_file());

        // Tuning the user owns, plus a folder the add-on writes by itself.
        fs::write(game.join("dlss5-neural.ini"), b"[dlss5]\nScale=0.5\n").unwrap();
        fs::create_dir_all(game.join("dlss5-runtime")).unwrap();
        fs::write(game.join("dlss5-runtime").join("d3d12.dll"), b"x").unwrap();

        let report = uninstall(game.to_str().unwrap(), Preset::Dx11);
        assert!(!report.failed);
        assert!(!game.join(ADDON_NAME).exists());
        assert!(!game.join("dlss5-runtime").exists());
        assert!(game.join("dlss5-neural.ini").is_file(), "the ini is the user's, not ours");
    }

    #[test]
    fn uninstall_on_an_unrelated_folder_says_so_rather_than_failing() {
        let game = temp("empty");
        let report = uninstall(game.to_str().unwrap(), Preset::Dx11);
        assert!(!report.failed);
        assert!(has_any(&report, "Nothing of ours"));
    }

    #[test]
    fn a_quoted_path_is_accepted_because_windows_copies_them_that_way() {
        let game = temp("quoted");
        let quoted = format!("\"{}\"", game.display());
        let report = install(&quoted, "", Preset::Dx11);
        assert!(!report.failed, "a path with quotes around it should still work");
        assert!(game.join(ADDON_NAME).is_file());
    }

    #[test]
    fn a_path_that_is_not_a_folder_is_reported_not_ignored() {
        let report = install(r"Z:\definitely\not\here", "", Preset::Dx11);
        assert!(report.failed);
        assert!(has_err(&report, "is not a folder"));
    }
}
