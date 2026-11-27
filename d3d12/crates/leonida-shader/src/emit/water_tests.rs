//! The water's tile programs (`CS_TransmissionVolumetrics`: 8 x 8 groups of one console wave64 with
//! 512 bytes of LDS) on a host of 32-lane waves, as hand-written fixtures of the constructs that
//! made them untranslatable or wrong: per-lane early returns around LDS that is written and read
//! (the program is taken wave-wide, [`super::widen`]), and the tile's starting minimum and
//! maximum stored by thread (0, 0) after those returns (stored by every thread at the start
//! instead, [`super::lds_init`]). Lowered, validated by `dxil.dll` when present, and run on the
//! host GPU when it takes waves of 32. The expected values are what the console computes.

use super::host::Host;
use super::wave64_tests::{native_of, whole_uav};
use super::{HostShader, lds_init, lower, validator};
use crate::native::{ComputeIo, NativeProgram, StageIo};
use crate::{dxbc, io, ir, resource, structure};

const fn vop1(op: u32, vdst: u32, src0: u32) -> u32 {
    0x7E00_0000 | vdst << 17 | op << 9 | src0
}

const fn vop2(op: u32, vdst: u32, src0: u32, vsrc1: u32) -> u32 {
    op << 25 | vdst << 17 | vsrc1 << 9 | src0
}

const fn vopc(op: u32, src0: u32, vsrc1: u32) -> u32 {
    0x7C00_0000 | op << 17 | vsrc1 << 9 | src0
}

/// DS `op` with its data and address VGPRs (`vdst` for a read), the two byte offsets.
const fn ds(op: u32, vdst: u32, addr: u32, data0: u32, data1: u32, offset0: u32, offset1: u32) -> [u32; 2] {
    [0x36 << 26 | op << 18 | offset1 << 8 | offset0, vdst << 24 | data1 << 16 | data0 << 8 | addr]
}

const DS_WRITE_B32: u32 = 0x0D;
const DS_WRITE2_B32: u32 = 0x0E;
const DS_MIN_U32: u32 = 0x07;
const DS_MAX_U32: u32 = 0x08;
const DS_READ2_B32: u32 = 0x37;

const V: u32 = 256;
const fn k(n: u32) -> u32 {
    128 + n
}
const K_M1: u32 = 193;
const S_WAITCNT_LGKM0: u32 = 0xBF8C_C07F;
const S_AND_SAVEEXEC_VCC: u32 = 0xBE8C_246A; // s_and_saveexec_b64 s[12:13], vcc
const S_RESTORE_EXEC: u32 = 0xBEFE_040C; // s_mov_b64 exec, s[12:13]
const SAVE_EXEC_14: u32 = 0xBE8E_047E; // s_mov_b64 s[14:15], exec
const RESTORE_EXEC_14: u32 = 0xBEFE_040E; // s_mov_b64 exec, s[14:15]

/// `buffer_store_dword v<data>, v<addr>, s[8:11], 0 offen offset:<offset>`.
const fn store(data: u32, addr: u32, offset: u32) -> [u32; 2] {
    [0x38 << 26 | 0x1C << 18 | 1 << 12 | offset, 0x80 << 24 | 2 << 16 | data << 8 | addr]
}

/// One 8 x 8 console wave64 (thread ids in v0 and v1) with 512 bytes of LDS. Every thread but the
/// one whose number is s0 goes on (the others return early, `s_cbranch_execz` past the rest); of
/// those the one at (0, 0), if it is among them, stores the tile's starting maximum (0, dword 0)
/// and minimum (-1, dword 1), then every thread `ds_max_u32`s and `ds_min_u32`s its key (its
/// number + 10) in and reads both back, and stores them at `out[2 * number]` (max) and
/// `out[2 * number + 1]` (min). No barrier: the console runs one wave.
///
/// `max_start` replaces the 0; `stray`: a write at an address that is not a constant;
/// `atomics: false` leaves them out.
fn tile_program(max_start: u32, stray: bool, atomics: bool) -> NativeProgram {
    let mut after = vec![
        vop2(0x1C, 3, V + 1, 0), // v_or_b32 v3, v1, v0
        SAVE_EXEC_14,
        vopc(0xD2, k(0), 3), // v_cmpx_eq_u32 exec, 0, v3
        vop1(1, 4, k(0)),    // v_mov_b32 v4, 0
        vop1(1, 5, K_M1),    // v_mov_b32 v5, -1
    ];
    let start = if max_start != 0 {
        after.extend([vop1(1, 10, 0xFF), max_start]); // v_mov_b32 v10, lit
        10
    } else {
        4
    };
    after.extend(ds(DS_WRITE2_B32, 0, 4, start, 5, 0, 1)); // ds_write2_b32 v4, v<start>, v5 offset1:1
    after.push(RESTORE_EXEC_14);
    after.extend([
        vop2(0x25, 6, k(10), 2), // v_add_nc_u32 v6, 10, v2
        vop1(1, 4, k(0)),        // v_mov_b32 v4, 0
    ]);
    if stray {
        after.extend(ds(DS_WRITE_B32, 0, 2, 6, 0, 0, 0)); // ds_write_b32 v2, v6: any address
    }
    if atomics {
        after.extend(ds(DS_MIN_U32, 0, 4, 6, 0, 4, 0)); // ds_min_u32 v4, v6 offset:4
        after.extend(ds(DS_MAX_U32, 0, 4, 6, 0, 0, 0)); // ds_max_u32 v4, v6
    }
    after.extend([S_WAITCNT_LGKM0]);
    after.extend(ds(DS_READ2_B32, 7, 4, 0, 0, 0, 1)); // ds_read2_b32 v[7:8], v4 offset1:1
    after.extend([
        S_WAITCNT_LGKM0,
        vop2(0x1A, 9, k(3), 2), // v_lshlrev_b32 v9, 3, v2
    ]);
    after.extend(store(7, 9, 0));
    after.extend(store(8, 9, 4));
    let mut body = vec![
        vop2(0x1A, 2, k(3), 1),  // v_lshlrev_b32 v2, 3, v1
        vop2(0x25, 2, V + 2, 0), // v_add_nc_u32 v2, v2, v0: the thread's number
        vopc(0xC5, 0, 2),        // v_cmp_ne_u32 vcc, s0, v2
        S_AND_SAVEEXEC_VCC,
        0xBF88_0000 | after.len() as u32, // s_cbranch_execz to the restore of EXEC
    ];
    body.extend(after);
    body.push(S_RESTORE_EXEC);
    let mut native = native_of(&body, true, 64);
    native.io = StageIo::Cs(ComputeIo {
        rsrc2: 5 << 1 | 1 << 11 | 1 << 15, // 5 user SGPRs, thread ids x and y, 512 bytes of LDS
        threads: [8, 8, 1],
    });
    native
}

fn hoists(native: &NativeProgram) -> Vec<(u32, u32)> {
    let p = crate::spill::build(native).expect("build");
    lds_init::init_hoists(&p, 128)
}

#[test]
fn accumulator_starts_are_hoisted() {
    assert_eq!(hoists(&tile_program(0, false, true)), [(0, 0), (1, u32::MAX)]);
    // a start that is not the maximum's identity stays where it is
    assert_eq!(hoists(&tile_program(5, false, true)), [(1, u32::MAX)]);
    // plain stores (a flag, data) are not accumulators
    assert_eq!(hoists(&tile_program(0, false, false)), []);
    // a write that could land anywhere: nothing is known
    assert_eq!(hoists(&tile_program(0, true, true)), []);
}

/// The program translated for a host of 32-lane waves by `emit`, which takes its early returns
/// wave-wide (the other forms refuse LDS that is read and written where not every lane runs).
fn translated(native: &NativeProgram, hoist: bool) -> HostShader {
    let p = crate::spill::build(native).expect("build");
    let st = structure::structure(&p).expect("structure");
    let res = resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resource::track");
    let io = io::raise(&p, native).expect("io::raise");
    let bytes = dxbc::build(&[]);
    let c = dxbc::Container::parse(&bytes).expect("container");
    let was = lower::TEST_HOST_WAVE.replace(Some(32));
    let was_hoist = lds_init::TEST_HOIST.replace(Some(hoist));
    let shader = super::emit(&c, native, &p, &st, &io, &res);
    lds_init::TEST_HOIST.set(was_hoist);
    lower::TEST_HOST_WAVE.set(was);
    let shader = shader.expect("the tile program translates for 32-lane waves");
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        v.validate(&mut copy).expect("DXIL validation failed");
    }
    shader
}

/// What the console leaves in the UAV: `(max, min)` of the keys of the threads that went on, at
/// every one of them; nothing for the thread that returned.
fn expected(returned: u32) -> Vec<u32> {
    let keys: Vec<u32> = (0..64).filter(|&t| t != returned).map(|t| t + 10).collect();
    let (max, min) = (*keys.iter().max().unwrap(), *keys.iter().min().unwrap());
    (0..64u32).flat_map(|t| if t == returned { [0, 0] } else { [max, min] }).collect()
}

#[test]
fn a_tile_whose_corner_thread_returned_early_still_starts_its_minimum_and_maximum() {
    let native = tile_program(0, false, true);
    let shader = translated(&native, true);
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    if Host::new(false).is_err() {
        eprintln!("no SM 6.6 hardware device; not run");
        return;
    }
    // 0: the corner returns (the minimum is the next thread's key); 5 and 63: another does (the
    // maximum drops for 63); 64: none
    for returned in [0u32, 5, 32, 63, 64] {
        // (twice: the second group of a card finds what the first left in the LDS)
        for _ in 0..2 {
            let Some(out) = whole_uav(&shader, [returned, 0], 128) else {
                return;
            };
            assert_eq!(out, expected(returned), "thread {returned} returned early");
        }
    }
    eprintln!("ran on the hardware device: the tile's minimum and maximum for every early return");
}

#[test]
fn the_tile_program_keeps_working_with_the_hoist_off_when_its_corner_stays() {
    // what the hoist changes is only the tile whose corner returned: with the corner in, the
    // program's own store does the job
    let native = tile_program(0, false, true);
    let shader = translated(&native, false);
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    if Host::new(false).is_err() {
        eprintln!("no SM 6.6 hardware device; not run");
        return;
    }
    for returned in [5u32, 63, 64] {
        let Some(out) = whole_uav(&shader, [returned, 0], 128) else {
            return;
        };
        assert_eq!(out, expected(returned), "thread {returned} returned early");
    }
}
