//! What the installer actually does to a folder. No terminal, no drawing -- so the whole of it
//! can be reasoned about, and tested, without a TUI in the way.

use sha2::{Digest, Sha256};
use std::fmt::Write as _;
use std::fs;
#[cfg(windows)]
use std::os::windows::ffi::OsStrExt as _;
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
    Vulkan,
}

impl Preset {
    pub const ALL: [Preset; 5] =
        [Preset::Pcsx2, Preset::Rpcs3, Preset::Dx11, Preset::Dx12, Preset::Vulkan];

    pub fn label(self) -> &'static str {
        match self {
            Preset::Pcsx2 => "PCSX2",
            Preset::Rpcs3 => "RPCS3",
            Preset::Dx11 => "D3D11 game",
            Preset::Dx12 => "D3D12 game",
            Preset::Vulkan => "Vulkan game",
        }
    }

    /// Vulkan is not a proxy DLL: ReShade loads as a global layer and the add-on sits beside the
    /// executable all the same. Both the emulator on Vulkan and a native Vulkan game are checked
    /// the same way, which is why this is a question and not four copies of one branch.
    fn is_vulkan(self) -> bool {
        matches!(self, Preset::Rpcs3 | Preset::Vulkan)
    }

    /// "Game" is wrong for an emulator, and the people most likely to get the folder wrong are
    /// exactly the emulator users -- the files go beside the emulator, not beside the ROM.
    pub fn folder_label(self) -> &'static str {
        match self {
            Preset::Pcsx2 | Preset::Rpcs3 => " Emulator folder ",
            Preset::Dx11 | Preset::Dx12 | Preset::Vulkan => " Game folder ",
        }
    }

    /// An executable whose presence says the folder is the right one. Absent means "warn", never
    /// "refuse": there is no whitelist anywhere in this project and there is not going to be one
    /// here either.
    fn expected_exe(self) -> Option<&'static str> {
        match self {
            Preset::Pcsx2 => Some("pcsx2-qt.exe"),
            Preset::Rpcs3 => Some("rpcs3.exe"),
            Preset::Dx11 | Preset::Dx12 | Preset::Vulkan => None,
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
            Preset::Vulkan => {
                "EXPERIMENTAL. ReShade on Vulkan is a global layer, not a proxy DLL: run its \
                 installer against the game's own .exe and pick Vulkan, or nothing loads. The \
                 game also has to import vkCreateDevice statically -- one that resolves Vulkan \
                 through vkGetInstanceProcAddr cannot be hooked, and the add-on stands down \
                 rather than guess. No depth on Vulkan either way: colour and estimated motion."
            }
        }
    }
}

/// A ReShade proxy, by the name it has to be loaded under.
const PROXIES: [&str; 4] = ["d3d11.dll", "dxgi.dll", "d3d12.dll", "opengl32.dll"];

/// The sizes that go with the two hashes above. Hashing 147 MB on every keystroke is not an
/// option, but comparing a length is free, and a wrong length is a wrong file -- which is the
/// whole of what the pre-flight needs to say before anything is copied.
const RUNTIME_SIZE: u64 = 7_248_384;
const WEIGHTS_SIZE: u64 = 147_689_451;

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
    pub fn new() -> Self {
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

    if preset.is_vulkan() {
        if found.is_empty() {
            report.info(format!(
                "No ReShade proxy DLL here, which is correct for Vulkan: ReShade loads as a \
                 global layer instead. Make sure you ran its installer against {} and picked \
                 Vulkan.",
                preset.expected_exe().unwrap_or("the game's own .exe")
            ));
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

// ---------------------------------------------------------------------------------------------
// Pre-flight: everything that can be known before a single byte is written.
//
// All of it is cheap enough to redo on every keystroke -- metadata, one open(), one free-space
// call -- so the screen can answer "will this work?" while the path is still being pasted,
// instead of after 147 MB have been copied into a folder that was read-only.

/// Can this folder be written to at all? Program Files without elevation is the usual answer.
fn folder_is_writable(dir: &Path) -> bool {
    let probe = dir.join(".dlss5-installer-write-probe");
    match fs::write(&probe, b"") {
        Ok(()) => {
            let _ = fs::remove_file(&probe);
            true
        }
        Err(_) => false,
    }
}

/// A file that exists but cannot be opened for writing is held by something -- on Windows that is
/// nearly always the game still running, which is the single most common way an install fails.
fn is_locked(path: &Path) -> bool {
    path.is_file() && fs::OpenOptions::new().write(true).open(path).is_err()
}

#[cfg(windows)]
fn free_bytes(dir: &Path) -> Option<u64> {
    use windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW;
    let mut wide: Vec<u16> = dir.as_os_str().encode_wide().collect();
    wide.push(0);
    let mut free = 0u64;
    let ok = unsafe {
        GetDiskFreeSpaceExW(wide.as_ptr(), &mut free, std::ptr::null_mut(), std::ptr::null_mut())
    };
    (ok != 0).then_some(free)
}
#[cfg(not(windows))]
fn free_bytes(_dir: &Path) -> Option<u64> {
    None
}

/// ReShade writes `DisabledAddons=` into its own ini the first time anyone unticks an add-on, and
/// from then on it never loads it again and says nothing anywhere. It is the one failure in this
/// project that looks exactly like a broken install, so it is worth a line of its own.
fn check_disabled_addons(dir: &Path, report: &mut Report) {
    let ini = dir.join("ReShade.ini");
    let Ok(text) = fs::read_to_string(&ini) else { return };
    for line in text.lines() {
        let line = line.trim();
        if let Some(list) = line.strip_prefix("DisabledAddons=") {
            if list.contains(ADDON_NAME) || list.to_lowercase().contains("dlss5") {
                report.err(
                    "ReShade.ini has this add-on in DisabledAddons=. ReShade writes that line if \
                     the add-on is ever unticked, and then it never loads it again, with no error \
                     anywhere. Clear that line before blaming the install.",
                );
            } else if !list.is_empty() {
                report.info(format!("ReShade.ini disables other add-ons: {list}"));
            }
        }
    }
}

/// Same file, same bytes? Only the length is compared -- see `RUNTIME_SIZE`.
fn size_of(path: &Path) -> Option<u64> {
    fs::metadata(path).ok().map(|m| m.len())
}

/// What is known before F5, from whatever is filled in so far. Never writes anything except one
/// zero-byte probe it removes again.
pub fn preflight(game_dir: &str, runtime_dir: &str, preset: Preset) -> Report {
    let mut report = Report::new();
    let dir = resolve_source(game_dir);
    let src = resolve_source(runtime_dir);

    // --- the two files, which is where someone starts -------------------------------------
    if src.as_os_str().is_empty() {
        report.info("Waiting for field 1: the folder with the runtime and the weights.");
    } else if !src.is_dir() {
        report.err(format!("Field 1: {} is not a folder.", src.display()));
    } else {
        let mut all_there = true;
        for (name, want) in [(RUNTIME_NAME, RUNTIME_SIZE), (WEIGHTS_NAME, WEIGHTS_SIZE)] {
            match size_of(&src.join(name)) {
                None => {
                    report.err(format!("{name} is not in that folder."));
                    all_there = false;
                }
                Some(got) if got != want => {
                    report.err(format!(
                        "{name} is {got} bytes, and this release expects {want}. That is a \
                         different build, and the add-on refuses anything but the one it was \
                         compiled against.",
                    ));
                    all_there = false;
                }
                Some(_) => {}
            }
        }
        if all_there {
            report.ok("Both files are there and the right size. F5 verifies the SHA-256 too.");
        }
    }

    // --- the target ------------------------------------------------------------------------
    if dir.as_os_str().is_empty() {
        report.info(format!(
            "Waiting for field 2: the {}.",
            preset.folder_label().trim().to_lowercase()
        ));
        return report;
    }
    if !dir.is_dir() {
        report.err(format!("Field 2: {} is not a folder.", dir.display()));
        return report;
    }

    if !folder_is_writable(&dir) {
        report.err(
            "That folder cannot be written to. It is either read-only or somewhere that needs \
             administrator rights -- run this installer as administrator, or move the game.",
        );
    }

    // Anything already there and held open will fail the copy, so name the files rather than let
    // fs::copy come back with "Acesso negado" halfway through.
    let held: Vec<&str> = [ADDON_NAME, RUNTIME_NAME, WEIGHTS_NAME]
        .into_iter()
        .filter(|n| is_locked(&dir.join(n)))
        .collect();
    if !held.is_empty() {
        report.err(format!(
            "{} {} open by another program. The game or emulator is almost certainly still \
             running -- close it and this line goes away.",
            held.join(", "),
            if held.len() == 1 { "is" } else { "are" }
        ));
    }

    // --- room for the weights ---------------------------------------------------------------
    let mut need = 0u64;
    for (name, size) in
        [(ADDON_NAME, ADDON.len() as u64), (RUNTIME_NAME, RUNTIME_SIZE), (WEIGHTS_NAME, WEIGHTS_SIZE)]
    {
        if size_of(&dir.join(name)) != Some(size) {
            need += size;
        }
    }
    if let Some(free) = free_bytes(&dir) {
        if need > 0 && free < need {
            report.err(format!(
                "Not enough room: {} MB free, and this needs {} MB. The weights alone are {} MB.",
                free / 1_048_576,
                need / 1_048_576,
                WEIGHTS_SIZE / 1_048_576
            ));
        }
    }

    check_exe(&dir, preset, &mut report);
    check_reshade(&dir, preset, &mut report);
    check_disabled_addons(&dir, &mut report);

    let dead: Vec<String> =
        dead_files().into_iter().filter(|n| dir.join(n).is_file()).collect();
    if !dead.is_empty() {
        report.info(format!(
            "{} file(s) from the old per-pass layout are here and will be removed: {}",
            dead.len(),
            dead.join(", ")
        ));
    }

    if !report.failed {
        report.ok("Nothing in the way. F5 installs.");
    }
    report
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
    if dir.as_os_str().is_empty() {
        report.err("No game folder given.");
        return report;
    }
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

    #[test]
    fn preflight_asks_for_the_files_first_and_then_the_folder() {
        let empty = preflight("", "", Preset::Dx11);
        assert!(has_any(&empty, "Waiting for field 1"));
        assert!(!empty.failed, "an empty form is not an error");

        let game = temp("preflight-order");
        let half = preflight(game.to_str().unwrap(), "", Preset::Dx11);
        assert!(has_any(&half, "Waiting for field 1"));
    }

    #[test]
    fn preflight_names_a_wrong_sized_runtime_without_hashing_it() {
        let src = temp("preflight-wrong-size");
        fs::write(src.join(RUNTIME_NAME), b"far too small").unwrap();
        fs::write(src.join(WEIGHTS_NAME), vec![0u8; 32]).unwrap();

        let report = preflight("", src.to_str().unwrap(), Preset::Dx11);
        assert!(report.failed);
        assert!(has_err(&report, "different build"), "{}", report.to_log("size"));
    }

    #[test]
    fn preflight_spots_a_missing_file_in_the_runtime_folder() {
        let src = temp("preflight-missing");
        fs::write(src.join(RUNTIME_NAME), vec![0u8; RUNTIME_SIZE as usize]).unwrap();
        let report = preflight("", src.to_str().unwrap(), Preset::Dx11);
        assert!(has_err(&report, "dlssnr_on_amd_weights.bin is not in that folder"));
    }

    /// The failure nobody can diagnose from the game: ReShade quietly refusing to load the add-on
    /// because its ini still carries a DisabledAddons line from an old untick.
    #[test]
    fn preflight_finds_the_disabled_addons_line() {
        let game = temp("preflight-disabled");
        fs::write(game.join("d3d11.dll"), b"reshade").unwrap();
        fs::write(
            game.join("ReShade.ini"),
            b"[ADDON]\nDisabledAddons=dlss5 neural@dlss5-neural.addon64\n",
        )
        .unwrap();
        let report = preflight(game.to_str().unwrap(), "", Preset::Dx11);
        assert!(report.failed);
        assert!(has_err(&report, "DisabledAddons"), "{}", report.to_log("ini"));

        // An unrelated add-on being disabled is worth saying, but it is not a problem.
        fs::write(game.join("ReShade.ini"), b"[ADDON]\nDisabledAddons=SomeOther.addon64\n").unwrap();
        let clean = preflight(game.to_str().unwrap(), "", Preset::Dx11);
        assert!(!clean.failed, "{}", clean.to_log("ini"));
        assert!(has_any(&clean, "disables other add-ons"));
    }

    #[test]
    fn preflight_reports_a_file_another_program_is_holding_open() {
        let game = temp("preflight-locked");
        fs::write(game.join(ADDON_NAME), b"in place").unwrap();
        // An exclusive handle is what a running game looks like from out here.
        use std::os::windows::fs::OpenOptionsExt as _;
        let held = fs::OpenOptions::new().write(true).share_mode(0).open(game.join(ADDON_NAME));
        assert!(held.is_ok(), "could not take an exclusive handle to set the test up");

        let report = preflight(game.to_str().unwrap(), "", Preset::Dx11);
        assert!(report.failed);
        assert!(has_err(&report, "still running"), "{}", report.to_log("locked"));

        drop(held);
        let after = preflight(game.to_str().unwrap(), "", Preset::Dx11);
        assert!(!has_err(&after, "still running"), "closing it should clear the line");
    }

    #[test]
    fn preflight_is_quiet_when_there_is_genuinely_nothing_wrong() {
        let src = temp("preflight-clean-src");
        fs::write(src.join(RUNTIME_NAME), vec![0u8; RUNTIME_SIZE as usize]).unwrap();
        fs::write(src.join(WEIGHTS_NAME), vec![0u8; WEIGHTS_SIZE as usize]).unwrap();
        let game = temp("preflight-clean-game");
        fs::write(game.join("d3d11.dll"), b"reshade").unwrap();

        let report = preflight(game.to_str().unwrap(), src.to_str().unwrap(), Preset::Dx11);
        assert!(!report.failed, "{}", report.to_log("clean"));
        assert!(has_any(&report, "Nothing in the way"));
    }

    #[test]
    fn uninstall_with_no_folder_says_which_field_is_empty() {
        let report = uninstall("", Preset::Dx11);
        assert!(report.failed);
        assert!(has_err(&report, "No game folder given"));
    }

    #[test]
    fn every_preset_has_a_label_a_folder_word_and_a_note() {
        for p in Preset::ALL {
            assert!(!p.label().is_empty());
            assert!(p.folder_label().contains("folder"), "{}", p.label());
            assert!(p.note().len() > 40, "{} has no real note", p.label());
        }
        // Two entries carrying the same name is the kind of thing a copy-pasted arm produces, and
        // on screen it just looks like a preset that will not select.
        let mut labels: Vec<&str> = Preset::ALL.iter().map(|p| p.label()).collect();
        labels.sort_unstable();
        let count = labels.len();
        labels.dedup();
        assert_eq!(labels.len(), count, "two presets share a label");
    }

    #[test]
    fn the_vulkan_presets_expect_a_layer_and_the_d3d_ones_expect_a_proxy_dll() {
        let dir = temp("vulkan-clean");
        for p in [Preset::Vulkan, Preset::Rpcs3] {
            let report = install(dir.to_str().unwrap(), "", p);
            assert!(has_any(&report, "global layer"), "{} said the wrong thing", p.label());
            assert!(!has_any(&report, "No ReShade proxy DLL found"), "{}", p.label());
        }
        for p in [Preset::Dx11, Preset::Dx12] {
            let report = install(dir.to_str().unwrap(), "", p);
            assert!(has_any(&report, "No ReShade proxy DLL found"), "{}", p.label());
        }
        // With ReShade actually present the D3D route is happy and the Vulkan route objects.
        fs::write(dir.join("d3d11.dll"), b"not really reshade").unwrap();
        assert!(has_any(&install(dir.to_str().unwrap(), "", Preset::Dx11), "ReShade found"));
        assert!(has_any(&install(dir.to_str().unwrap(), "", Preset::Vulkan), "two ReShade"));
    }

    /// The one thing made-up bytes can never check: that the two SHA-256 constants this binary
    /// refuses everything else against are the hashes of the files people are actually given. If
    /// a release bumps the runtime and nobody bumps the constant, every other test still passes
    /// and every user gets "does not match the expected SHA-256".
    ///
    /// Needs a folder holding both files, named by `DLSS5_TEST_RUNTIME_DIR`. A clean clone has no
    /// such folder, so without it this reports that it was skipped instead of failing.
    #[test]
    fn a_real_install_round_trip_against_the_shipped_hashes() {
        let Ok(raw) = std::env::var("DLSS5_TEST_RUNTIME_DIR") else {
            eprintln!("skipped: set DLSS5_TEST_RUNTIME_DIR to a folder holding the two files");
            return;
        };
        let src = PathBuf::from(&raw);
        assert!(src.join(RUNTIME_NAME).is_file(), "{RUNTIME_NAME} is not in {raw}");
        assert!(src.join(WEIGHTS_NAME).is_file(), "{WEIGHTS_NAME} is not in {raw}");

        let game = temp("real-round-trip");
        fs::write(game.join("dxgi.dll"), b"stand-in for ReShade").unwrap();
        // Tuning that has to survive, and junk from the old layout that has to not.
        fs::write(game.join("dlss5-neural.ini"), b"StartOn=1\n").unwrap();
        fs::write(game.join("dlssnr_amd_pass4.dll"), b"dead").unwrap();

        let report = install(game.to_str().unwrap(), &raw, Preset::Dx11);
        assert!(!report.failed, "install failed:\n{}", report.to_log("real files"));
        for name in [ADDON_NAME, RUNTIME_NAME, WEIGHTS_NAME] {
            assert!(game.join(name).is_file(), "{name} was not installed");
        }
        assert_eq!(sha256(&game.join(RUNTIME_NAME)).unwrap(), RUNTIME_SHA);
        assert_eq!(sha256(&game.join(WEIGHTS_NAME)).unwrap(), WEIGHTS_SHA);
        assert!(!game.join("dlssnr_amd_pass4.dll").exists(), "the old per-pass file survived");

        // Running it twice is what a person does when they are not sure it worked.
        let again = install(game.to_str().unwrap(), &raw, Preset::Dx11);
        assert!(!again.failed);
        assert!(has_any(&again, "already correct, left alone"));

        let removed = uninstall(game.to_str().unwrap(), Preset::Dx11);
        assert!(!removed.failed, "uninstall failed:\n{}", removed.to_log("real files"));
        for name in [ADDON_NAME, RUNTIME_NAME, WEIGHTS_NAME] {
            assert!(!game.join(name).exists(), "{name} was left behind");
        }
        assert!(game.join("dlss5-neural.ini").is_file(), "the user's tuning was deleted");
        assert!(game.join("dxgi.dll").is_file(), "ReShade was touched, and it must not be");
    }
}
