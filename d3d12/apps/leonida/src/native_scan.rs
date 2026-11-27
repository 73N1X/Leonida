//! Diagnostics only (`LEONIDA_NATIVE_SCAN`): looks, at run time, for the tables
//! the title's script engine calls its commands through.
//!
//! A RAGE-style engine resolves each script's native table from hashes to
//! handler addresses once the script is loaded, so the handlers (all inside
//! the title's executable section) sit in arrays in memory the title builds
//! itself, even though no such table exists in the file (`docs/mod-menu.md`).
//! This only *observes* data the title has written to its own memory: it reads
//! committed memory (through `ReadProcessMemory` on the own process, so a page
//! that goes away under the scan fails the read instead of the process) and
//! writes nothing to the title.
//!
//! `LEONIDA_NATIVE_SCAN=<s>[,<s>...]` scans that many seconds after the title
//! starts (once per number). Tuning, all optional:
//!
//! - `LEONIDA_NATIVE_MIN_RUN` (default 64): fewest array elements in a run.
//! - `LEONIDA_NATIVE_MIN_DISTINCT` (default 200): fewest distinct handlers for
//!   a run to be listed and to enter the union.
//! - `LEONIDA_NATIVE_ALL=1`: also scan read-only and image-backed memory (the
//!   default is private/mapped memory a program can write to).
//!
//! Output goes under `%TEMP%\leonida-natives\scan-<s>s\` (addresses of the
//! title: never commit them): `summary.txt`, `candidates.txt`, `targets.txt`
//! (handler RVAs per candidate) and `union.txt` (one handler RVA per line).

use std::collections::HashMap;
use std::fmt::Write as _;
use std::path::PathBuf;
use std::time::Duration;

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
    fn GetCurrentProcess() -> isize;
    fn ReadProcessMemory(
        process: isize,
        address: usize,
        buffer: *mut u8,
        size: usize,
        read: *mut usize,
    ) -> i32;
}

const MEM_COMMIT: u32 = 0x1000;
const MEM_RESERVE: u32 = 0x2000;
const MEM_IMAGE: u32 = 0x100_0000;
const PAGE_READWRITE: u32 = 0x04;
const PAGE_READONLY: u32 = 0x02;
const PAGE_WRITECOPY: u32 = 0x08;
const PAGE_EXECUTE_READ: u32 = 0x20;
const PAGE_EXECUTE_READWRITE: u32 = 0x40;
const PAGE_EXECUTE_WRITECOPY: u32 = 0x80;
const PAGE_GUARD: u32 = 0x100;
const CHUNK: usize = 4 << 20;
const MAX_STRIDE: usize = 8;
/// Candidates listed in full (the best by distinct handlers).
const LIST_MAX: usize = 200;

/// Starts the scan thread when `LEONIDA_NATIVE_SCAN` is set.
pub fn start(base: usize, info: &leonida_pe::ImageInfo) {
    let Ok(list) = std::env::var("LEONIDA_NATIVE_SCAN") else {
        return;
    };
    let delays: Vec<u64> = list
        .split(',')
        .filter_map(|s| s.trim().parse().ok())
        .collect();
    if delays.is_empty() {
        eprintln!("Leonida: LEONIDA_NATIVE_SCAN={list}: expected seconds, e.g. 240 or 120,300");
        return;
    }
    let env_num = |name: &str, default: usize| {
        std::env::var(name)
            .ok()
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(default)
    };
    let config = Config {
        base,
        size: info.size_of_image as usize,
        exec: info
            .sections
            .iter()
            .filter(|s| s.characteristics & 0x2000_0000 != 0)
            .map(|s| (s.rva as usize, s.rva as usize + s.virtual_size as usize))
            .collect(),
        writable_image: info
            .sections
            .iter()
            .filter(|s| s.characteristics & 0x8000_0000 != 0)
            .map(|s| (s.rva as usize, s.rva as usize + s.virtual_size as usize))
            .collect(),
        min_run: env_num("LEONIDA_NATIVE_MIN_RUN", 64).max(8),
        min_distinct: env_num("LEONIDA_NATIVE_MIN_DISTINCT", 200),
        all: std::env::var("LEONIDA_NATIVE_ALL").is_ok_and(|v| v == "1"),
    };
    eprintln!(
        "Leonida: native scan at {delays:?} s; executable ranges (rva) {:x?}; output {}",
        config.exec,
        out_root().display()
    );
    std::thread::Builder::new()
        .name("leonida-native-scan".into())
        .spawn(move || {
            let mut waited = 0;
            for delay in delays {
                std::thread::sleep(Duration::from_secs(delay.saturating_sub(waited)));
                waited = delay;
                let started = std::time::Instant::now();
                let dir = out_root().join(format!("scan-{delay}s"));
                match scan(&config, &dir) {
                    Ok(headline) => eprintln!(
                        "Leonida: native scan {delay}s done in {:.1}s: {headline}; details in {}",
                        started.elapsed().as_secs_f32(),
                        dir.display()
                    ),
                    Err(e) => eprintln!("Leonida: native scan {delay}s failed: {e}"),
                }
            }
        })
        .ok();
}

fn out_root() -> PathBuf {
    std::env::temp_dir().join("leonida-natives")
}

struct Config {
    base: usize,
    size: usize,
    /// Executable sections, as RVA ranges.
    exec: Vec<(usize, usize)>,
    /// Writable sections of the title's image, as RVA ranges.
    writable_image: Vec<(usize, usize)>,
    min_run: usize,
    min_distinct: usize,
    all: bool,
}

impl Config {
    /// The RVA of `value` when it points into an executable section.
    #[inline]
    fn target(&self, value: u64) -> Option<u32> {
        let rva = (value as usize).wrapping_sub(self.base);
        if rva >= self.size {
            return None;
        }
        self.exec
            .iter()
            .any(|&(lo, hi)| rva >= lo && rva < hi)
            .then_some(rva as u32)
    }

    #[inline]
    fn target32(&self, value: u32) -> Option<u32> {
        let rva = value as usize;
        self.exec
            .iter()
            .any(|&(lo, hi)| rva >= lo && rva < hi)
            .then_some(value)
    }
}

/// One run of array elements that all point into code.
struct Candidate {
    /// First element's address.
    address: usize,
    /// Elements in the run.
    len: usize,
    /// Bytes between elements (8 * stride for qwords, 4 for the 32-bit kind).
    step: usize,
    /// 32-bit RVAs rather than 64-bit pointers.
    narrow: bool,
    /// Sorted distinct handler RVAs.
    targets: Vec<u32>,
    /// What the memory is (see [`Region::tag`]).
    tag: &'static str,
}

#[derive(Clone, Copy, Default)]
struct Run {
    start: usize,
    len: usize,
}

struct Region {
    start: usize,
    size: usize,
    tag: &'static str,
}

/// Committed memory worth scanning, minus the scan's own buffer.
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
        let readable = writable || mbi.protect & (PAGE_READONLY | PAGE_EXECUTE_READ) != 0;
        if mbi.state != MEM_COMMIT || mbi.protect & PAGE_GUARD != 0 || !readable {
            continue;
        }
        let in_title = start >= config.base && start < config.base + config.size;
        let image_backed = mbi.kind == MEM_IMAGE && !in_title;
        if !config.all && (!writable || image_backed) {
            continue;
        }
        let tag = match (in_title, image_backed, writable) {
            (true, _, true) => "title-image-rw",
            (true, _, false) => "title-image-ro",
            (_, true, _) => "dll-image",
            (_, _, true) => "heap-rw",
            _ => "heap-ro",
        };
        // Split around the scan's own buffer so it never finds itself.
        let end = start + size;
        if skip.0 < end && skip.1 > start {
            if skip.0 > start {
                out.push(Region {
                    start,
                    size: skip.0 - start,
                    tag,
                });
            }
            if skip.1 < end {
                out.push(Region {
                    start: skip.1,
                    size: end - skip.1,
                    tag,
                });
            }
        } else {
            out.push(Region { start, size, tag });
        }
    }
    out
}

fn scan(config: &Config, dir: &std::path::Path) -> Result<String, String> {
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    // The copy buffer lives outside the allocator, so it can be told apart.
    // SAFETY: a fresh private allocation owned by this function.
    let buffer = unsafe { VirtualAlloc(0, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
    if buffer.is_null() {
        return Err("no buffer".into());
    }
    let skip = (buffer as usize, buffer as usize + CHUNK);
    let regions = regions(config, skip);
    // SAFETY: the buffer is CHUNK bytes of read-write memory.
    let chunk = unsafe { std::slice::from_raw_parts_mut(buffer, CHUNK) };

    let mut found: Vec<Candidate> = Vec::new();
    // Runs seen by how many distinct handlers they hold, for context.
    let mut by_distinct = [0usize; 7];
    let mut unreadable = 0usize;
    let mut scanned = 0usize;
    for region in &regions {
        let mut scanner = Scanner::new(config, region.tag);
        let mut offset = 0;
        while offset < region.size {
            let want = CHUNK.min(region.size - offset);
            let mut read = 0usize;
            // SAFETY: reads the own process into the buffer; a failure is a
            // return value, not a fault.
            let ok = unsafe {
                ReadProcessMemory(
                    GetCurrentProcess(),
                    region.start + offset,
                    chunk.as_mut_ptr(),
                    want,
                    &mut read,
                )
            };
            if ok == 0 || read == 0 {
                unreadable += want;
                scanner.finish_all(&mut found, &mut by_distinct);
                offset += want;
                continue;
            }
            scanned += read;
            scanner.feed(region.start + offset, &chunk[..read], &mut found, &mut by_distinct);
            offset += want;
        }
        scanner.finish_all(&mut found, &mut by_distinct);
    }

    // A stride-1 run holds every longer-stride run inside it.
    let spans: Vec<(usize, usize, usize, bool)> = found
        .iter()
        .map(|c| (c.address, c.address + c.len * c.step, c.step, c.narrow))
        .collect();
    let mut listed: Vec<usize> = (0..found.len())
        .filter(|&i| found[i].targets.len() >= config.min_distinct)
        .filter(|&i| {
            let (lo, hi, step, narrow) = spans[i];
            !spans.iter().enumerate().any(|(j, &(l, h, s, n))| {
                j != i && n == narrow && s < step && l <= lo && h >= hi
            })
        })
        .collect();
    listed.sort_by_key(|&i| std::cmp::Reverse(found[i].targets.len()));
    listed.truncate(LIST_MAX);

    // The union of handlers over the listed candidates.
    let mut union: Vec<u32> = listed
        .iter()
        .flat_map(|&i| found[i].targets.iter().copied())
        .collect();
    union.sort_unstable();
    union.dedup();

    let mut candidates = String::new();
    let mut targets = String::new();
    for (n, &i) in listed.iter().enumerate() {
        let c = &found[i];
        let covered = intersect(&c.targets, &union);
        let _ = write!(
            candidates,
            "#{n} addr={:#x} (rva {:#x}) {} len={} step={} {} distinct={} cover_of_union={:.1}%",
            c.address,
            c.address.wrapping_sub(config.base),
            c.tag,
            c.len,
            c.step,
            if c.narrow { "u32-rva" } else { "qword" },
            c.targets.len(),
            100.0 * covered as f64 / union.len().max(1) as f64
        );
        if !c.narrow {
            let _ = write!(candidates, " | {}", slot_stats(c));
        }
        candidates.push('\n');
        let _ = write!(targets, "#{n}:");
        for t in &c.targets {
            let _ = write!(targets, " {t:x}");
        }
        targets.push('\n');
    }
    // Overlaps between listed candidates (handler sets sharing at least half
    // of the smaller one).
    candidates.push_str("\noverlaps (shared handlers; pairs sharing at least half of the smaller set):\n");
    for (a, &i) in listed.iter().enumerate() {
        for (b, &j) in listed.iter().enumerate().skip(a + 1) {
            let shared = intersect(&found[i].targets, &found[j].targets);
            let smaller = found[i].targets.len().min(found[j].targets.len());
            if shared * 2 >= smaller && shared > 0 {
                let _ = writeln!(candidates, "#{a} & #{b}: {shared} shared of {smaller}");
            }
        }
    }

    // Where the union's handlers sit: 1 MiB buckets of RVA.
    let mut histogram: std::collections::BTreeMap<u32, usize> = Default::default();
    for &t in &union {
        *histogram.entry(t >> 20).or_default() += 1;
    }
    let mut summary = String::new();
    let _ = writeln!(summary, "title base {:#x}, image size {:#x}", config.base, config.size);
    let _ = writeln!(summary, "executable rva ranges: {:x?}", config.exec);
    let _ = writeln!(summary, "title writable section rva ranges: {:x?}", config.writable_image);
    let _ = writeln!(
        summary,
        "min run {}, min distinct {}, all memory {}",
        config.min_run, config.min_distinct, config.all
    );
    let _ = writeln!(
        summary,
        "regions {}, {} MiB scanned, {} MiB unreadable",
        regions.len(),
        scanned >> 20,
        unreadable >> 20
    );
    let mut by_tag: HashMap<&str, (usize, usize)> = HashMap::new();
    for r in &regions {
        let e = by_tag.entry(r.tag).or_default();
        e.0 += 1;
        e.1 += r.size;
    }
    for (tag, (n, bytes)) in &by_tag {
        let _ = writeln!(summary, "  {tag}: {n} regions, {} MiB", bytes >> 20);
    }
    let _ = writeln!(summary, "runs of >= {} handler pointers: {}", config.min_run, found.len());
    let labels = ["<16", "<64", "<100", "<200", "<500", "<1000", ">=1000"];
    for (label, n) in labels.iter().zip(by_distinct) {
        let _ = writeln!(summary, "  runs with {label} distinct handlers: {n}");
    }
    let _ = writeln!(
        summary,
        "listed candidates (>= {} distinct, not inside a denser run): {}",
        config.min_distinct,
        listed.len()
    );
    let _ = writeln!(summary, "union of their handlers: {} distinct", union.len());
    let _ = writeln!(summary, "union by 1 MiB of rva (bucket start: handlers):");
    for (bucket, n) in &histogram {
        let _ = writeln!(summary, "  {:#x}: {n}", bucket << 20);
    }
    let headline = format!(
        "{} MiB scanned, {} runs, {} candidates with >= {} handlers, union {} handlers",
        scanned >> 20,
        found.len(),
        listed.len(),
        config.min_distinct,
        union.len()
    );
    let _ = writeln!(summary, "\n{headline}");

    let mut union_text = String::new();
    for t in &union {
        let _ = writeln!(union_text, "{t:x}");
    }
    for (name, text) in [
        ("summary.txt", &summary),
        ("candidates.txt", &candidates),
        ("targets.txt", &targets),
        ("union.txt", &union_text),
    ] {
        std::fs::write(dir.join(name), text).map_err(|e| format!("{name}: {e}"))?;
    }
    Ok(headline)
}

/// How many values the two sorted lists share.
fn intersect(a: &[u32], b: &[u32]) -> usize {
    let (mut i, mut j, mut n) = (0, 0, 0);
    while i < a.len() && j < b.len() {
        match a[i].cmp(&b[j]) {
            std::cmp::Ordering::Less => i += 1,
            std::cmp::Ordering::Greater => j += 1,
            std::cmp::Ordering::Equal => {
                n += 1;
                i += 1;
                j += 1;
            }
        }
    }
    n
}

/// For a strided run: per slot of the element, how many distinct values and
/// how many look like a 64-bit hash (both halves non-zero, not a small
/// number). The slot holding the handlers shows as `code`.
fn slot_stats(c: &Candidate) -> String {
    let stride = c.step / 8;
    if stride <= 1 {
        return "stride 1".into();
    }
    let span = (c.len * c.step).min(8 << 20);
    let mut data = vec![0u8; span];
    let mut read = 0usize;
    // SAFETY: the same own-process read as the scan.
    let ok = unsafe {
        ReadProcessMemory(GetCurrentProcess(), c.address, data.as_mut_ptr(), span, &mut read)
    };
    if ok == 0 {
        return "slots unreadable".into();
    }
    let qwords: Vec<u64> = data[..read]
        .chunks_exact(8)
        .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
        .collect();
    let mut out = format!("stride {stride}:");
    for slot in 0..stride {
        let values: Vec<u64> = qwords.iter().skip(slot).step_by(stride).copied().collect();
        let mut distinct = values.clone();
        distinct.sort_unstable();
        distinct.dedup();
        let hashlike = values
            .iter()
            .filter(|&&v| (v >> 32) != 0 && (v & 0xFFFF_FFFF) != 0 && (v >> 47) == 0)
            .count();
        let _ = write!(
            out,
            " [slot {slot}: {} distinct of {}, {} hash-like]",
            distinct.len(),
            values.len(),
            hashlike
        );
    }
    out
}

/// Runs of code pointers, found over a stream of qwords (strides 1 to 8
/// elements, so (key, handler) pairs and wider buckets show) and of u32s.
struct Scanner<'a> {
    config: &'a Config,
    tag: &'static str,
    /// [stride - 1][phase]
    runs: [[Run; MAX_STRIDE]; MAX_STRIDE],
    rvas: Vec<Vec<u32>>,
    narrow: Run,
    narrow_rvas: Vec<u32>,
}

impl<'a> Scanner<'a> {
    fn new(config: &'a Config, tag: &'static str) -> Self {
        Scanner {
            config,
            tag,
            runs: [[Run::default(); MAX_STRIDE]; MAX_STRIDE],
            rvas: vec![Vec::new(); MAX_STRIDE * MAX_STRIDE],
            narrow: Run::default(),
            narrow_rvas: Vec::new(),
        }
    }

    /// Consumes `bytes` at `address`; chunks of a region arrive in order and
    /// every chunk is a multiple of 8 bytes, so no qword straddles two.
    fn feed(
        &mut self,
        address: usize,
        bytes: &[u8],
        found: &mut Vec<Candidate>,
        hist: &mut [usize; 7],
    ) {
        let base_q = address / 8;
        for (k, raw) in bytes.chunks_exact(8).enumerate() {
            let value = u64::from_le_bytes(raw.try_into().unwrap());
            let q = base_q + k;
            let hit = self.config.target(value);
            for stride in 1..=MAX_STRIDE {
                let slot = (stride - 1) * MAX_STRIDE + q % stride;
                let run = &mut self.runs[stride - 1][q % stride];
                match hit {
                    Some(rva) => {
                        if run.len == 0 {
                            run.start = q;
                        }
                        run.len += 1;
                        self.rvas[slot].push(rva);
                    }
                    None if run.len > 0 => {
                        let done = *run;
                        *run = Run::default();
                        let rvas = std::mem::take(&mut self.rvas[slot]);
                        emit(self.config, self.tag, stride * 8, false, done, rvas, found, hist);
                    }
                    None => {}
                }
            }
            // The two 32-bit halves, as RVAs (a table of offsets).
            for (half, word) in raw.chunks_exact(4).enumerate() {
                let w = u32::from_le_bytes(word.try_into().unwrap());
                let at = q * 2 + half;
                match self.config.target32(w) {
                    Some(rva) => {
                        if self.narrow.len == 0 {
                            self.narrow.start = at;
                        }
                        self.narrow.len += 1;
                        self.narrow_rvas.push(rva);
                    }
                    None if self.narrow.len > 0 => {
                        let done = std::mem::take(&mut self.narrow);
                        let rvas = std::mem::take(&mut self.narrow_rvas);
                        emit(self.config, self.tag, 4, true, done, rvas, found, hist);
                    }
                    None => {}
                }
            }
        }
    }

    /// The region (or its readable part) ended: close every open run.
    fn finish_all(&mut self, found: &mut Vec<Candidate>, hist: &mut [usize; 7]) {
        for stride in 1..=MAX_STRIDE {
            for phase in 0..stride {
                let run = std::mem::take(&mut self.runs[stride - 1][phase]);
                let rvas = std::mem::take(&mut self.rvas[(stride - 1) * MAX_STRIDE + phase]);
                if run.len > 0 {
                    emit(self.config, self.tag, stride * 8, false, run, rvas, found, hist);
                }
            }
        }
        let run = std::mem::take(&mut self.narrow);
        let rvas = std::mem::take(&mut self.narrow_rvas);
        if run.len > 0 {
            emit(self.config, self.tag, 4, true, run, rvas, found, hist);
        }
    }
}

/// Records a finished run when it is long enough. `run.start` counts qwords
/// (or u32s for the narrow kind) from address 0.
#[allow(clippy::too_many_arguments)]
fn emit(
    config: &Config,
    tag: &'static str,
    step: usize,
    narrow: bool,
    run: Run,
    mut rvas: Vec<u32>,
    found: &mut Vec<Candidate>,
    hist: &mut [usize; 7],
) {
    if run.len < config.min_run {
        return;
    }
    rvas.sort_unstable();
    rvas.dedup();
    let bucket = match rvas.len() {
        0..16 => 0,
        16..64 => 1,
        64..100 => 2,
        100..200 => 3,
        200..500 => 4,
        500..1000 => 5,
        _ => 6,
    };
    hist[bucket] += 1;
    // Only runs that could be listed keep their handler sets.
    if rvas.len() < config.min_distinct.min(100) {
        return;
    }
    let address = if narrow { run.start * 4 } else { run.start * 8 };
    found.push(Candidate {
        address,
        len: run.len,
        step: if narrow { 4 } else { step },
        narrow,
        targets: rvas,
        tag,
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(min_run: usize) -> Config {
        Config {
            base: 0x1_4000_0000,
            size: 0x10_0000,
            exec: vec![(0x1000, 0x8_0000)],
            writable_image: vec![],
            min_run,
            min_distinct: 8,
            all: false,
        }
    }

    fn run(config: &Config, qwords: &[u64]) -> Vec<Candidate> {
        let bytes: Vec<u8> = qwords.iter().flat_map(|q| q.to_le_bytes()).collect();
        let mut found = Vec::new();
        let mut hist = [0; 7];
        let mut scanner = Scanner::new(config, "test");
        scanner.feed(0x10_0000, &bytes, &mut found, &mut hist);
        scanner.finish_all(&mut found, &mut hist);
        found
    }

    #[test]
    fn finds_a_dense_array_and_a_key_handler_table() {
        let c = config(16);
        let code = |i: u64| 0x1_4000_0000 + 0x1000 + i * 0x10;
        // 40 consecutive handlers, then a gap, then 40 (hash, handler) pairs.
        let mut q: Vec<u64> = (0..40).map(code).collect();
        q.push(7);
        for i in 0..40 {
            q.push(0x1234_5678_9ABC_DEF0 ^ i);
            q.push(code(100 + i));
        }
        let found = run(&c, &q);
        let dense: Vec<_> = found.iter().filter(|f| f.step == 8 && f.len == 40).collect();
        assert_eq!(dense.len(), 1);
        assert_eq!(dense[0].targets.len(), 40);
        assert_eq!(dense[0].address, 0x10_0000);
        let pairs: Vec<_> = found.iter().filter(|f| f.step == 16 && f.len == 40).collect();
        assert_eq!(pairs.len(), 1);
        assert_eq!(pairs[0].len, 40);
        assert_eq!(pairs[0].address, 0x10_0000 + 41 * 8 + 8);
    }

    #[test]
    fn ignores_short_runs_and_pointers_outside_code() {
        let c = config(16);
        let mut q: Vec<u64> = (0..8).map(|i| 0x1_4000_1000 + i * 8).collect();
        q.extend((0..64).map(|i| 0x1_4000_0000 + 0x9_0000 + i * 8));
        assert!(run(&c, &q).is_empty());
    }
}
