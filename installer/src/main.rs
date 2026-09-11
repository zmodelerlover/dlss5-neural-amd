//! Terminal installer for the dlss5-neural-amd ReShade add-on.
//!
//! One screen, three fields, two keys. Paste a folder, pick what it is, press F5.
//!
//! The add-on is compiled into this executable. The runtime and the weights are not and never
//! will be -- the weights are NVIDIA-derived and the runtime is a third-party build -- so the
//! user points at the folder they unzipped them into, and every byte is checked against a known
//! SHA-256 before it is copied anywhere.

// A TUI has no console window to print to, and opening one behind it is worse than useless.
#![windows_subsystem = "console"]

mod work;

use crossterm::event::{
    self, DisableBracketedPaste, EnableBracketedPaste, Event, KeyCode, KeyEventKind, KeyModifiers,
};
use crossterm::execute;
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use ratatui::backend::CrosstermBackend;
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph, Wrap};
use ratatui::{Frame, Terminal};
use std::io::{self, Stdout};
use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};
use work::{Level, Preset, Report};

#[derive(Clone, Copy, PartialEq, Eq)]
enum Focus {
    Preset,
    GameDir,
    RuntimeDir,
}

struct App {
    preset: usize,
    game_dir: String,
    runtime_dir: String,
    focus: Focus,
    report: Option<Report>,
    status: String,
    quit: bool,
}

impl App {
    fn new() -> Self {
        App {
            preset: 0,
            game_dir: String::new(),
            runtime_dir: String::new(),
            focus: Focus::GameDir,
            report: None,
            status: "Paste the game folder, pick what it is, then F5.".into(),
            quit: false,
        }
    }

    fn preset(&self) -> Preset {
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
        self.focus = match (self.focus, back) {
            (Focus::Preset, false) => Focus::GameDir,
            (Focus::GameDir, false) => Focus::RuntimeDir,
            (Focus::RuntimeDir, false) => Focus::Preset,
            (Focus::Preset, true) => Focus::RuntimeDir,
            (Focus::GameDir, true) => Focus::Preset,
            (Focus::RuntimeDir, true) => Focus::GameDir,
        };
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
                    "{what} FAILED. Log written to {} -- send that file and we can tell you why.",
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

/// Beside the installer if that is writable, otherwise the working directory. Somewhere the
/// person can actually find it either way, and the path is always printed.
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

    let beside = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("dlss5-installer.log")));
    if let Some(path) = beside {
        if std::fs::write(&path, &body).is_ok() {
            return Ok(path);
        }
    }
    let fallback = PathBuf::from("dlss5-installer.log");
    std::fs::write(&fallback, &body)?;
    Ok(fallback)
}

fn main() -> io::Result<()> {
    let mut terminal = setup()?;
    let mut app = App::new();
    let result = event_loop(&mut terminal, &mut app);
    restore(&mut terminal)?;
    result
}

fn setup() -> io::Result<Terminal<CrosstermBackend<Stdout>>> {
    enable_raw_mode()?;
    let mut out = io::stdout();
    // Bracketed paste is the whole point: a pasted path arrives as one event instead of as a
    // burst of keystrokes the loop would have to reassemble.
    execute!(out, EnterAlternateScreen, EnableBracketedPaste)?;
    Terminal::new(CrosstermBackend::new(out))
}

fn restore(terminal: &mut Terminal<CrosstermBackend<Stdout>>) -> io::Result<()> {
    disable_raw_mode()?;
    execute!(terminal.backend_mut(), DisableBracketedPaste, LeaveAlternateScreen)?;
    terminal.show_cursor()
}

fn event_loop(
    terminal: &mut Terminal<CrosstermBackend<Stdout>>,
    app: &mut App,
) -> io::Result<()> {
    while !app.quit {
        terminal.draw(|f| draw(f, app))?;
        match event::read()? {
            Event::Paste(text) => {
                let cleaned: String =
                    text.chars().filter(|c| !c.is_control()).collect::<String>();
                if let Some(field) = app.field_mut() {
                    field.push_str(cleaned.trim());
                }
            }
            Event::Key(key) if key.kind == KeyEventKind::Press => on_key(app, key),
            _ => {}
        }
    }
    Ok(())
}

fn on_key(app: &mut App, key: event::KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    match key.code {
        KeyCode::Esc => app.quit = true,
        KeyCode::Char('c') if ctrl => app.quit = true,
        KeyCode::Char('q') if ctrl => app.quit = true,
        KeyCode::Tab => app.next_focus(false),
        KeyCode::BackTab => app.next_focus(true),
        KeyCode::F(5) => app.run(false),
        KeyCode::F(8) => app.run(true),
        KeyCode::Left if app.focus == Focus::Preset => {
            app.preset = (app.preset + Preset::ALL.len() - 1) % Preset::ALL.len();
        }
        KeyCode::Right if app.focus == Focus::Preset => {
            app.preset = (app.preset + 1) % Preset::ALL.len();
        }
        // Ctrl+U clears a field, which is faster than holding backspace over a long path.
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

fn draw(f: &mut Frame, app: &App) {
    let area = f.area();
    let rows = Layout::vertical([
        Constraint::Length(3),  // preset
        Constraint::Length(3),  // game dir
        Constraint::Length(3),  // runtime dir
        Constraint::Length(4),  // preset note
        Constraint::Min(5),     // log
        Constraint::Length(3),  // status
        Constraint::Length(1),  // keys
    ])
    .split(area);

    draw_preset(f, rows[0], app);
    draw_field(f, rows[1], "Game folder", &app.game_dir, app.focus == Focus::GameDir);
    draw_field(
        f,
        rows[2],
        "Runtime folder  (dlssnr_amd_pass1.dll + dlssnr_on_amd_weights.bin)",
        &app.runtime_dir,
        app.focus == Focus::RuntimeDir,
    );
    draw_note(f, rows[3], app);
    draw_log(f, rows[4], app);
    draw_status(f, rows[5], app);
    draw_keys(f, rows[6]);
}

fn bordered(title: &str, focused: bool) -> Block<'_> {
    let style = if focused {
        Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)
    } else {
        Style::default().fg(Color::DarkGray)
    };
    Block::default().borders(Borders::ALL).border_style(style).title(Span::styled(
        format!(" {title} "),
        if focused { style } else { Style::default().fg(Color::Gray) },
    ))
}

fn draw_preset(f: &mut Frame, area: Rect, app: &App) {
    let focused = app.focus == Focus::Preset;
    let mut spans = Vec::new();
    for (i, p) in Preset::ALL.iter().enumerate() {
        let selected = i == app.preset;
        let style = if selected {
            Style::default().fg(Color::Black).bg(Color::Cyan).add_modifier(Modifier::BOLD)
        } else {
            Style::default().fg(Color::Gray)
        };
        spans.push(Span::styled(format!(" {} ", p.label()), style));
        spans.push(Span::raw("  "));
    }
    let title = if focused { "Target   (left/right to change)" } else { "Target" };
    f.render_widget(Paragraph::new(Line::from(spans)).block(bordered(title, focused)), area);
}

fn draw_field(f: &mut Frame, area: Rect, title: &str, value: &str, focused: bool) {
    let shown = if value.is_empty() {
        Span::styled("paste a path here", Style::default().fg(Color::DarkGray))
    } else {
        Span::styled(value, Style::default().fg(Color::White))
    };
    let mut line = vec![shown];
    if focused {
        line.push(Span::styled("_", Style::default().fg(Color::Cyan)));
    }
    f.render_widget(Paragraph::new(Line::from(line)).block(bordered(title, focused)), area);
}

fn draw_note(f: &mut Frame, area: Rect, app: &App) {
    let note = Paragraph::new(app.preset().note())
        .style(Style::default().fg(Color::Yellow))
        .wrap(Wrap { trim: true })
        .block(bordered("About this target", false));
    f.render_widget(note, area);
}

fn draw_log(f: &mut Frame, area: Rect, app: &App) {
    let lines: Vec<Line> = match &app.report {
        None => vec![Line::from(Span::styled(
            "Nothing run yet. F5 installs, F8 uninstalls.",
            Style::default().fg(Color::DarkGray),
        ))],
        Some(report) => report
            .lines
            .iter()
            .map(|(level, text)| {
                let (tag, colour) = match level {
                    Level::Ok => ("  ok  ", Color::Green),
                    Level::Warn => (" warn ", Color::Yellow),
                    Level::Err => (" ERR  ", Color::Red),
                    Level::Info => ("      ", Color::Gray),
                };
                Line::from(vec![
                    Span::styled(tag, Style::default().fg(colour).add_modifier(Modifier::BOLD)),
                    Span::styled(text.replace('\n', " "), Style::default().fg(colour)),
                ])
            })
            .collect(),
    };
    // Show the tail: the end is where the verdict is, and a failure is always last.
    let height = area.height.saturating_sub(2) as usize;
    let start = lines.len().saturating_sub(height.max(1));
    let view: Vec<Line> = lines[start..].to_vec();
    f.render_widget(
        Paragraph::new(view).wrap(Wrap { trim: true }).block(bordered("Log", false)),
        area,
    );
}

fn draw_status(f: &mut Frame, area: Rect, app: &App) {
    let failed = app.report.as_ref().map(|r| r.failed).unwrap_or(false);
    let colour = if failed { Color::Red } else if app.report.is_some() { Color::Green } else { Color::Gray };
    f.render_widget(
        Paragraph::new(app.status.as_str())
            .style(Style::default().fg(colour).add_modifier(Modifier::BOLD))
            .wrap(Wrap { trim: true })
            .block(bordered("Status", false)),
        area,
    );
}

fn draw_keys(f: &mut Frame, area: Rect) {
    let key = Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD);
    let txt = Style::default().fg(Color::DarkGray);
    f.render_widget(
        Paragraph::new(Line::from(vec![
            Span::styled("  Tab", key),
            Span::styled(" next field   ", txt),
            Span::styled("Ctrl+V", key),
            Span::styled(" paste   ", txt),
            Span::styled("Ctrl+U", key),
            Span::styled(" clear   ", txt),
            Span::styled("F5", key),
            Span::styled(" install   ", txt),
            Span::styled("F8", key),
            Span::styled(" uninstall   ", txt),
            Span::styled("Esc", key),
            Span::styled(" quit", txt),
        ])),
        area,
    );
}
