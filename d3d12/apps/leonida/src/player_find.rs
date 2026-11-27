//! Diagnostics only (`LEONIDA_FIND_PLAYER=<start s>,<seconds>`): finds, at run
//! time, where the title keeps the local player's position, and how to reach it
//! from the title's image, so that a teleport can write there (`player.rs`).
//!
//! 1. At `<start>` seconds it lists every candidate entity transform in the
//!    title's writable memory: three unit axes at right angles (within 1e-3)
//!    and a finite position row (|x|, |y| < 20000, -500 < z < 3000), as a 4x4
//!    row-major matrix (`R4`), a 3x4 one whose position is the fourth column
//!    (`T34`) or a packed 4x3 (`R43`). Then, for `<seconds>`, it reads every
//!    candidate's position every `LEONIDA_PLAYER_INTERVAL_MS` (250) together
//!    with the keys held at that moment (the file `LEONIDA_KEYS_FILE`
//!    names). A candidate dies when it moves while no key has been held for a
//!    second (more than `LEONIDA_PLAYER_IDLE_TOL` = 0.02 m, `LEONIDA_PLAYER_IDLE_MAX`
//!    = 2 times) or stops looking like a transform; the survivors that moved
//!    are ranked by how their displacement follows the keys (speed while a
//!    movement key is held, direction along one of their own axes, the opposite
//!    sign for S).
//! 2. The best transform and the ones that move together with it (the physics
//!    copy) are searched for in memory as pointers: qwords equal to the address
//!    of the struct they sit in (a start 0 to 0x200 bytes before the matrix, 16
//!    byte steps), then qwords that point to a block holding such a pointer less
//!    than 0x1000 bytes in, up to `LEONIDA_PLAYER_LEVELS` (3) levels. The paths
//!    that start in a writable section of the title's image are written to
//!    `paths.txt`, which `player.rs` follows in later runs.
//!
//! Tuning, all optional: `LEONIDA_PLAYER_ALIGN` (4; 16 is faster),
//! `LEONIDA_PLAYER_MAX_CANDIDATES` (4000000), `LEONIDA_PLAYER_MAX_TRACKS`
//! (300000), `LEONIDA_PLAYER_TOP` (100), `LEONIDA_PLAYER_GROUPS` (6),
//! `LEONIDA_PLAYER_POINTERS=0` (no pointer search).
//!
//! Output goes under `%TEMP%\leonida-player\` (addresses of the title: never
//! commit them): `summary.txt`, `samples.txt` (time, wall clock, keys),
//! `candidates.txt` (the ranking), `tracks.csv`, `comovers.txt`, `best.txt`
//! (where the best one lies), `pointers.txt` and `paths.txt`.
//!
//! Only reads the title's memory (`ReadProcessMemory` on the own process: a page
//! that goes away fails the read). Addresses the scan keeps itself have a high
//! bit set so that its own tables are not found as pointers.

use std::fmt::Write as _;
use std::path::Path;
use std::time::{Duration, Instant};

use crate::player::{self, Group, Known, Layout, Mat, mem};

#[repr(C)]
struct MemoryBasicInformation {
    base_address: usize,
    allocation_base: usize,
    allocation_protect: u32,
    partition_id: u16,
    region_size: usize,
    state: u32,
    protect: u32,
    kind: u32,
}

unsafe extern "system" {
    fn VirtualQuery(address: usize, buffer: *mut MemoryBasicInformation, len: usize) -> usize;
    fn VirtualAlloc(address: usize, size: usize, kind: u32, protect: u32) -> *mut u8;
}

const MEM_COMMIT: u32 = 0x1000;
const MEM_RESERVE: u32 = 0x2000;
const MEM_IMAGE: u32 = 0x100_0000;
const PAGE_READWRITE: u32 = 0x04;
const PAGE_WRITECOPY: u32 = 0x08;
const PAGE_EXECUTE_READWRITE: u32 = 0x40;
const PAGE_EXECUTE_WRITECOPY: u32 = 0x80;
const PAGE_GUARD: u32 = 0x100;
const CHUNK: usize = 4 << 20;
/// A matrix (at most 64 bytes) that starts inside a chunk is read whole.
const OVERLAP: usize = 64;
/// Added to every address the scan stores itself.
const ENC: usize = 1 << 62;

const W: u8 = 1;
const A: u8 = 2;
const S: u8 = 4;
const D: u8 = 8;
const SHIFT: u8 = 16;
const SPACE: u8 = 32;
const OTHER: u8 = 64;
const WASD: u8 = W | A | S | D;

fn enc(address: usize) -> usize {
    address | ENC
}

fn dec(address: usize) -> usize {
    address & !ENC
}

struct Config {
    base: usize,
    size: usize,
    /// (name, rva, size) of every section; the writable ones are the roots.
    sections: Vec<(String, usize, usize)>,
    writable_image: Vec<(usize, usize)>,
    seconds: f64,
    align: usize,
    interval: Duration,
    max_candidates: usize,
    max_tracks: usize,
    idle_tol: f32,
    idle_max: u8,
    top: usize,
    groups: usize,
    levels: usize,
    pointers: bool,
}

/// Starts the finder thread when `LEONIDA_FIND_PLAYER` is set.
pub fn start(base: usize, info: &leonida_pe::ImageInfo) {
    let Ok(spec) = std::env::var("LEONIDA_FIND_PLAYER") else {
        return;
    };
    let numbers: Vec<f64> = spec.split(',').filter_map(|s| s.trim().parse().ok()).collect();
    let [delay, seconds] = numbers[..] else {
        eprintln!("Leonida: LEONIDA_FIND_PLAYER={spec}: expected <start s>,<seconds>, e.g. 120,90");
        return;
    };
    let num = |name: &str, default: usize| {
        std::env::var(name)
            .ok()
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(default)
    };
    let writable = |s: &&leonida_pe::SectionInfo| s.characteristics & 0x8000_0000 != 0;
    let config = Config {
        base,
        size: info.size_of_image as usize,
        sections: info
            .sections
            .iter()
            .map(|s| (s.name.clone(), s.rva as usize, s.virtual_size as usize))
            .collect(),
        writable_image: info
            .sections
            .iter()
            .filter(writable)
            .map(|s| (s.rva as usize, s.rva as usize + s.virtual_size as usize))
            .collect(),
        seconds,
        align: if num("LEONIDA_PLAYER_ALIGN", 4) >= 16 { 16 } else { 4 },
        interval: Duration::from_millis(num("LEONIDA_PLAYER_INTERVAL_MS", 250).max(20) as u64),
        max_candidates: num("LEONIDA_PLAYER_MAX_CANDIDATES", 4_000_000),
        max_tracks: num("LEONIDA_PLAYER_MAX_TRACKS", 300_000),
        idle_tol: std::env::var("LEONIDA_PLAYER_IDLE_TOL")
            .ok()
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(0.02),
        idle_max: num("LEONIDA_PLAYER_IDLE_MAX", 2) as u8,
        top: num("LEONIDA_PLAYER_TOP", 100),
        groups: num("LEONIDA_PLAYER_GROUPS", 6).clamp(1, 16),
        levels: num("LEONIDA_PLAYER_LEVELS", 3).clamp(1, 4),
        pointers: std::env::var("LEONIDA_PLAYER_POINTERS").map_or(true, |v| v != "0"),
    };
    eprintln!(
        "Leonida: find player at {delay} s for {seconds} s (keys file {:?}); output {}",
        std::env::var("LEONIDA_KEYS_FILE").ok(),
        player::out_dir().display()
    );
    std::thread::Builder::new()
        .name("leonida-find-player".into())
        .spawn(move || {
            std::thread::sleep(Duration::from_secs_f64(delay.max(0.0)));
            match run(&config) {
                Ok(headline) => eprintln!("Leonida: find player done: {headline}"),
                Err(e) => eprintln!("Leonida: find player failed: {e}"),
            }
        })
        .ok();
}

// ---------------------------------------------------------------- memory

struct Region {
    start: usize,
    size: usize,
}

/// Committed writable memory (the title's image and private or mapped memory,
/// not other libraries' images), minus the scan's own buffer.
fn regions(config: &Config, skip: (usize, usize)) -> Vec<Region> {
    let mut out = Vec::new();
    let mut at = 0x1_0000usize;
    loop {
        let mut mbi = MemoryBasicInformation {
            base_address: 0,
            allocation_base: 0,
            allocation_protect: 0,
            partition_id: 0,
            region_size: 0,
            state: 0,
            protect: 0,
            kind: 0,
        };
        // SAFETY: a plain query into a struct of the documented layout.
        let got = unsafe { VirtualQuery(at, &mut mbi, std::mem::size_of_val(&mbi)) };
        if got == 0 {
            break;
        }
        let (start, size) = (mbi.base_address, mbi.region_size);
        at = start.saturating_add(size.max(0x1000));
        let writable = mbi.protect
            & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
            != 0;
        if mbi.state != MEM_COMMIT || mbi.protect & PAGE_GUARD != 0 || !writable {
            continue;
        }
        let in_title = start >= config.base && start < config.base + config.size;
        if mbi.kind == MEM_IMAGE && !in_title {
            continue;
        }
        let end = start + size;
        if skip.0 < end && skip.1 > start {
            if skip.0 > start {
                out.push(Region { start, size: skip.0 - start });
            }
            if skip.1 < end {
                out.push(Region { start: skip.1, size: end - skip.1 });
            }
        } else {
            out.push(Region { start, size });
        }
    }
    out
}

/// Reads `want` bytes at `address` into `out`; pages that cannot be read are
/// left zero (and say so by the returned count).
fn read_robust(address: usize, out: &mut [u8]) -> usize {
    if mem::read(address, out) {
        return 0;
    }
    let mut lost = 0;
    for (i, page) in out.chunks_mut(0x1000).enumerate() {
        if !mem::read(address + i * 0x1000, page) {
            page.fill(0);
            lost += page.len();
        }
    }
    lost
}

/// Calls `f(address of the chunk, bytes, limit)` for every chunk of every region;
/// positions below `limit` belong to this chunk (the rest is the overlap into
/// the next). Returns the bytes that could not be read.
fn for_each_chunk(
    regions: &[Region],
    buffer: &mut [u8],
    overlap: usize,
    mut f: impl FnMut(&Region, usize, &[u8], usize),
) -> usize {
    let mut lost = 0;
    for region in regions {
        let mut offset = 0;
        while offset < region.size {
            let want = buffer.len().min(region.size - offset);
            lost += read_robust(region.start + offset, &mut buffer[..want]);
            let last = offset + want >= region.size;
            let limit = if last { want } else { want.saturating_sub(overlap) };
            f(region, region.start + offset, &buffer[..want], limit);
            if last || limit == 0 {
                break;
            }
            offset += limit;
        }
    }
    lost
}

fn alloc_buffer() -> Result<&'static mut [u8], String> {
    // SAFETY: a fresh private allocation that is never freed.
    let buffer = unsafe { VirtualAlloc(0, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
    if buffer.is_null() {
        return Err("no buffer".into());
    }
    // SAFETY: CHUNK bytes of read-write memory owned by nobody else.
    Ok(unsafe { std::slice::from_raw_parts_mut(buffer, CHUNK) })
}

// ------------------------------------------------------------ candidates

/// One candidate transform and what is known of it so far.
struct Slot {
    /// `enc`oded address.
    addr: usize,
    layout: Layout,
    w: f32,
    alive: bool,
    /// Moves while no key was held.
    violations: u8,
    prev: [f32; 3],
    track: Option<Box<Track>>,
}

/// What happened to a candidate that moved.
#[derive(Default)]
struct Track {
    /// Sample index of `pos[0]`.
    first: usize,
    pos: Vec<[f32; 3]>,
    /// Samples with the same movement keys held as the one before.
    steady: u32,
    /// Of those, the ones that moved at a walking to sprinting speed.
    follow: u32,
    held_dist: f32,
    speeds_w: Vec<f32>,
    speeds_shift: Vec<f32>,
    /// Sum of cosines between the movement and each axis, for W, S, D, A alone.
    cos_sum: [[f32; 3]; 4],
    cos_n: [u32; 4],
}

fn pos_ok(x: f32, y: f32, z: f32) -> bool {
    x.abs() < 20000.0 && y.abs() < 20000.0 && z > -500.0 && z < 3000.0 && x.abs() + y.abs() + z.abs() > 1.0e-3
}

#[inline]
fn unit(a: f32, b: f32, c: f32) -> bool {
    let n = a * a + b * b + c * c;
    (n - 1.0).abs() < 2.0e-3
}

#[inline]
fn dot(f: &[f32], i: usize, j: usize) -> f32 {
    f[i] * f[j] + f[i + 1] * f[j + 1] + f[i + 2] * f[j + 2]
}

fn scan_candidates(config: &Config, regions: &[Region], buffer: &mut [u8], out: &mut Vec<Slot>) -> (usize, [usize; 3]) {
    let mut unreadable = 0;
    let mut by_layout = [0usize; 3];
    let step = config.align / 4;
    let max = config.max_candidates;
    unreadable += for_each_chunk(regions, buffer, OVERLAP, |_, address, bytes, limit| {
        // SAFETY: the buffer is page aligned; `bytes` is a prefix of it.
        let f = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const f32, bytes.len() / 4) };
        let count = f.len();
        let mut j = 0;
        while j < limit / 4 {
            if out.len() >= max {
                return;
            }
            let here = address + j * 4;
            // Rows of four floats (R4, T34).
            if j + 12 <= count && unit(f[j], f[j + 1], f[j + 2]) && unit(f[j + 4], f[j + 5], f[j + 6]) && unit(f[j + 8], f[j + 9], f[j + 10]) {
                let ortho = dot(f, j, j + 4).abs() < 1.0e-3 && dot(f, j, j + 8).abs() < 1.0e-3 && dot(f, j + 4, j + 8).abs() < 1.0e-3;
                if ortho {
                    if j + 16 <= count && pos_ok(f[j + 12], f[j + 13], f[j + 14]) {
                        out.push(slot(here, Layout::R4, f[j + 15]));
                        by_layout[0] += 1;
                    }
                    if pos_ok(f[j + 3], f[j + 7], f[j + 11]) {
                        out.push(slot(here, Layout::T34, 0.0));
                        by_layout[1] += 1;
                    }
                }
            }
            // Packed rows of three floats (R43).
            if j + 12 <= count && unit(f[j], f[j + 1], f[j + 2]) && unit(f[j + 3], f[j + 4], f[j + 5]) && unit(f[j + 6], f[j + 7], f[j + 8]) {
                let ortho = dot(f, j, j + 3).abs() < 1.0e-3 && dot(f, j, j + 6).abs() < 1.0e-3 && dot(f, j + 3, j + 6).abs() < 1.0e-3;
                if ortho && pos_ok(f[j + 9], f[j + 10], f[j + 11]) {
                    out.push(slot(here, Layout::R43, 0.0));
                    by_layout[2] += 1;
                }
            }
            j += step;
        }
    });
    (unreadable, by_layout)
}

fn slot(address: usize, layout: Layout, w: f32) -> Slot {
    Slot {
        addr: enc(address),
        layout,
        w,
        alive: true,
        violations: 0,
        prev: [0.0; 3],
        track: None,
    }
}

// -------------------------------------------------------------- sampling

/// The keys the test script holds now (`LEONIDA_KEYS_FILE`, as `gameinput`
/// reads it), as bits.
fn keys_now() -> u8 {
    let Some(path) = std::env::var_os("LEONIDA_KEYS_FILE") else {
        return 0;
    };
    let text = std::fs::read_to_string(path).unwrap_or_default();
    let mut keys = 0;
    for key in text.split(|c: char| c.is_whitespace() || c == ',' || c == '+').filter(|k| !k.is_empty()) {
        keys |= match key.to_ascii_uppercase().as_str() {
            "W" => W,
            "A" => A,
            "S" => S,
            "D" => D,
            "SHIFT" => SHIFT,
            "SPACE" => SPACE,
            _ => OTHER,
        };
    }
    keys
}

fn keys_text(keys: u8) -> String {
    if keys == 0 {
        return "-".into();
    }
    let names = [(W, "W"), (A, "A"), (S, "S"), (D, "D"), (SHIFT, "Shift"), (SPACE, "Space"), (OTHER, "other")];
    names.iter().filter(|(b, _)| keys & b != 0).map(|(_, n)| *n).collect::<Vec<_>>().join("+")
}

struct Sample {
    t: f64,
    unix_ms: u128,
    keys: u8,
}

struct Block {
    start: usize,
    len: usize,
    from: usize,
    to: usize,
}

/// Reads of the alive slots, grouped so that near ones share one read.
fn build_blocks(slots: &[Slot]) -> (Vec<u32>, Vec<Block>) {
    const GAP: usize = 256;
    const SPAN: usize = 1 << 16;
    let alive: Vec<u32> = (0..slots.len() as u32).filter(|&i| slots[i as usize].alive).collect();
    let mut blocks: Vec<Block> = Vec::new();
    for (n, &i) in alive.iter().enumerate() {
        let s = &slots[i as usize];
        let (a, end) = (dec(s.addr), dec(s.addr) + s.layout.size());
        match blocks.last_mut() {
            Some(b) if a <= b.start + b.len + GAP && end.max(b.start + b.len) - b.start <= SPAN => {
                b.len = end.max(b.start + b.len) - b.start;
                b.to = n + 1;
            }
            _ => blocks.push(Block { start: a, len: end - a, from: n, to: n + 1 }),
        }
    }
    (alive, blocks)
}

/// One sample's facts that every slot needs.
struct Tick {
    index: usize,
    dt: f32,
    keys: u8,
    prev_keys: u8,
    idle: bool,
    prev_idle: bool,
}

fn observe(s: &mut Slot, m: &Mat, tick: &Tick, config: &Config, tracks: &mut usize) {
    let finite = m.pos.iter().all(|v| v.is_finite() && v.abs() < 1.0e5);
    if !finite || !m.orthonormal(0.05) {
        s.alive = false;
        s.track = None;
        return;
    }
    let d = [m.pos[0] - s.prev[0], m.pos[1] - s.prev[1], m.pos[2] - s.prev[2]];
    let dist = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
    if tick.idle && tick.prev_idle && dist > config.idle_tol {
        s.violations += 1;
        if s.violations > config.idle_max {
            s.alive = false;
            if s.track.take().is_some() {
                *tracks -= 1;
            }
            return;
        }
    }
    if s.track.is_none() && dist > 0.005 && *tracks < config.max_tracks {
        *tracks += 1;
        s.track = Some(Box::new(Track {
            first: tick.index - 1,
            pos: vec![s.prev],
            ..Track::default()
        }));
    }
    if let Some(t) = s.track.as_mut() {
        t.pos.push(m.pos);
        let stable = tick.keys & WASD != 0 && tick.keys == tick.prev_keys;
        if stable && tick.dt > 0.0 {
            let dxy = (d[0] * d[0] + d[1] * d[1]).sqrt();
            let speed = dxy / tick.dt;
            t.steady += 1;
            t.held_dist += dxy;
            if (0.4..=12.0).contains(&speed) {
                t.follow += 1;
            }
            let only = |bit: u8| tick.keys & WASD == bit;
            let class = [only(W), only(S), only(D), only(A)].iter().position(|&c| c);
            if let Some(c) = class {
                if tick.keys & SHIFT != 0 {
                    t.speeds_shift.push(speed);
                } else {
                    t.speeds_w.push(speed);
                }
                if speed > 0.3 {
                    let dir = [d[0] / dxy, d[1] / dxy];
                    for (k, axis) in m.axes.iter().enumerate() {
                        let len = (axis[0] * axis[0] + axis[1] * axis[1]).sqrt();
                        if len > 0.1 {
                            t.cos_sum[c][k] += (dir[0] * axis[0] + dir[1] * axis[1]) / len;
                        }
                    }
                    t.cos_n[c] += 1;
                }
            }
        }
    }
    s.prev = m.pos;
}

fn median(v: &[f32]) -> f32 {
    if v.is_empty() {
        return 0.0;
    }
    let mut v = v.to_vec();
    v.sort_by(f32::total_cmp);
    v[v.len() / 2]
}

/// How well a track follows the keys, and the axis the player walks along.
struct Rank {
    score: f32,
    follow: f32,
    steady: u32,
    held_dist: f32,
    med_w: f32,
    med_shift: f32,
    /// Mean cosine of the W movement with each axis, and of the S and D movement.
    cos_w: [f32; 3],
    cos_s: [f32; 3],
    cos_d: [f32; 3],
    axis: usize,
    sign: f32,
}

fn rank(t: &Track, violations: u8) -> Rank {
    let mean = |c: usize| -> [f32; 3] {
        let n = t.cos_n[c].max(1) as f32;
        t.cos_sum[c].map(|v| v / n)
    };
    let (cos_w, cos_s, cos_d) = (mean(0), mean(1), mean(2));
    let axis = (0..3).max_by(|&a, &b| cos_w[a].abs().total_cmp(&cos_w[b].abs())).unwrap_or(0);
    let sign = if cos_w[axis] < 0.0 { -1.0 } else { 1.0 };
    let follow = t.follow as f32 / t.steady.max(1) as f32;
    let med_w = median(&t.speeds_w);
    let mut score = 0.0;
    if t.steady >= 3 && t.held_dist >= 1.0 {
        score = 2.0 * follow + cos_w[axis].abs();
        if t.cos_n[1] > 0 && cos_s[axis] * sign < -0.5 {
            score += 0.5;
        }
        if (0.8..=4.0).contains(&med_w) {
            score += 0.5;
        }
        if t.cos_n[2] > 0 && (0..3).any(|j| j != axis && cos_d[j].abs() > 0.6) {
            score += 0.25;
        }
        score -= 0.25 * f32::from(violations);
    }
    Rank {
        score,
        follow,
        steady: t.steady,
        held_dist: t.held_dist,
        med_w,
        med_shift: median(&t.speeds_shift),
        cos_w,
        cos_s,
        cos_d,
        axis,
        sign,
    }
}

// ------------------------------------------------------------------ run

fn region_note(config: &Config, address: usize) -> String {
    let mut mbi = MemoryBasicInformation {
        base_address: 0,
        allocation_base: 0,
        allocation_protect: 0,
        partition_id: 0,
        region_size: 0,
        state: 0,
        protect: 0,
        kind: 0,
    };
    // SAFETY: a plain query into a struct of the documented layout.
    let got = unsafe { VirtualQuery(address, &mut mbi, std::mem::size_of_val(&mbi)) };
    if got == 0 {
        return "not mapped".into();
    }
    let mut s = format!(
        "region {:#x}..{:#x} (+{:#x} into it, {:#x} bytes) allocation base {:#x} (+{:#x}) protect {:#x} type {:#x}",
        mbi.base_address,
        mbi.base_address + mbi.region_size,
        address - mbi.base_address,
        mbi.region_size,
        mbi.allocation_base,
        address - mbi.allocation_base,
        mbi.protect,
        mbi.kind
    );
    if address >= config.base && address < config.base + config.size {
        let rva = address - config.base;
        let section = config.sections.iter().find(|(_, r, n)| rva >= *r && rva < r + n);
        let _ = write!(s, "; in the title's image at rva {rva:#x} {}", section.map_or("", |(n, _, _)| n.as_str()));
    }
    s
}

fn run(config: &Config) -> Result<String, String> {
    let dir = player::out_dir();
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let buffer = alloc_buffer()?;
    let skip = (buffer.as_ptr() as usize, buffer.as_ptr() as usize + CHUNK);
    let regions = regions(config, skip);
    let total: usize = regions.iter().map(|r| r.size).sum();
    let started = Instant::now();

    let mut slots: Vec<Slot> = Vec::new();
    let (unreadable, by_layout) = scan_candidates(config, &regions, buffer, &mut slots);
    let found_in = started.elapsed().as_secs_f32();
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "memory scanned: {} regions, {} MiB, {} KiB unreadable, {found_in:.1} s; candidates {} (R4 {}, T34 {}, R43 {}; R4 with w == 1: {}){}",
        regions.len(),
        total >> 20,
        unreadable >> 10,
        slots.len(),
        by_layout[0],
        by_layout[1],
        by_layout[2],
        slots.iter().filter(|s| s.layout == Layout::R4 && (s.w - 1.0).abs() < 1.0e-4).count(),
        if slots.len() >= config.max_candidates { " (limit reached)" } else { "" },
    );
    eprintln!("Leonida: find player: {}", summary.trim_end());

    // Sampling.
    let unix = |t: std::time::SystemTime| t.duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_millis();
    let t0 = Instant::now();
    let wall0 = unix(std::time::SystemTime::now());
    let mut samples: Vec<Sample> = Vec::new();
    let mut tracks = 0usize;
    let (mut alive, mut blocks) = build_blocks(&slots);
    let mut alive_at_build = alive.len();
    let mut last_key_t = f64::NEG_INFINITY;
    let mut prev_idle = false;
    let mut read_buf = vec![0u8; 1 << 17];
    let mut next = t0;
    loop {
        let now = Instant::now();
        let t = now.duration_since(t0).as_secs_f64();
        let keys = keys_now();
        if keys != 0 {
            last_key_t = t;
        }
        let idle = keys == 0 && t - last_key_t >= 1.0;
        let index = samples.len();
        let dt = index.checked_sub(1).map_or(0.0, |p| (t - samples[p].t) as f32);
        let tick = Tick {
            index,
            dt,
            keys,
            prev_keys: samples.last().map_or(0, |s| s.keys),
            idle,
            prev_idle,
        };
        for b in &blocks {
            let ok = b.len <= read_buf.len() && mem::read(b.start, &mut read_buf[..b.len]);
            for &i in &alive[b.from..b.to] {
                let s = &mut slots[i as usize];
                let (a, size) = (dec(s.addr), s.layout.size());
                let bytes = if ok {
                    Some(read_buf[a - b.start..a - b.start + size].to_vec())
                } else {
                    mem::read_vec(a, size)
                };
                let Some(bytes) = bytes else {
                    s.alive = false;
                    if s.track.take().is_some() {
                        tracks -= 1;
                    }
                    continue;
                };
                let m = player::decode(&bytes, s.layout);
                if index == 0 {
                    s.prev = m.pos;
                } else {
                    observe(s, &m, &tick, config, &mut tracks);
                }
            }
        }
        samples.push(Sample { t, unix_ms: wall0 + (t * 1000.0) as u128, keys });
        prev_idle = idle;
        let alive_now = slots.iter().filter(|s| s.alive).count();
        if alive_now * 10 < alive_at_build * 9 {
            (alive, blocks) = build_blocks(&slots);
            alive_at_build = alive.len();
        }
        if index % 20 == 0 {
            eprintln!(
                "Leonida: find player: sample {index} t={t:.1}s keys={} alive {alive_now} tracked {tracks}",
                keys_text(keys)
            );
        }
        if t >= config.seconds {
            break;
        }
        next += config.interval;
        if let Some(wait) = next.checked_duration_since(Instant::now()) {
            std::thread::sleep(wait);
        } else {
            next = Instant::now();
        }
    }
    let alive_end = slots.iter().filter(|s| s.alive).count();
    let _ = writeln!(
        summary,
        "sampling: {} samples over {:.1} s (wall clock of t=0: unix ms {wall0}), alive at the end {alive_end}, tracked {tracks}",
        samples.len(),
        samples.last().map_or(0.0, |s| s.t)
    );

    // Ranking.
    let mut ranked: Vec<(usize, Rank)> = slots
        .iter()
        .enumerate()
        .filter(|(_, s)| s.alive)
        .filter_map(|(i, s)| Some((i, rank(s.track.as_ref()?, s.violations))))
        .filter(|(_, r)| r.score > 0.0)
        .collect();
    ranked.sort_by(|a, b| b.1.score.total_cmp(&a.1.score));
    let _ = writeln!(summary, "ranked (moved with the keys): {}", ranked.len());

    let mut text = String::new();
    for (n, (i, r)) in ranked.iter().take(config.top).enumerate() {
        let s = &slots[*i];
        let _ = writeln!(
            text,
            "#{n} addr={:#x} {} w={:.2} score={:.2} follow={:.2} steady={} dist={:.1} m median speed W={:.2} W+Shift={:.2} m/s violations={} axis={} sign={:+} cosW={:.2?} cosS={:.2?} cosD={:.2?}",
            dec(s.addr),
            s.layout.name(),
            s.w,
            r.score,
            r.follow,
            r.steady,
            r.held_dist,
            r.med_w,
            r.med_shift,
            s.violations,
            r.axis,
            r.sign,
            r.cos_w,
            r.cos_s,
            r.cos_d
        );
    }
    write(&dir, "candidates.txt", &text)?;
    let mut text = String::from("sample,t,unix_ms,keys\n");
    for (n, s) in samples.iter().enumerate() {
        let _ = writeln!(text, "{n},{:.3},{},{}", s.t, s.unix_ms, keys_text(s.keys));
    }
    write(&dir, "samples.txt", &text)?;

    let Some(&(best_i, _)) = ranked.first() else {
        write(&dir, "summary.txt", &summary)?;
        return Err(format!("nothing moved with the keys; {}", summary.trim_end().replace('\n', "; ")));
    };
    let best = &slots[best_i];
    let best_track = best.track.as_ref().expect("ranked slots have tracks");
    let (best_addr, best_layout) = (dec(best.addr), best.layout);
    let best_rank = rank(best_track, best.violations);
    write(
        &dir,
        "best.txt",
        &format!(
            "best candidate {best_addr:#x} {} score {:.2}\n{}\nforward axis {} sign {:+} (cosines with W: {:.2?})\nstruct start candidates: see pointers.txt\n",
            best_layout.name(),
            best_rank.score,
            region_note(config, best_addr),
            best_rank.axis,
            best_rank.sign,
            best_rank.cos_w
        ),
    )?;

    // Transforms that move together with the best one (a constant offset).
    let mut movers: Vec<(usize, [f32; 3], f32)> = Vec::new();
    for (i, r) in &ranked {
        let (s, t) = (&slots[*i], slots[*i].track.as_ref().unwrap());
        let from = t.first.max(best_track.first);
        let end = (t.first + t.pos.len()).min(best_track.first + best_track.pos.len());
        let n = end.saturating_sub(from);
        if n < 5 || r.held_dist < 0.7 * best_rank.held_dist {
            continue;
        }
        let off = |k: usize| {
            let (a, b) = (t.pos[from - t.first + k], best_track.pos[from - best_track.first + k]);
            [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
        };
        let mut mean = [0.0f32; 3];
        for k in 0..n {
            for (m, v) in mean.iter_mut().zip(off(k)) {
                *m += v / n as f32;
            }
        }
        let dev = (0..n)
            .map(|k| {
                let o = off(k);
                ((o[0] - mean[0]).powi(2) + (o[1] - mean[1]).powi(2) + (o[2] - mean[2]).powi(2)).sqrt()
            })
            .fold(0.0, f32::max);
        if dev <= 0.10 {
            movers.push((*i, mean, dev));
        }
        let _ = s;
    }
    let mut text = String::new();
    for &(i, mean, dev) in &movers {
        let _ = writeln!(
            text,
            "addr={:#x} {} offset from the best ({:+.3}, {:+.3}, {:+.3}) max deviation {dev:.3} m; {:+#x} bytes from the best",
            dec(slots[i].addr),
            slots[i].layout.name(),
            mean[0],
            mean[1],
            mean[2],
            dec(slots[i].addr) as i64 - best_addr as i64
        );
    }
    write(&dir, "comovers.txt", &text)?;
    let _ = writeln!(summary, "move together with the best: {}", movers.len());

    // Tracks of the best ranked and of the co-movers.
    let mut csv = String::from("rank,addr,layout,sample,t,unix_ms,x,y,z,keys\n");
    let mut shown: Vec<usize> = ranked.iter().take(30).map(|&(i, _)| i).collect();
    shown.extend(movers.iter().map(|m| m.0).filter(|i| !shown.contains(i)).take(20).collect::<Vec<_>>());
    for i in shown {
        let (s, t) = (&slots[i], slots[i].track.as_ref().unwrap());
        let n = ranked.iter().position(|(j, _)| *j == i).unwrap_or(0);
        for (k, p) in t.pos.iter().enumerate() {
            let sample = &samples[t.first + k];
            let _ = writeln!(
                csv,
                "{n},{:#x},{},{},{:.3},{},{:.3},{:.3},{:.3},{}",
                dec(s.addr),
                s.layout.name(),
                t.first + k,
                sample.t,
                sample.unix_ms,
                p[0],
                p[1],
                p[2],
                keys_text(sample.keys)
            );
        }
    }
    write(&dir, "tracks.csv", &csv)?;

    // The groups: the best, then the co-movers that are not at nearly the same
    // address as one already taken.
    let mut picked: Vec<(usize, Layout)> = vec![(enc(best_addr), best_layout)];
    for &(i, _, _) in &movers {
        let (a, l) = (dec(slots[i].addr), slots[i].layout);
        if picked.len() < config.groups && picked.iter().all(|&(p, _)| dec(p).abs_diff(a) >= 0x40) {
            picked.push((enc(a), l));
        }
    }
    let forward = Some((best_rank.axis, best_rank.sign));
    let _ = writeln!(summary, "groups for the teleport: {} (addresses in comovers.txt)", picked.len());
    let best_line = format!(
        "best {best_addr:#x} {} ({}); {} co-movers",
        best_layout.name(),
        region_note(config, best_addr),
        movers.len()
    );

    // Free what holds addresses, then look for pointers.
    drop(slots);
    drop(ranked);
    let mut known = Known {
        groups: picked
            .iter()
            .enumerate()
            .map(|(n, &(a, l))| Group {
                layout: l,
                primary: n == 0,
                paths: Vec::new(),
                fixed: Some(enc(a)),
            })
            .collect(),
        forward,
    };
    if config.pointers {
        let report = pointer_scan(config, &regions, buffer, &mut known, &dir)?;
        let _ = writeln!(summary, "{report}");
    }
    for g in &mut known.groups {
        // The address serves this run; the paths the next ones.
        g.fixed = g.fixed.map(dec);
    }
    let with_paths = known.groups.iter().filter(|g| !g.paths.is_empty()).count();
    if with_paths > 0 {
        let mut persisted = known.clone();
        for g in &mut persisted.groups {
            g.fixed = None;
        }
        // A group without paths cannot be found again.
        persisted.groups.retain(|g| !g.paths.is_empty());
        if !persisted.groups.iter().any(|g| g.primary) {
            persisted.groups[0].primary = true;
        }
        write(&dir, "paths.txt", &persisted.to_text(config.size))?;
    }
    player::set_known(known);
    write(&dir, "summary.txt", &summary)?;
    Ok(format!("{best_line}; {with_paths} group(s) with image paths; details in {}", dir.display()))
}

fn write(dir: &Path, name: &str, text: &str) -> Result<(), String> {
    std::fs::write(dir.join(name), text).map_err(|e| format!("{name}: {e}"))
}

// ------------------------------------------------------------- pointers

/// A qword in memory that points, directly or through blocks, to a struct.
struct Node {
    /// `enc`oded location of the qword.
    loc: usize,
    /// The node one level closer to the struct.
    parent: u32,
    /// Added to the qword's value to get the parent's location.
    delta: u32,
    group: u16,
    /// The struct start is the matrix address minus 16 * k.
    k: u8,
}

const NO_PARENT: u32 = u32::MAX;
const NODE_CAP: usize = 300_000;
const LOWER_BITS: u32 = 22;

fn bloom_slot(key: u64) -> (usize, u64) {
    let h = key.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> (64 - LOWER_BITS);
    ((h >> 6) as usize, 1u64 << (h & 63))
}

fn pointer_scan(
    config: &Config,
    regions: &[Region],
    buffer: &mut [u8],
    known: &mut Known,
    dir: &Path,
) -> Result<String, String> {
    let started = Instant::now();
    let mut report = String::new();
    // Struct start candidates of each group.
    let mut targets: Vec<(usize, u16, u8)> = Vec::new();
    for (g, group) in known.groups.iter().enumerate() {
        let matrix = group.fixed.map_or(0, dec);
        for k in 0..=32u8 {
            if let Some(start) = matrix.checked_sub(16 * usize::from(k)) {
                targets.push((enc(start), g as u16, k));
            }
        }
    }
    targets.sort_unstable();
    let (min_t, max_t) = (dec(targets[0].0), dec(targets[targets.len() - 1].0));
    let mut bloom = vec![0u64; 1 << (LOWER_BITS - 6)];
    for t in &targets {
        let (w, b) = bloom_slot(dec(t.0) as u64 >> 4);
        bloom[w] |= b;
    }

    let mut nodes: Vec<Node> = Vec::new();
    // Level 1: a qword equal to a struct candidate's address.
    let mut capped = false;
    for_each_chunk(regions, buffer, 0, |_, address, bytes, limit| {
        // SAFETY: the buffer is page aligned; `bytes` is a prefix of it.
        let q = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const u64, bytes.len() / 8) };
        for (j, &v) in q[..limit / 8].iter().enumerate() {
            let v = v as usize;
            if v < min_t || v > max_t {
                continue;
            }
            let (w, b) = bloom_slot(v as u64 >> 4);
            if bloom[w] & b == 0 {
                continue;
            }
            let e = enc(v);
            let mut at = targets.partition_point(|t| t.0 < e);
            while at < targets.len() && targets[at].0 == e {
                if nodes.len() >= NODE_CAP {
                    capped = true;
                    return;
                }
                nodes.push(Node {
                    loc: enc(address + j * 8),
                    parent: NO_PARENT,
                    delta: 0,
                    group: targets[at].1,
                    k: targets[at].2,
                });
                at += 1;
            }
        }
    });
    let in_image = |loc: usize| {
        let rva = dec(loc).wrapping_sub(config.base);
        config.writable_image.iter().any(|&(lo, hi)| rva >= lo && rva < hi)
    };
    let _ = writeln!(report, "pointer level 1: {} qwords point at a struct candidate{}", nodes.len(), if capped { " (capped)" } else { "" });
    let mut frontier: Vec<u32> = (0..nodes.len() as u32).filter(|&i| !in_image(nodes[i as usize].loc)).collect();
    let mut level_ranges = vec![(0usize, nodes.len())];

    for level in 2..=config.levels as u8 {
        if frontier.is_empty() {
            break;
        }
        let mut locs: Vec<(usize, u32)> = frontier.iter().map(|&i| (nodes[i as usize].loc, i)).collect();
        locs.sort_unstable();
        let (min_l, max_l) = (dec(locs[0].0), dec(locs[locs.len() - 1].0));
        let mut pages = vec![0u64; 1 << (LOWER_BITS - 6)];
        for &(l, _) in &locs {
            for page in [dec(l) >> 12, (dec(l) >> 12).wrapping_sub(1)] {
                let (w, b) = bloom_slot(page as u64);
                pages[w] |= b;
            }
        }
        let before = nodes.len();
        let mut capped = false;
        for_each_chunk(regions, buffer, 0, |_, address, bytes, limit| {
            // SAFETY: the buffer is page aligned; `bytes` is a prefix of it.
            let q = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const u64, bytes.len() / 8) };
            for (j, &v) in q[..limit / 8].iter().enumerate() {
                let v = v as usize;
                if v.saturating_add(0xFFF) < min_l || v > max_l {
                    continue;
                }
                let (w, b) = bloom_slot(v as u64 >> 12);
                if pages[w] & b == 0 {
                    continue;
                }
                let e = enc(v);
                let mut at = locs.partition_point(|l| l.0 < e);
                let mut taken = 0;
                while at < locs.len() && locs[at].0 <= e + 0xFFF && taken < 4 {
                    if nodes.len() - before >= NODE_CAP {
                        capped = true;
                        return;
                    }
                    let (pg, pk) = (nodes[locs[at].1 as usize].group, nodes[locs[at].1 as usize].k);
                    nodes.push(Node {
                        loc: enc(address + j * 8),
                        parent: locs[at].1,
                        delta: (dec(locs[at].0) - v) as u32,
                        group: pg,
                        k: pk,
                    });
                    at += 1;
                    taken += 1;
                }
            }
        });
        let from = before;
        level_ranges.push((from, nodes.len()));
        let roots = (from..nodes.len()).filter(|&i| in_image(nodes[i].loc)).count();
        let _ = writeln!(
            report,
            "pointer level {level}: {} qwords point into a block holding a level {} pointer within 0x1000 bytes{}; {roots} of them in the title's image",
            nodes.len() - from,
            level - 1,
            if capped { " (capped)" } else { "" }
        );
        frontier = (from..nodes.len()).filter(|&i| !in_image(nodes[i].loc)).map(|i| i as u32).collect();
    }

    // Image-rooted paths, shortest first, per group.
    let mut text = String::new();
    let mut counts = vec![0usize; known.groups.len()];
    for (range_level, &(lo, hi)) in level_ranges.iter().enumerate() {
        for i in lo..hi {
            let n = &nodes[i];
            if !in_image(n.loc) {
                continue;
            }
            let g = usize::from(n.group);
            if counts[g] >= 40 {
                continue;
            }
            counts[g] += 1;
            let mut deltas = Vec::new();
            let mut at = n;
            while at.parent != NO_PARENT {
                deltas.push(at.delta as usize);
                at = &nodes[at.parent as usize];
            }
            let rva = dec(n.loc) - config.base;
            let section = config
                .sections
                .iter()
                .find(|(_, r, s)| rva >= *r && rva < r + s)
                .map_or("", |(name, _, _)| name.as_str());
            let _ = writeln!(
                text,
                "group {g} level {} rva={rva:#x} ({section}) deltas={:x?} struct start = matrix - {:#x}",
                range_level + 1,
                deltas,
                16 * usize::from(n.k)
            );
            known.groups[g].paths.push(player::Path {
                rva,
                deltas,
                adjust: 16 * usize::from(n.k),
            });
        }
    }
    // What the struct start's first qword looks like (a vtable pointer is in the image).
    for (g, group) in known.groups.iter().enumerate() {
        let matrix = group.fixed.map_or(0, dec);
        let _ = writeln!(text, "group {g}: matrix {matrix:#x} {}", group.layout.name());
        for k in 0..=32usize {
            let Some(start) = matrix.checked_sub(16 * k) else { break };
            if let Some(first) = mem::read_u64(start) {
                let first = first as usize;
                let rva = first.wrapping_sub(config.base);
                if rva < config.size {
                    let section = config.sections.iter().find(|(_, r, s)| rva >= *r && rva < r + s).map_or("", |(n, _, _)| n.as_str());
                    let _ = writeln!(text, "  start candidate -{:#x}: first qword is title+{rva:#x} {section}", 16 * k);
                }
            }
        }
    }
    write(dir, "pointers.txt", &text)?;
    let _ = writeln!(
        report,
        "image-rooted paths per group: {counts:?} ({:.1} s)",
        started.elapsed().as_secs_f32()
    );
    Ok(report)
}
