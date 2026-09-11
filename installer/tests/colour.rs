//! The one check that would have caught the bug that cost a session: the backend writing escape
//! sequences with no colour in them.
//!
//! With NO_COLOR set, crossterm's `Display for Colored` returns an empty string, so `SetColors`
//! emits `ESC [ ; m` -- a well-formed sequence that selects nothing. The screen then draws its
//! full layout in white while every probe says colour is supported. main() opts out of that;
//! this pins the opt-out, so a crossterm upgrade that renames or drops it fails here instead of
//! on someone's desktop.

use ratatui::backend::{Backend, CrosstermBackend};
use ratatui::buffer::Buffer;
use ratatui::layout::Rect;
use ratatui::style::{Color, Style};
use ratatui::widgets::{Paragraph, Widget};
use std::cell::RefCell;
use std::io::Write;
use std::rc::Rc;

struct Sink(Rc<RefCell<Vec<u8>>>);

impl Write for Sink {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.0.borrow_mut().extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

#[test]
fn colour_survives_no_color() {
    std::env::set_var("NO_COLOR", "1");
    crossterm::style::Colored::set_ansi_color_disabled(false); // what main() does

    let area = Rect::new(0, 0, 8, 1);
    let mut buffer = Buffer::empty(area);
    Paragraph::new("AMD").style(Style::default().fg(Color::Rgb(237, 28, 36))).render(area, &mut buffer);

    let sink = Rc::new(RefCell::new(Vec::new()));
    let mut backend = CrosstermBackend::new(Sink(Rc::clone(&sink)));
    let cells: Vec<_> = buffer.content.iter().enumerate().map(|(i, c)| (i as u16, 0, c)).collect();
    backend.draw(cells.into_iter()).unwrap();
    Backend::flush(&mut backend).unwrap();

    let written = String::from_utf8(sink.borrow().clone()).unwrap();
    assert!(
        written.contains("38;2;237;28;36"),
        "backend wrote no foreground colour: {written:?}"
    );
}
