//! Run-time settings the menu changes and the rest of the runtime reads:
//! plain atomics, so a reader pays a load. The GPU side reads the sharpen
//! strength, the frame-rate cap, the panel switch, the frame recorder and
//! asked-for screenshots; the HLE side reads the title's clock rate.

use std::path::PathBuf;
use std::sync::Mutex;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

/// An `f32` in an atomic.
pub struct F32(AtomicU32);

impl F32 {
    pub const fn new(v: f32) -> F32 {
        F32(AtomicU32::new(v.to_bits()))
    }
    pub fn get(&self) -> f32 {
        f32::from_bits(self.0.load(Ordering::Relaxed))
    }
    pub fn set(&self, v: f32) {
        self.0.store(v.to_bits(), Ordering::Relaxed);
    }
}

/// An on/off switch something else owns: it says it is there
/// ([`Switch::provide`]) and reads it every frame; until then the menu
/// shows it as unavailable.
pub struct Switch {
    there: AtomicBool,
    on: AtomicBool,
}

impl Switch {
    pub const fn new() -> Switch {
        Switch {
            there: AtomicBool::new(false),
            on: AtomicBool::new(false),
        }
    }
    /// The owner is there, and starts `on` (once; later calls change nothing).
    pub fn provide(&self, on: bool) {
        if !self.there() && !self.there.swap(true, Ordering::AcqRel) {
            self.on.store(on, Ordering::Relaxed);
        }
    }
    pub fn there(&self) -> bool {
        self.there.load(Ordering::Acquire)
    }
    pub fn get(&self) -> bool {
        self.on.load(Ordering::Relaxed)
    }
    pub fn set(&self, on: bool) {
        self.on.store(on, Ordering::Relaxed);
    }
    pub fn flip(&self) {
        self.on.fetch_xor(true, Ordering::Relaxed);
    }
}

impl Default for Switch {
    fn default() -> Self {
        Switch::new()
    }
}

/// Tests that change these settings take turns.
#[cfg(test)]
pub(crate) static TEST_LOCK: Mutex<()> = Mutex::new(());

/// The FPS and memory panel (diagnostics builds; F3 flips it too).
pub static PANEL: Switch = Switch::new();
/// The frame recorder (diagnostics builds with `LEONIDA_CAPTURE`): every
/// presented frame saved while on.
pub static RECORD: Switch = Switch::new();

/// How much the game plane is sharpened, 0 to 1: `LEONIDA_SHARPEN` at the
/// start (0.4 if unset), then what the menu sets.
pub fn sharpen() -> f32 {
    static INIT: OnceLock<()> = OnceLock::new();
    INIT.get_or_init(|| {
        let v = std::env::var("LEONIDA_SHARPEN")
            .ok()
            .and_then(|v| v.trim().parse::<f32>().ok())
            .filter(|v| v.is_finite())
            .map_or(0.4, |v| v.clamp(0.0, 1.0));
        SHARPEN.set(v);
    });
    SHARPEN.get()
}

static SHARPEN: F32 = F32::new(0.4);

pub fn set_sharpen(v: f32) {
    sharpen();
    SHARPEN.set(v.clamp(0.0, 1.0));
}

/// Frames a second at most (0: no cap): presents wait for their turn.
pub static FPS_CAP: AtomicU32 = AtomicU32::new(0);

/// The menu's size against its 1080-line layout.
pub static MENU_SCALE: F32 = F32::new(1.0);

/// A screenshot of the next frame, to be saved at this path.
static SCREENSHOT: Mutex<Option<PathBuf>> = Mutex::new(None);
static SCREENSHOT_ASKED: AtomicBool = AtomicBool::new(false);

pub fn ask_screenshot(path: PathBuf) {
    *SCREENSHOT.lock().unwrap_or_else(|e| e.into_inner()) = Some(path);
    SCREENSHOT_ASKED.store(true, Ordering::Release);
}

/// Whether one is asked for: a load (the present asks every frame).
pub fn screenshot_asked() -> bool {
    SCREENSHOT_ASKED.load(Ordering::Acquire)
}

/// The path of the screenshot asked for, taken.
pub fn take_screenshot() -> Option<PathBuf> {
    if !SCREENSHOT_ASKED.swap(false, Ordering::AcqRel) {
        return None;
    }
    SCREENSHOT.lock().unwrap_or_else(|e| e.into_inner()).take()
}

/// The folder screenshots go to: `LEONIDA_SCREENSHOTS`, else `screenshots`
/// beside the runtime.
pub fn screenshot_folder() -> PathBuf {
    if let Some(dir) = std::env::var_os("LEONIDA_SCREENSHOTS") {
        return PathBuf::from(dir);
    }
    std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|p| p.join("screenshots")))
        .unwrap_or_else(|| PathBuf::from("screenshots"))
}

/// The title's clock: what its `QueryPerformanceCounter` reads, at a rate
/// against the host's. Continuous: a change of rate starts from the value
/// read at that moment, so the title's time never jumps or goes back.
pub mod clock {
    use super::*;

    /// A sequence lock over (host count, title count, rate) at the last
    /// change: odd while being written.
    static SEQ: AtomicU64 = AtomicU64::new(0);
    static HOST_AT: AtomicU64 = AtomicU64::new(0);
    static TITLE_AT: AtomicU64 = AtomicU64::new(0);
    static RATE: AtomicU64 = AtomicU64::new(0x3FF0_0000_0000_0000); // 1.0
    /// Whether the rate was ever changed: until then the host's count is
    /// passed through untouched.
    static CHANGED: AtomicBool = AtomicBool::new(false);
    static WRITER: Mutex<()> = Mutex::new(());

    fn read() -> (i64, i64, f64) {
        loop {
            let s = SEQ.load(Ordering::Acquire);
            if s & 1 == 1 {
                std::hint::spin_loop();
                continue;
            }
            let v = (
                HOST_AT.load(Ordering::Acquire) as i64,
                TITLE_AT.load(Ordering::Acquire) as i64,
                f64::from_bits(RATE.load(Ordering::Acquire)),
            );
            if SEQ.load(Ordering::Acquire) == s {
                return v;
            }
        }
    }

    /// The title's count for host count `host`.
    pub fn title(host: i64) -> i64 {
        if !CHANGED.load(Ordering::Relaxed) {
            return host;
        }
        let (host_at, title_at, rate) = read();
        title_at + ((host - host_at) as f64 * rate) as i64
    }

    /// The rate now.
    pub fn rate() -> f64 {
        read().2
    }

    /// Sets the rate (clamped to 0.05 to 4), at host count `host`.
    pub fn set_rate(rate: f64, host: i64) {
        let _one = WRITER.lock().unwrap_or_else(|e| e.into_inner());
        let rate = if rate.is_finite() {
            rate.clamp(0.05, 4.0)
        } else {
            1.0
        };
        let now = title(host);
        SEQ.fetch_add(1, Ordering::AcqRel);
        HOST_AT.store(host as u64, Ordering::Release);
        TITLE_AT.store(now as u64, Ordering::Release);
        RATE.store(rate.to_bits(), Ordering::Release);
        SEQ.fetch_add(1, Ordering::AcqRel);
        CHANGED.store(true, Ordering::Release);
    }

    /// Whether the title's clock is ours (the runtime put it in).
    pub static HOOKED: AtomicBool = AtomicBool::new(false);

    /// The host's count now.
    #[cfg(windows)]
    pub fn host_now() -> i64 {
        let mut count = 0i64;
        // SAFETY: a valid out pointer.
        unsafe { windows_sys::Win32::System::Performance::QueryPerformanceCounter(&mut count) };
        count
    }

    /// The host's count now: nanoseconds since the first look.
    #[cfg(not(windows))]
    pub fn host_now() -> i64 {
        static START: OnceLock<std::time::Instant> = OnceLock::new();
        START
            .get_or_init(std::time::Instant::now)
            .elapsed()
            .as_nanos() as i64
    }

    #[cfg(test)]
    pub(crate) fn reset() {
        SEQ.store(0, Ordering::SeqCst);
        CHANGED.store(false, Ordering::SeqCst);
        RATE.store(1f64.to_bits(), Ordering::SeqCst);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_clock_passes_through_then_scales_without_jumps() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        clock::reset();
        assert_eq!(clock::title(1000), 1000);
        clock::set_rate(0.5, 1000);
        assert_eq!(clock::title(1000), 1000);
        assert_eq!(clock::title(3000), 2000);
        clock::set_rate(2.0, 3000);
        assert_eq!(clock::title(3000), 2000);
        assert_eq!(clock::title(3500), 3000);
        clock::set_rate(1.0, 4000);
        assert_eq!(clock::title(4000), 4000);
        assert_eq!(clock::title(5000), 5000);
        assert_eq!(clock::rate(), 1.0);
        clock::set_rate(100.0, 5000);
        assert_eq!(clock::rate(), 4.0);
        clock::reset();
    }

    #[test]
    fn switches_start_unavailable_and_keep_their_first_state() {
        let s = Switch::new();
        assert!(!s.there());
        s.provide(true);
        s.provide(false);
        assert!(s.there() && s.get());
        s.flip();
        assert!(!s.get());
    }

    #[test]
    fn sharpen_is_clamped() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        set_sharpen(3.0);
        assert_eq!(sharpen(), 1.0);
        set_sharpen(0.25);
        assert_eq!(sharpen(), 0.25);
    }
}
