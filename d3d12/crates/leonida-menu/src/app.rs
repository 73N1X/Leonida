//! The running menu: a thread of its own reads the keys and the pads,
//! moves the menu and paints it when it changes; the present takes the
//! newest picture ([`frame`]). Closed, the present pays one atomic load
//! and the thread looks at F4 and the pads sixty times a second.

use std::sync::atomic::{AtomicBool, AtomicU16, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, Once, OnceLock};
use std::time::{Duration, Instant};

use crate::actions::NotYet;
use crate::font::FontSource;
use crate::input::{Held, Keys, Withheld};
use crate::knobs;
use crate::model::{Input, Menu, View};
use crate::paint::{Fonts, Layers, paint};
use crate::tree::{Leonida, pages};

/// Rows shown at once.
pub const VISIBLE: usize = 11;

/// Whether the menu is on: unless `LEONIDA_MENU=0`.
pub fn enabled() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_MENU").is_none_or(|v| v != "0"))
}

static OPEN: AtomicBool = AtomicBool::new(false);
/// Keys (bits of `input::KEYS`) and pad buttons (XInput's) kept from the title.
static WITHHELD_KEYS: AtomicU16 = AtomicU16::new(0);
static WITHHELD_PAD: AtomicU16 = AtomicU16::new(0);
/// The height of the picture presented, for the menu's scale.
static HEIGHT: AtomicU32 = AtomicU32::new(1080);

/// The newest picture of the menu and its number.
static PAINTED: Mutex<(u64, Option<Arc<Layers>>)> = Mutex::new((0, None));

/// Whether the menu is open.
pub fn is_open() -> bool {
    OPEN.load(Ordering::Relaxed)
}

/// Whether the title must not see virtual key `vk` now.
pub fn withheld_key(vk: u8) -> bool {
    let mask = WITHHELD_KEYS.load(Ordering::Relaxed);
    mask != 0 && crate::input::withheld(mask, vk)
}

/// The XInput buttons the title must not see now.
pub fn withheld_pad() -> u16 {
    WITHHELD_PAD.load(Ordering::Relaxed)
}

/// What a present draws.
pub struct Frame {
    /// Changes whenever the picture does.
    pub version: u64,
    pub layers: Arc<Layers>,
}

/// Called by every present with its picture's height: the menu's picture
/// while it is open. Starts the menu's thread on the first call.
pub fn frame(height: u32) -> Option<Frame> {
    if !enabled() {
        return None;
    }
    start();
    if HEIGHT.load(Ordering::Relaxed) != height {
        HEIGHT.store(height, Ordering::Relaxed);
    }
    if !is_open() {
        return None;
    }
    let painted = PAINTED.lock().unwrap_or_else(|e| e.into_inner());
    let layers = painted.1.clone()?;
    Some(Frame {
        version: painted.0,
        layers,
    })
}

/// The scale for a picture `height` lines high.
pub fn scale_for(height: u32) -> f32 {
    (height.max(1) as f32 / 1080.0 * knobs::MENU_SCALE.get()).max(0.25)
}

/// Starts the menu's thread (once; nothing when disabled or off Windows).
pub fn start() {
    static STARTED: Once = Once::new();
    if !enabled() {
        return;
    }
    STARTED.call_once(|| {
        #[cfg(windows)]
        {
            let _ = std::thread::Builder::new()
                .name("leonida-menu".into())
                .spawn(run);
        }
    });
}

/// The menu, its backend and the fonts at the scale painted last.
pub struct App {
    pub menu: Menu,
    pub backend: Leonida,
    fonts: Option<Fonts>,
    source: Box<dyn FontSource + Send>,
    last: Option<(View, f32)>,
    painted_at: Option<Instant>,
}

impl App {
    pub fn new(source: Box<dyn FontSource + Send>) -> App {
        App {
            menu: Menu::new(pages(), VISIBLE),
            backend: Leonida::new(Box::new(NotYet)),
            fonts: None,
            source,
            last: None,
            painted_at: None,
        }
    }

    /// Applies `input`; whether something changed.
    pub fn handle(&mut self, input: Input) -> bool {
        self.menu.handle(input, &mut self.backend)
    }

    /// A new picture if the view or the scale changed since the last.
    pub fn repaint(&mut self, scale: f32) -> Option<Layers> {
        let view = self.menu.view(&self.backend);
        if self
            .last
            .as_ref()
            .is_some_and(|(v, s)| *v == view && *s == scale)
        {
            return None;
        }
        if self.fonts.as_ref().is_none_or(|f| f.scale != scale) {
            self.fonts = Some(Fonts::new(&*self.source, scale));
        }
        let layers = paint(&view, self.fonts.as_ref()?);
        self.last = Some((view, scale));
        self.painted_at = Some(Instant::now());
        Some(layers)
    }
}

/// The system's fonts where there are, else the dot font.
pub fn system_fonts() -> Box<dyn FontSource + Send> {
    #[cfg(windows)]
    return Box::new(crate::gdi::Gdi);
    #[cfg(not(windows))]
    Box::new(crate::font::Dots)
}

/// A picture of the menu after `inputs` (it is opened first), at `scale`:
/// for previews.
pub fn preview(inputs: &[Input], scale: f32) -> Layers {
    let mut app = App::new(system_fonts());
    app.handle(Input::Toggle);
    for &i in inputs {
        app.handle(i);
    }
    app.repaint(scale).expect("a first picture")
}

/// The thread: samples, moves, paints.
#[cfg(windows)]
fn run() {
    let mut app = App::new(system_fonts());
    let mut keys = Keys::default();
    let mut withheld = Withheld::default();
    let mut pads = crate::poll::Pads::default();
    let mut version = 0u64;
    loop {
        let now = Instant::now();
        let focused = crate::poll::focused();
        let pad = pads.buttons();
        let held = if focused {
            Held::of_keys(crate::poll::held_keys())
        } else {
            Held::default()
        };
        let held = held.union(Held::of_pad(pad));
        let mut changed = false;
        for input in keys.step(held, now) {
            changed |= app.handle(input);
        }
        let open = app.menu.is_open();
        OPEN.store(open, Ordering::Relaxed);
        let mask = withheld.update(open, |vk| focused && crate::poll::down(vk));
        WITHHELD_KEYS.store(mask, Ordering::Relaxed);
        WITHHELD_PAD.store(
            if open { crate::input::PAD_MENU } else { 0 },
            Ordering::Relaxed,
        );
        if open {
            // Things change behind the menu too (F3, a screenshot saved):
            // looked at twice a second.
            let stale = app
                .painted_at
                .is_none_or(|t| t.elapsed() >= Duration::from_millis(500));
            if changed
                || stale
                || app
                    .last
                    .as_ref()
                    .is_some_and(|(_, s)| *s != scale_for(HEIGHT.load(Ordering::Relaxed)))
            {
                if let Some(layers) = app.repaint(scale_for(HEIGHT.load(Ordering::Relaxed))) {
                    version += 1;
                    *PAINTED.lock().unwrap_or_else(|e| e.into_inner()) =
                        (version, Some(Arc::new(layers)));
                } else {
                    app.painted_at = Some(Instant::now());
                }
            }
        }
        std::thread::sleep(Duration::from_millis(if open { 8 } else { 16 }));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_preview_is_painted_and_repaints_only_on_change() {
        let mut app = App::new(Box::new(crate::font::Dots));
        app.handle(Input::Toggle);
        assert!(app.repaint(1.0).is_some());
        assert!(app.repaint(1.0).is_none());
        app.handle(Input::Down);
        let l = app.repaint(1.0).unwrap();
        assert_eq!((l.highlight, l.rows), (1, 9));
        assert!(app.repaint(1.5).is_some(), "a new scale paints again");
    }

    #[test]
    fn scale_follows_the_picture() {
        let _one = knobs::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        knobs::MENU_SCALE.set(1.0);
        assert_eq!(scale_for(2160), 2.0);
        assert_eq!(scale_for(1080), 1.0);
    }
}
