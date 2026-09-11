//! Terminal installer for the dlss5-neural-amd ReShade add-on.
//!
//! Three numbered steps down the screen, in the order a person does them. The add-on is compiled
//! into this executable; the runtime and the weights are not and never will be -- the weights are
//! NVIDIA-derived and the runtime is a third-party build -- so the user points at the folder they
//! unzipped them into, and every byte is checked against a known SHA-256 before it is copied.

#![windows_subsystem = "console"]

mod diag;
mod logo;
mod ui;
mod work;

use crossterm::event::{
    self, DisableBracketedPaste, EnableBracketedPaste, Event, KeyCode, KeyEventKind, KeyModifiers,
};
use crossterm::execute;
use crossterm::terminal::{disable_raw_mode, enable_raw_mode, Clear, ClearType};
use ratatui::backend::CrosstermBackend;
use ratatui::Terminal;
use std::io::{self, Stdout};
use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};
use work::{Preset, Report};

// ---------------------------------------------------------------------------------------------
// Colour, in two parts.
//
// 1. NO_COLOR. crossterm honours https://no-color.org inside `Display for Colored`: with the
//    variable set to anything non-empty every colour escape it writes comes out with no
//    parameters at all -- `ESC [ ; m` instead of `ESC [ 38;5;10 m` -- so the layout draws
//    perfectly and entirely in white while supports_ansi() answers true and the console reports
//    65535 colours. That is exactly what this installer did for a whole session, because the
//    shell it was launched from exported NO_COLOR=1. The convention is meant for programs that
//    add colour to otherwise plain output; this is a full-screen TUI whose panes and focus
//    marker are the interface, so it opts out explicitly rather than leaving the palette to
//    whatever a parent process happened to set.
//
// 2. Virtual-terminal processing. A console opened by Explorer or Start-Process has it off, and
//    with it off the escape sequences go nowhere.
#[cfg(windows)]
fn enable_colour() {
    use windows_sys::Win32::Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
    };
    use windows_sys::Win32::System::Console::{
        GetConsoleMode, SetConsoleMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING,
    };

    // CONOUT$ rather than GetStdHandle(STD_OUTPUT_HANDLE): it always opens whichever screen
    // buffer is currently active, so the flag lands on the buffer actually being drawn to.
    unsafe {
        let name: Vec<u16> = "CONOUT$\0".encode_utf16().collect();
        let handle = CreateFileW(
            name.as_ptr(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            std::ptr::null(),
            OPEN_EXISTING,
            0,
            std::ptr::null_mut(),
        );
        if handle == INVALID_HANDLE_VALUE {
            return;
        }
        let mut mode = 0u32;
        if GetConsoleMode(handle, &mut mode) != 0 {
            SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}
#[cfg(not(windows))]
fn enable_colour() {}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Focus {
    Preset,
    GameDir,
    RuntimeDir,
}

pub(crate) struct App {
    pub(crate) preset: usize,
    pub(crate) game_dir: String,
    pub(crate) runtime_dir: String,
    pub(crate) focus: Focus,
    pub(crate) report: Option<Report>,
    /// What is wrong right now, recomputed after every keystroke. Cheap enough to do that way --
    /// it is metadata and one open() -- and it is the difference between finding out the game is
    /// still running now or after 147 MB have been copied.
    pub(crate) preflight: Report,
    pub(crate) status: String,
    quit: bool,
}

impl App {
    fn new() -> Self {
        let mut app = App {
            preset: 0,
            game_dir: String::new(),
            runtime_dir: String::new(),
            focus: Focus::RuntimeDir,
            report: None,
            preflight: Report::new(),
            status: String::new(),
            quit: false,
        };
        app.recheck();
        app
    }

    fn recheck(&mut self) {
        self.preflight = work::preflight(&self.game_dir, &self.runtime_dir, self.preset());
    }

    pub(crate) fn preset(&self) -> Preset {
        Preset::ALL[self.preset]
    }

    fn field_mut(&mut self) -> Option<&mut String> {
        match self.focus {
            Focus::GameDir => Some(&mut self.game_dir),
            Focus::RuntimeDir => Some(&mut self.runtime_dir),
            Focus::Preset => None,
        }
    }

    fn next_focus(&mut self, back: bool) {
        const ORDER: [Focus; 3] = [Focus::Preset, Focus::RuntimeDir, Focus::GameDir];
        let at = ORDER.iter().position(|f| *f == self.focus).unwrap_or(1);
        let next = if back { at + ORDER.len() - 1 } else { at + 1 } % ORDER.len();
        self.focus = ORDER[next];
    }

    /// What the screen should be telling you to do next, when nothing has been run yet.
    pub(crate) fn hint(&self) -> String {
        if self.runtime_dir.trim().is_empty() {
            "First: paste the folder holding the two files from the Discord #files channel."
                .into()
        } else if self.game_dir.trim().is_empty() {
            "Now paste the folder the game or emulator runs from, then press F5.".into()
        } else {
            "Ready. F5 installs, F8 uninstalls.".into()
        }
    }

    fn run(&mut self, uninstall: bool) {
        let report = if uninstall {
            work::uninstall(&self.game_dir, self.preset())
        } else {
            work::install(&self.game_dir, &self.runtime_dir, self.preset())
        };

        let what = if uninstall { "Uninstall" } else { "Install" };
        self.status = if report.failed {
            match write_log(&report, self.preset(), &self.game_dir) {
                Ok(path) => format!(
                    "{what} FAILED.  Log written to {}  --  send that file and we can tell you why.",
                    path.display()
                ),
                Err(e) => format!("{what} FAILED, and the log could not be written either: {e}"),
            }
        } else {
            format!("{what} finished. Nothing failed.")
        };
        self.report = Some(report);
    }
}

/// Beside the installer if that is writable, otherwise the working directory. Findable either
/// way, and the path is always shown on screen.
fn write_log(report: &Report, preset: Preset, dir: &str) -> io::Result<PathBuf> {
    let secs = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let header = format!(
        "installer {} | preset {} | target {} | unix {}",
        env!("CARGO_PKG_VERSION"),
        preset.label(),
        dir,
        secs
    );
    let body = report.to_log(&header);

    if let Some(path) =
        std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.join("dlss5-installer.log")))
    {
        if std::fs::write(&path, &body).is_ok() {
            return Ok(path);
        }
    }
    let fallback = PathBuf::from("dlss5-installer.log");
    std::fs::write(&fallback, &body)?;
    Ok(fallback)
}

fn main() -> io::Result<()> {
    crossterm::style::Colored::set_ansi_color_disabled(false);
    if std::env::args().any(|a| a == "--diag") {
        return diag::run(enable_colour);
    }
    enable_colour();
    let mut terminal = setup()?;
    let mut app = App::new();
    let result = event_loop(&mut terminal, &mut app);
    restore(&mut terminal)?;
    result
}

fn setup() -> io::Result<Terminal<CrosstermBackend<Stdout>>> {
    enable_raw_mode()?;
    let mut out = io::stdout();
    // Deliberately NOT EnterAlternateScreen: on Windows crossterm implements it with a real
    // second Win32 screen buffer, and drawing on the main one costs only the scrollback, which is
    // cleared on the way out anyway.
    execute!(out, EnableBracketedPaste, Clear(ClearType::All))?;
    enable_colour();
    Terminal::new(CrosstermBackend::new(out))
}

fn restore(terminal: &mut Terminal<CrosstermBackend<Stdout>>) -> io::Result<()> {
    disable_raw_mode()?;
    execute!(terminal.backend_mut(), DisableBracketedPaste, Clear(ClearType::All))?;
    terminal.set_cursor_position((0, 0))?;
    terminal.show_cursor()
}

fn event_loop(terminal: &mut Terminal<CrosstermBackend<Stdout>>, app: &mut App) -> io::Result<()> {
    while !app.quit {
        terminal.draw(|f| ui::draw(f, app))?;
        match event::read()? {
            Event::Paste(text) => {
                let cleaned: String = text.chars().filter(|c| !c.is_control()).collect();
                if let Some(field) = app.field_mut() {
                    field.push_str(cleaned.trim());
                }
            }
            Event::Key(key) if key.kind == KeyEventKind::Press => on_key(app, key),
            _ => {}
        }
        // After every event, not on a timer: the answer only changes when a path changes or when
        // the user goes and closes the game, and either way there is a keystroke in between.
        app.recheck();
    }
    Ok(())
}

fn on_key(app: &mut App, key: event::KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    match key.code {
        KeyCode::Esc => app.quit = true,
        KeyCode::Char('c' | 'q') if ctrl => app.quit = true,
        KeyCode::Tab | KeyCode::Down | KeyCode::Enter => app.next_focus(false),
        KeyCode::BackTab | KeyCode::Up => app.next_focus(true),
        KeyCode::F(5) => app.run(false),
        KeyCode::F(8) => app.run(true),
        KeyCode::Left if app.focus == Focus::Preset => {
            app.preset = (app.preset + Preset::ALL.len() - 1) % Preset::ALL.len();
        }
        KeyCode::Right if app.focus == Focus::Preset => {
            app.preset = (app.preset + 1) % Preset::ALL.len();
        }
        // Faster than holding backspace over a long path.
        KeyCode::Char('u') if ctrl => {
            if let Some(field) = app.field_mut() {
                field.clear();
            }
        }
        KeyCode::Backspace => {
            if let Some(field) = app.field_mut() {
                field.pop();
            }
        }
        KeyCode::Char(c) if !ctrl => {
            if let Some(field) = app.field_mut() {
                field.push(c);
            }
        }
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossterm::event::KeyEvent;

    fn press(app: &mut App, code: KeyCode) {
        on_key(app, KeyEvent::new(code, KeyModifiers::NONE));
    }
    fn ctrl(app: &mut App, c: char) {
        on_key(app, KeyEvent::new(KeyCode::Char(c), KeyModifiers::CONTROL));
    }

    /// Tab walks the three regions in screen order and comes back round, and shift-tab walks it
    /// backwards. Worth pinning because `next_focus` leans on `%` binding to the whole `if`
    /// expression, which is easy to read wrong and would show up as a focus that skips a field.
    #[test]
    fn focus_cycles_both_ways_in_screen_order() {
        let mut app = App::new();
        assert!(app.focus == Focus::RuntimeDir, "the first field is the one nobody has yet");

        press(&mut app, KeyCode::Tab);
        assert!(app.focus == Focus::GameDir);
        press(&mut app, KeyCode::Tab);
        assert!(app.focus == Focus::Preset);
        press(&mut app, KeyCode::Tab);
        assert!(app.focus == Focus::RuntimeDir, "tab wrapped");

        press(&mut app, KeyCode::BackTab);
        assert!(app.focus == Focus::Preset);
        press(&mut app, KeyCode::BackTab);
        assert!(app.focus == Focus::GameDir);
        press(&mut app, KeyCode::BackTab);
        assert!(app.focus == Focus::RuntimeDir, "shift-tab wrapped the other way");
    }

    /// Left and right only mean anything on the target row, and they wrap -- including left from
    /// the first entry, which is the one that underflows if the modulo is written carelessly.
    #[test]
    fn presets_wrap_and_only_move_when_the_target_row_has_focus() {
        let mut app = App::new();
        press(&mut app, KeyCode::Right);
        assert_eq!(app.preset, 0, "arrows must not change the target while a path field is up");

        app.focus = Focus::Preset;
        press(&mut app, KeyCode::Left);
        assert_eq!(app.preset, Preset::ALL.len() - 1, "left from the first wraps to the last");
        press(&mut app, KeyCode::Right);
        assert_eq!(app.preset, 0, "and back");

        for i in 0..Preset::ALL.len() {
            assert_eq!(app.preset, i);
            press(&mut app, KeyCode::Right);
        }
        assert_eq!(app.preset, 0, "every preset is reachable and the row is a ring");
    }

    /// Typing and the two editing keys go to whichever field has focus, and nowhere when the
    /// target row does.
    #[test]
    fn typing_lands_in_the_focused_field_only() {
        let mut app = App::new();
        for c in "D:\\game".chars() {
            press(&mut app, KeyCode::Char(c));
        }
        assert_eq!(app.runtime_dir, "D:\\game");
        assert_eq!(app.game_dir, "");

        press(&mut app, KeyCode::Backspace);
        assert_eq!(app.runtime_dir, "D:\\gam");
        ctrl(&mut app, 'u');
        assert_eq!(app.runtime_dir, "");

        app.focus = Focus::Preset;
        press(&mut app, KeyCode::Char('x'));
        ctrl(&mut app, 'u');
        assert_eq!(app.runtime_dir, "", "the target row has no field to type into");
        assert_eq!(app.game_dir, "");
    }

    /// The three states of the first-run line, in the order someone walks through them.
    #[test]
    fn the_hint_leads_with_the_files_nobody_has_yet() {
        let mut app = App::new();
        assert!(app.hint().contains("#files"), "{}", app.hint());
        app.runtime_dir = "D:\\runtime".into();
        assert!(app.hint().contains("game or emulator"), "{}", app.hint());
        app.game_dir = "D:\\game".into();
        assert!(app.hint().contains("F5"), "{}", app.hint());
    }

    /// Esc and both quit chords actually set the flag the loop reads.
    #[test]
    fn every_advertised_way_out_works() {
        for build in [
            &(|a: &mut App| press(a, KeyCode::Esc)) as &dyn Fn(&mut App),
            &|a: &mut App| ctrl(a, 'c'),
            &|a: &mut App| ctrl(a, 'q'),
        ] {
            let mut app = App::new();
            build(&mut app);
            assert!(app.quit);
        }
    }
}

