"""--loop-limit (docs/design.md, "Loop iteration cap"): a translated loop runs at most N times per entry into it,
N = 128 in a pixel shader, 256 in a vertex, geometry or mesh shader and 4096 in a compute shader. Then a branch back
takes its other way out, and a loop with no other way (a jump back) leaves the program; a pixel is discarded.
The rule and the numbers are VIctory's (victory/crates/victory-shader/src/emit/lower.rs, LOOP_LIMIT and
count_iteration; commit 15b0091e8, the per-stage values from 570a941e9): loops over data the host had not mirrored
yet read zeros, ran about four billion times and lost the GPU.

The fixtures are hand-written (tests/semantic/fixtures/loop_limit_*.asm, no game data). Each loop counts its passes
in s4 and ends when s4 reaches s5, a value from user data, so one program shows a loop below the cap (unchanged),
one whose end never comes (s5 = 0, as with data that reads as zeros) and one cut short above the cap. The expected
words follow from the rule alone: the first block of a loop runs at most N times, so a loop closed by a branch
back exports min(s5, N) for s5 >= 1 and N for s5 = 0, and a loop that jumps back exports s5 for 1 <= s5 <= N and
is discarded otherwise.

The barrier fixture is a compute shader of two waves where only the second one never leaves its loop. That wave
must end at the cap without holding the first one at s_barrier: RDNA counts only waves that have not ended, and
the translation's barrier rounds (native wave32) and wave64 emulation rounds count a wave that left at the cap as
done. spirv_mini_interp.run_workgroup and test_wave64_emulate_opt.run_emulated_workgroup require every invocation
to reach each barrier at the same point, so a stall is an error here, not a hang.

    python -m unittest discover -s tests/semantic -p test_loop_limit.py
"""
import gc
import os
import struct
import tempfile
import unittest
import warnings

import support
import spirv_mini_interp as si
import spirv_wave_interp as wi
from test_loop_ballot_barrier import run_compute_workgroup
from test_wave64_emulate_opt import run_emulated_workgroup

rdna2spirv = support.rdna2spirv
VSHARP = [0, 0, 0x10000, 0x31016FAC]       # raw buffer V#: base 0, num_records 0x10000 bytes


class Limits:
    """Set rdna2spirv's --loop-limit (and --dispatch-loop / --wave64-emulate) for one translation."""

    def __init__(self, structured=True, emulate=False, **limits):
        self.want = (structured, emulate, limits)

    def __enter__(self):
        self.saved = (rdna2spirv.STRUCTURED, rdna2spirv.WAVE64_EMULATE, dict(rdna2spirv.LOOP_LIMITS))
        structured, emulate, limits = self.want
        rdna2spirv.STRUCTURED, rdna2spirv.WAVE64_EMULATE = structured, emulate
        rdna2spirv.LOOP_LIMITS.update(limits)
        return self

    def __exit__(self, *exc):
        rdna2spirv.STRUCTURED, rdna2spirv.WAVE64_EMULATE, limits = self.saved
        rdna2spirv.LOOP_LIMITS.clear()
        rdna2spirv.LOOP_LIMITS.update(limits)


def ps_words(fixture, s5, **limits):
    """mrt0 of the pixel-shader fixture with s5 from user data, or None when the pixel is discarded. The
    dispatch-loop form (structured=False) has a switch, which spirv_wave_interp runs (lane 0 of a wave32)."""
    with Limits(**limits):
        spv = support.translate_lines("ps_" + fixture, support.resolve_labels(support.asm_fixture(fixture)))
    if not limits.get("structured", True):
        out = wi.run_wave(spv, wave=32, push_constants=[0] * 5 + [s5])["outputs"][0]
    else:
        out = si.run_fragment(spv, push_constants=[0] * 5 + [s5])
    return None if not out else list(out["%out_mrt0"])


def cs_fssm(threads, wave):
    """An FSSM header that says the wave size (u32 0xD4 bit 5: wave32) and the group: RSRC2 0x8088, threads x 1 x 1."""
    blob = bytearray(0x148)
    blob[:4] = b"FSSM"
    struct.pack_into("<I", blob, 0xD4, 0x20 if wave == 32 else 0)
    struct.pack_into("<4I", blob, 0x138, 0x8088, threads, 1, 1)
    return bytes(blob)


def translate_cs(threads, wave, **options):
    lines = support.resolve_labels(support.asm_fixture("loop_limit_barrier"))
    with Limits(**options):
        spv = support.translate_lines("cs_loop_limit_barrier", lines, fssm=cs_fssm(threads, wave))
        return spv, rdna2spirv.translate.last_gen


class PixelBackEdge(unittest.TestCase):
    """loop_limit_back.asm: the branch back is not taken at the cap; the code after the loop runs."""

    def test_default_cap_is_128(self):
        for s5, want in ((1, 1), (7, 7), (127, 127), (128, 128), (129, 128), (500, 128), (0, 128)):
            with self.subTest(s5=s5):
                self.assertEqual(ps_words("loop_limit_back", s5), [want] * 4)

    def test_dispatch_loop_form_is_capped_the_same_way(self):
        for s5, want in ((7, 7), (0, 128)):
            with self.subTest(s5=s5):
                self.assertEqual(ps_words("loop_limit_back", s5, structured=False), [want] * 4)

    def test_other_limit_and_no_cap(self):
        self.assertEqual(ps_words("loop_limit_back", 0, ps=5), [5] * 4)
        self.assertEqual(ps_words("loop_limit_back", 9, ps=0), [9] * 4)
        with self.assertRaisesRegex(si.UnsupportedConstruct, "loop iteration limit"):
            ps_words("loop_limit_back", 0, ps=0)          # without the cap the loop never ends


class PixelJumpBack(unittest.TestCase):
    """loop_limit_jump.asm: the jump back has no other way at the cap: the pixel is discarded."""

    def test_default_cap_discards(self):
        for s5, want in ((1, [1] * 4), (7, [7] * 4), (128, [128] * 4), (129, None), (0, None)):
            with self.subTest(s5=s5):
                self.assertEqual(ps_words("loop_limit_jump", s5), want)
        self.assertEqual(ps_words("loop_limit_jump", 0, structured=False), None)
        self.assertEqual(ps_words("loop_limit_jump", 3, ps=3), [3] * 4)
        self.assertEqual(ps_words("loop_limit_jump", 4, ps=3), None)

    def test_kill_flag_only_where_a_loop_can_leave(self):
        with Limits():
            back = support.translate_lines("ps_b", support.resolve_labels(support.asm_fixture("loop_limit_back")))
            jump = support.translate_lines("ps_j", support.resolve_labels(support.asm_fixture("loop_limit_jump")))
        self.assertNotIn("%lc_kill", back)
        self.assertIn("%lc_kill = OpVariable %_ptr_Function_bool Function %false", jump)


class VertexStage(unittest.TestCase):
    def test_vertex_cap_is_256(self):
        lines = [ln.replace("exp mrt0 v0, v0, v0, v0 done vm", "exp pos0 v0, v0, v0, v0 done")
                 for ln in support.asm_fixture("loop_limit_back")]
        with Limits():
            spv = support.translate_lines("vs_loop_limit_back", support.resolve_labels(lines))
        for s5, want in ((9, 9), (0, 256), (300, 256)):
            with self.subTest(s5=s5):
                out = si.run_fragment(spv, push_constants=[0] * 5 + [s5])
                self.assertEqual(list(out["%out_pos"]), [want] * 4)


class ComputeBarrier(unittest.TestCase):
    """loop_limit_barrier.asm: the second wave leaves at the cap; the first still passes s_barrier and stores 3."""

    def test_wave32_barrier_rounds(self):
        spv, gen = translate_cs(64, 32, cs=8)
        self.assertEqual(gen.control_flow, "barrier rounds")
        self.assertTrue(gen.loop_exits)
        for rev in (False, True):
            with self.subTest(reverse_subgroups=rev):
                mem = run_compute_workgroup(spv, 64, 32, push_constants=VSHARP, reverse_subgroups=rev)
                self.assertEqual(mem, {4 * t: 3 for t in range(32)})

    def test_wave64_emulated_two_waves(self):
        spv, gen = translate_cs(128, 64, cs=8, emulate=True)
        self.assertTrue(gen.emulate)
        mem, barriers = run_emulated_workgroup(spv, (128, 1, 1), push_constants=VSHARP + [0] * 28)
        self.assertEqual({a: w for a, w in mem.items() if a < 0x1000}, {4 * t: 3 for t in range(64)})
        self.assertGreater(barriers, 8)

    def test_wave64_emulated_one_wave(self):
        # One wave: the whole workgroup leaves together, skipping the same barriers; below the cap it stores 3.
        for cap, want in ((3, {4 * t: 3 for t in range(64)}), (2, {})):
            with self.subTest(cap=cap):
                spv, gen = translate_cs(64, 64, cs=cap, emulate=True)
                mem, _ = run_emulated_workgroup(spv, (64, 1, 1), push_constants=VSHARP + [0] * 28)
                self.assertEqual({a: w for a, w in mem.items() if a < 0x1000}, want)

    def test_default_compute_cap_and_metadata(self):
        spv, gen = translate_cs(64, 32)
        self.assertEqual(gen.loop_limit, 4096)
        self.assertIn(f"OpULessThan %bool %t", spv)
        self.assertIn(rdna2spirv.Gen("cs").cu(4096), spv)
        meta = rdna2spirv.shader_meta(gen, spv)
        self.assertEqual((meta["loop_limit"], meta["capped_loops"]), (4096, 1))
        straight = support.translate_lines("ps_straight", ["v_mov_b32 v0, 1.0", "exp mrt0 v0, v0, v0, v0 done vm",
                                                           "s_endpgm"])
        self.assertNotIn("%lc0", straight)
        self.assertNotIn("loop_limit", rdna2spirv.shader_meta(rdna2spirv.translate.last_gen, straight))


class Option(unittest.TestCase):
    def test_parse(self):
        parse = rdna2spirv._loop_limit_arg
        self.assertEqual(parse("128,256,4096"), {"ps": 128, "vs": 256, "gs": 256, "ms": 256, "cs": 4096})
        self.assertEqual(parse("0"), {"ps": 0, "vs": 0, "gs": 0, "ms": 0, "cs": 0})
        for bad in ("1,2", "a", "-1", "1,2,3,4", ""):
            with self.subTest(bad=bad), self.assertRaises(rdna2spirv.argparse.ArgumentTypeError):
                parse(bad)
        self.assertEqual(rdna2spirv.LOOP_LIMITS, {"ps": 128, "vs": 256, "gs": 256, "ms": 256, "cs": 4096})


if __name__ == "__main__":
    unittest.main()
