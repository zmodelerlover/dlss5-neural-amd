//! The DLSS5-NR-AMD wordmark, for the top of the screen.
//!
//! Two sizes, chosen by the window: the block-letter one when there is room for 97 columns and six
//! rows, and a half-height version built from upper/lower block glyphs at 47 columns when there is
//! not. A logo that pushes a field off the screen is worse than no logo, so below either threshold
//! it simply is not drawn.

pub const TALL: [&str; 6] = [
    r"██████╗ ██╗     ███████╗███████╗███████╗      ███╗   ██╗██████╗        █████╗ ███╗   ███╗██████╗ ",
    r"██╔══██╗██║     ██╔════╝██╔════╝██╔════╝      ████╗  ██║██╔══██╗      ██╔══██╗████╗ ████║██╔══██╗",
    r"██║  ██║██║     ███████╗███████╗███████╗█████╗██╔██╗ ██║██████╔╝█████╗███████║██╔████╔██║██║  ██║",
    r"██║  ██║██║     ╚════██║╚════██║╚════██║╚════╝██║╚██╗██║██╔══██╗╚════╝██╔══██║██║╚██╔╝██║██║  ██║",
    r"██████╔╝███████╗███████║███████║███████║      ██║ ╚████║██║  ██║      ██║  ██║██║ ╚═╝ ██║██████╔╝",
    r"╚═════╝ ╚══════╝╚══════╝╚══════╝╚══════╝      ╚═╝  ╚═══╝╚═╝  ╚═╝      ╚═╝  ╚═╝╚═╝     ╚═╝╚═════╝ ",
];

pub const SHORT: [&str; 2] = [
    r"█▀▄ █   █▀▀ █▀▀ █▀▀    █▄█ █▀▄    ▄▀█ █▀▄▀█ █▀▄",
    r"█▄▀ █▄▄ ▄▄█ ▄▄█ ▄▄█ ▀▀ █ █ █▀▄ ▀▀ █▀█ █ ▀ █ █▄▀",
];

const TALL_COLS: u16 = 97;
const SHORT_COLS: u16 = 47;

/// How many rows the logo will take in this window, 0 when there is no room to spare.
/// The panes below need 15 rows between them plus something for the output, so the banner only
/// gets what is left over.
pub fn rows_for(width: u16, height: u16) -> u16 {
    if width >= TALL_COLS + 2 && height >= 30 {
        TALL.len() as u16
    } else if width >= SHORT_COLS + 2 && height >= 26 {
        SHORT.len() as u16
    } else {
        0
    }
}

pub fn lines(width: u16, height: u16) -> &'static [&'static str] {
    match rows_for(width, height) {
        6 => &TALL,
        2 => &SHORT,
        _ => &[],
    }
}
