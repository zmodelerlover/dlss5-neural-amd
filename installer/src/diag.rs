//! `--diag`: what this console actually reports, written to a file and printed.
//!
//! Guessing at why a terminal shows no colour is how an afternoon disappears. This turns it into
//! one readable file that can be sent over, the same way the install log is.

use std::io;
use std::path::PathBuf;

pub fn run(enable_colour: fn()) -> io::Result<()> {
    let mut out = String::new();
    out.push_str("dlss5-installer terminal diagnostic\n\n");
    out.push_str(&format!("version          {}\n", env!("CARGO_PKG_VERSION")));

    match crossterm::terminal::size() {
        Ok((w, h)) => out.push_str(&format!("terminal size    {w} x {h}\n")),
        Err(e) => out.push_str(&format!("terminal size    FAILED: {e}\n")),
    }
    out.push_str(&format!("colour count     {}\n", crossterm::style::available_color_count()));

    // NO_COLOR first, and named for what it does: set non-empty, crossterm writes every colour
    // escape with no parameters, and the whole screen draws in white with nothing else looking
    // wrong. The installer overrides it, but a console that has it set is still worth knowing
    // about when someone reports a washed-out screen.
    out.push('\n');
    let no_color = std::env::var("NO_COLOR").unwrap_or_default();
    out.push_str(&format!(
        "NO_COLOR          {}\n",
        if no_color.is_empty() { "<unset>  (colour on)".into() } else { format!("{no_color:?}  -- set, overridden by this installer") }
    ));

    for key in ["WT_SESSION", "TERM", "TERM_PROGRAM", "COLORTERM", "ConEmuANSI", "ANSICON"] {
        let value = std::env::var(key).unwrap_or_else(|_| "<unset>".into());
        out.push_str(&format!("{key:<16} {value}\n"));
    }

    #[cfg(windows)]
    {
        out.push('\n');
        out.push_str(&format!("console mode before  {}\n", mode_line()));
        enable_colour();
        out.push_str(&format!("console mode after   {}\n", mode_line()));
    }
    #[cfg(not(windows))]
    let _ = enable_colour;

    // Printed raw. If the console understands these, the next line is coloured; if it does not,
    // the escape codes appear as text -- and either answer is the one worth having.
    out.push_str("\nescape test (should be green, then red, then normal):\n");
    out.push_str("\x1b[32mGREEN\x1b[0m \x1b[31mRED\x1b[0m plain\n");

    let path = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("dlss5-installer-diag.txt")))
        .unwrap_or_else(|| PathBuf::from("dlss5-installer-diag.txt"));
    std::fs::write(&path, &out)?;

    print!("{out}");
    println!("\nwritten to {}", path.display());
    println!("press Enter to close");
    let mut line = String::new();
    let _ = io::stdin().read_line(&mut line);
    Ok(())
}

#[cfg(windows)]
fn mode_line() -> String {
    use windows_sys::Win32::System::Console::{GetConsoleMode, GetStdHandle, STD_OUTPUT_HANDLE};
    unsafe {
        let handle = GetStdHandle(STD_OUTPUT_HANDLE);
        let mut mode = 0u32;
        if GetConsoleMode(handle, &mut mode) == 0 {
            return "GetConsoleMode failed -- stdout is not a console (redirected?)".into();
        }
        // 0x0004 is ENABLE_VIRTUAL_TERMINAL_PROCESSING: without it every colour escape this
        // program writes is either printed literally or dropped.
        format!(
            "0x{mode:08X}  VT processing {}",
            if mode & 0x0004 != 0 { "ON" } else { "OFF" }
        )
    }
}
