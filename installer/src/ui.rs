//! The screen, in the visual language of openapi-tui: every region is a titled `Borders::ALL`
//! pane, the focused one is drawn with a thick border while the rest stay plain, a right-aligned
//! `[ name · version ]` header, and one status line at the bottom.
//!
//! The palette is AMD red on black, with white and grey for the text. Border weight also carries
//! the focus, not colour alone, so the screen still reads on a console with a washed-out palette.

use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::symbols;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Paragraph, Wrap};
use ratatui::Frame;

use crate::work::{Level, Preset};
use crate::{App, Focus};

// --- AMD palette ------------------------------------------------------------------------------
// AMD's ED1C24 at full intensity blows out on a dark terminal -- a wall of pure saturated red is
// painful to sit in front of. These are the same hue held back: the accent at about two thirds,
// the chrome at a third, which reads as AMD without glaring.
const RED: Color = Color::Rgb(166, 28, 34);
const RED_DIM: Color = Color::Rgb(96, 26, 30);
const TEXT: Color = Color::Rgb(228, 228, 228);
const MUTED: Color = Color::Rgb(128, 128, 128);
const FAINT: Color = Color::Rgb(90, 90, 90);
// Install results keep their conventional colours: a green OK and a red FAIL are information, not
// decoration, and repainting them all red would cost the one distinction that matters most.
const OK: Color = Color::Rgb(80, 200, 120);
const WARN: Color = Color::Rgb(230, 180, 60);
const FAIL: Color = RED;

fn pane(title: &str, focused: bool) -> Block<'_> {
    let (border, title_style) = if focused {
        (RED, Style::default().fg(RED).add_modifier(Modifier::BOLD))
    } else {
        (RED_DIM, Style::default().fg(MUTED))
    };
    Block::default()
        .title(Span::styled(title, title_style))
        .borders(Borders::ALL)
        .border_style(Style::default().fg(border))
        .border_type(if focused { BorderType::Thick } else { BorderType::Plain })
}

pub fn draw(f: &mut Frame, app: &App) {
    let area = f.area();
    if area.width < 64 || area.height < 24 {
        f.render_widget(
            Paragraph::new(format!(
                "\n Window is {}x{}; this needs 64x24.\n Make it bigger or use a smaller font.",
                area.width, area.height
            ))
            .style(Style::default().fg(WARN)),
            area,
        );
        return;
    }

    let banner = crate::logo::rows_for(area.width, area.height);
    let rows = Layout::vertical([
        Constraint::Max(banner), // wordmark, dropped entirely on a small window
        Constraint::Max(1),      // header
        Constraint::Max(3),      // target
        Constraint::Max(3),      // runtime folder
        Constraint::Max(3),      // game folder
        Constraint::Max(5),      // notes -- three text rows, because the shortest note needs them
        Constraint::Fill(1),     // output
        Constraint::Max(1),      // status
    ])
    .split(area);

    if banner > 0 {
        logo(f, rows[0], area.width, area.height);
    }
    header(f, rows[1]);
    target_pane(f, rows[2], app);
    // The two files are asked for first: without them the add-on does nothing, and they are the
    // part a newcomer does not have yet. Pointing at a game folder is the easy half.
    field_pane(
        f,
        rows[3],
        " 1. Runtime + weights  (from #files on the DLSS-NR-AMD Discord) ",
        &app.runtime_dir,
        app.focus == Focus::RuntimeDir,
    );
    field_pane(
        f,
        rows[4],
        &format!(" 2.{}", app.preset().folder_label()),
        &app.game_dir,
        app.focus == Focus::GameDir,
    );
    notes_pane(f, rows[5], app);
    output_pane(f, rows[6], app);
    status(f, rows[7], app);
}

fn logo(f: &mut Frame, area: Rect, width: u16, height: u16) {
    let lines: Vec<Line> = crate::logo::lines(width, height)
        .iter()
        .map(|row| {
            Line::from(Span::styled(*row, Style::default().fg(RED).add_modifier(Modifier::BOLD)))
        })
        .collect();
    f.render_widget(Paragraph::new(lines).centered(), area);
}

/// Keys on the left, name and version on the right. The key strip lives up here rather than on
/// the status line because the status line carries whole sentences -- the first-run instruction,
/// or the path of a failure log -- and the two were overrunning each other.
///
/// Every key is spelled out. `[^v]` and `[^u]` are how a terminal person writes Ctrl+V and
/// Ctrl+U, and this is aimed at someone who wants a mod installed, not at a terminal person.
fn header(f: &mut Frame, area: Rect) {
    let key = |s: &'static str| Span::styled(s, Style::default().fg(MUTED));
    // The long strip is 71 columns and the title on the right is 28, so it only goes out at 100
    // or wider. Below that the two verbs that matter keep their words and the rest shorten.
    let keys = if area.width >= 100 {
        vec![
            key("[tab move]"),
            key("[ctrl+v paste]"),
            key("[ctrl+u clear]"),
            Span::styled("[F5 install]", Style::default().fg(RED).add_modifier(Modifier::BOLD)),
            Span::styled("[F8 remove]", Style::default().fg(RED_DIM)),
            key("[esc quit]"),
        ]
    } else {
        vec![
            key("[tab]"),
            Span::styled("[F5 install]", Style::default().fg(RED).add_modifier(Modifier::BOLD)),
            Span::styled("[F8 remove]", Style::default().fg(RED_DIM)),
            key("[esc]"),
        ]
    };
    f.render_widget(Line::from(keys), area);
    f.render_widget(
        Line::from(vec![
            Span::styled("[ ", Style::default().fg(RED_DIM)),
            Span::styled("dlss5-neural-amd ", Style::default().fg(TEXT)),
            Span::styled(format!("{} ", symbols::DOT), Style::default().fg(RED_DIM)),
            Span::styled(
                format!("{} ", env!("CARGO_PKG_VERSION")),
                Style::default().fg(RED).add_modifier(Modifier::BOLD),
            ),
            Span::styled("]", Style::default().fg(RED_DIM)),
        ])
        .right_aligned(),
        area,
    );
}

fn target_pane(f: &mut Frame, area: Rect, app: &App) {
    let focused = app.focus == Focus::Preset;
    // The selection is marked three ways, so it survives any one of them failing: brackets in the
    // text itself, a filled background, and bold.
    let mut spans = vec![Span::raw(" ")];
    for (i, p) in Preset::ALL.iter().enumerate() {
        if i == app.preset {
            spans.push(Span::styled(
                format!("[{}]", p.label()),
                Style::default().fg(Color::Black).bg(RED).add_modifier(Modifier::BOLD),
            ));
        } else {
            spans.push(Span::styled(format!(" {} ", p.label()), Style::default().fg(MUTED)));
        }
        spans.push(Span::raw("  "));
    }
    let title = if focused {
        " Target  (left/right changes it) "
    } else {
        " Target  (Tab to reach it) "
    };
    f.render_widget(Paragraph::new(Line::from(spans)).block(pane(title, focused)), area);
}

fn field_pane(f: &mut Frame, area: Rect, title: &str, value: &str, focused: bool) {
    let empty_hint = if title.contains("Runtime") {
        "paste the folder holding dlssnr_amd_pass1.dll and dlssnr_on_amd_weights.bin  (Ctrl+V)"
    } else {
        "paste the folder the .exe runs from  (Ctrl+V)"
    };
    let inner_width = area.width.saturating_sub(4) as usize;
    let mut spans = if value.is_empty() {
        vec![Span::styled(empty_hint, Style::default().fg(FAINT))]
    } else {
        // Keep the tail of a long path visible: the folder name matters more than the drive.
        let count = value.chars().count();
        let shown = if count > inner_width {
            format!("...{}", value.chars().skip(count - inner_width + 3).collect::<String>())
        } else {
            value.to_string()
        };
        vec![Span::styled(shown, Style::default().fg(TEXT))]
    };
    if focused {
        spans.push(Span::styled("_", Style::default().fg(RED).add_modifier(Modifier::RAPID_BLINK)));
    }
    f.render_widget(Paragraph::new(Line::from(spans)).block(pane(title, focused)), area);
}

fn notes_pane(f: &mut Frame, area: Rect, app: &App) {
    f.render_widget(
        Paragraph::new(app.preset().note())
            .style(Style::default().fg(WARN))
            .wrap(Wrap { trim: true })
            .block(pane(" Notes ", false)),
        area,
    );
}

fn output_pane(f: &mut Frame, area: Rect, app: &App) {
    // Before anything is run the pane is not empty -- it carries the pre-flight, which answers
    // "will this work?" while the path is still being pasted. After F5 it carries what happened.
    let (title, report) = match &app.report {
        Some(report) => (" Output ", report),
        None => (" Before you press F5 ", &app.preflight),
    };
    let lines: Vec<Line> = report
            .lines
            .iter()
            .map(|(level, text)| {
                // The tag is readable with no colour at all; colour is the second signal.
                let (tag, colour) = match level {
                    Level::Ok => ("OK  ", OK),
                    Level::Warn => ("WARN", WARN),
                    Level::Err => ("FAIL", FAIL),
                    Level::Info => ("    ", MUTED),
                };
                Line::from(vec![
                    Span::raw(" "),
                    Span::styled(tag, Style::default().fg(colour).add_modifier(Modifier::BOLD)),
                    Span::raw(" "),
                    Span::styled(text.replace('\n', " "), Style::default().fg(colour)),
                ])
            })
            .collect();
    // The tail: a failure is always last, and the verdict is what matters.
    let height = area.height.saturating_sub(2) as usize;
    let start = lines.len().saturating_sub(height.max(1));
    f.render_widget(
        Paragraph::new(lines[start..].to_vec())
            .wrap(Wrap { trim: false })
            .block(pane(title, false)),
        area,
    );
}

fn status(f: &mut Frame, area: Rect, app: &App) {
    let (text, colour) = match &app.report {
        Some(r) if r.failed => (app.status.clone(), FAIL),
        Some(_) => (app.status.clone(), OK),
        // The pre-flight never blocks F5 -- it is not the installer's place to refuse -- but it
        // does get to say so plainly before 147 MB are copied into a folder that will not take
        // them.
        None if app.preflight.failed && !app.game_dir.trim().is_empty() => (
            "Something in the panel above will stop this. F5 still runs, and still writes a log."
                .into(),
            FAIL,
        ),
        None => (app.hint(), TEXT),
    };
    f.render_widget(Line::from(Span::styled(format!(" {text}"), Style::default().fg(colour))), area);
}
