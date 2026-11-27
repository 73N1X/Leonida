//! Leonida's in-game trainer menu (`LEONIDA_MENU=0` turns it off; F4, or
//! RB with d-pad Left, opens and closes it).
//!
//! - [`model`]: pages, items, the cursor and its back stack.
//! - [`tree`]: Leonida's pages and the backend behind them.
//! - [`actions`]: the features that need the title's scripts (none can be
//!   done yet; `docs/mod-menu.md`).
//! - [`knobs`]: the run-time settings the rest of the runtime reads.
//! - [`input`]: keys and pad buttons to presses, and what is kept from the
//!   title while the menu is open.
//! - [`font`], `gdi`, [`paint`]: glyphs from the system's fonts and the
//!   menu painted on the CPU; the GPU side lays it over the frame at
//!   present (`leonida-gpu`, `host/menu.rs`).
//! - [`png`]: an encoder for screenshots.
//!
//! Single player only: nothing here reaches other players or anything
//! online.

pub mod actions;
mod app;
pub mod font;
#[cfg(windows)]
pub mod gdi;
pub mod input;
pub mod knobs;
pub mod model;
pub mod paint;
pub mod png;
#[cfg(windows)]
mod poll;
pub mod tree;

pub use app::{
    App, Frame, VISIBLE, enabled, frame, is_open, preview, scale_for, start, system_fonts,
    withheld_key, withheld_pad,
};
