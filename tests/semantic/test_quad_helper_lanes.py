"""Quad reads and helper lanes in pixel shaders (docs/design.md, "Quad-confined DPP and ds_swizzle in pixel
shaders", "First active lane in a pixel shader's helper lane", "Discarded pixels stay helpers").

On the console whole-quad mode (`s_wqm`) puts a pixel shader's helper lanes in EXEC: the pixels of a 2x2 quad
outside the triangle run with the others so that quad reads and derivatives see them. Vulkan keeps a helper
active for quad (and derivative) operations only; whether it joins a ballot or a shuffle is up to the driver. Ported
from VIctory, which runs the title:
  * DPP quad_perm, a dpp8 that stays in each quad, and a ds_swizzle that does (QUAD_PERM, or masks that keep lane
    bits 2-4) are quad operations (OpGroupNonUniformQuadBroadcast / QuadSwap), so a helper across a triangle edge
    is read as on hardware (victory/crates/victory-shader/src/emit/lower.rs quad_pick and the DPP and ds_swizzle
    lowering; commit 921d8d3b4);
  * a helper lane's v_readfirstlane takes its own value when its own ballot leaves it out, so a waterfall loop
    serves it in the first pass (VIctory 060195536; without it such loops spun with only helpers left until
    DEVICE_HUNG);
  * a pixel's discard (s_andn2 of the live mask, s_wqm of what is left) keeps the killed lane running as a helper,
    so the survivors' quad reads still see it (VIctory 77053a0c5). Leonida never ends a lane before the end of the
    program, so this holds already; the test pins it.

spirv_mini_interp.run_workgroup models the driver that leaves helpers out: `helpers` take part in no ballot, vote
or shuffle (a shuffle from one is undefined, an error once used) but do take part in quad operations. The fixtures
are hand-written (tests/semantic/fixtures/quad_helper_reads.asm, discard_keeps_helper.asm); every expected word
follows from the lane numbers: v1 = 16 * lane + 5 in every lane.

    python -m unittest discover -s tests/semantic -p test_quad_helper_lanes.py
"""
import unittest

import support
import spirv_mini_interp as si

rdna2spirv = support.rdna2spirv
VSHARP = [0, 0, 0x10000, 0x31016FAC]       # raw buffer V#: base 0, num_records 0x10000 bytes
HELPERS = frozenset({1, 2, 3, 9, 11})      # quad 0 has one covered pixel, quad 2 two; the others are whole


def translate(fixture, name=None):
    return support.translate_lines(name or "ps_" + fixture, support.resolve_labels(support.asm_fixture(fixture)))


def run(spv, helpers=HELPERS, lanes=32):
    return si.run_workgroup(spv, lanes, 32, inputs_for=lambda i: {"%sg_lane": i}, push_constants=VSHARP + [0] * 28,
                            memory={}, helpers=helpers, return_outputs=True)


def v1(lane):
    return 16 * lane + 5


class QuadReads(unittest.TestCase):
    def test_quad_operations_read_helpers(self):
        spv = translate("quad_helper_reads")
        self.assertIn("OpGroupNonUniformQuadBroadcast", spv)      # quad_perm:[1,1,1,1]
        self.assertIn("OpGroupNonUniformQuadSwap", spv)           # dpp8 reverse (diagonal), QUAD_PERM 2,3,0,1
        self.assertIn("OpCapability GroupNonUniformQuad", spv)
        for helpers in (HELPERS, frozenset()):
            with self.subTest(helpers=sorted(helpers)):
                mem, _ = run(spv, helpers)
                for lane in range(32):
                    if lane in helpers:
                        continue                                    # a helper's stores are dropped
                    b, q = lane & ~3, lane & 3
                    self.assertEqual([mem.get(16 * lane + 4 * k) for k in range(4)],
                                     [v1(b + 1), v1(b + 3 - q), v1(b + (q ^ 2)), b + 1], f"lane {lane}")

    def test_only_quad_confined_patterns_become_quad_operations(self):
        quad = ["v_mov_b32_dpp v2, v1 quad_perm:[2,2,0,1] row_mask:0xf bank_mask:0xf",   # general pattern
                "v_mov_b32_dpp v2, v1 dpp8:[1,0,3,2,6,7,4,5]",                            # differs per quad
                "ds_swizzle_b32 v2, v1 offset:swizzle(SWAP,2)",
                "ds_swizzle_b32 v2, v1 offset:swizzle(BROADCAST,4,1)",
                'ds_swizzle_b32 v2, v1 offset:swizzle(BITMASK_PERM,"ppp1i")']
        wide = ["v_mov_b32_dpp v2, v1 row_shr:1 row_mask:0xf bank_mask:0xf",
                "v_mov_b32_dpp v2, v1 dpp8:[4,5,6,7,0,1,2,3]",                            # leaves the quad
                "ds_swizzle_b32 v2, v1 offset:swizzle(SWAP,4)",
                "ds_swizzle_b32 v2, v1 offset:swizzle(BROADCAST,8,1)"]
        for line in quad + wide:
            with self.subTest(line=line):
                prog = ["v_mbcnt_lo_u32_b32 v0, -1, 0", "v_lshlrev_b32 v1, 4, v0", "v_add_nc_u32 v1, 5, v1",
                        line, "s_waitcnt lgkmcnt(0)", "v_lshlrev_b32 v7, 4, v0",
                        "buffer_store_dword v2, v7, s[0:3], 0 offen", "exp mrt0 v1, v1, v1, v1 done vm",
                        "s_endpgm"]
                spv = support.translate_lines("ps_quad_kind", support.resolve_labels(prog))
                self.assertEqual("OpGroupNonUniformShuffle" not in spv, line in quad)
                if line in quad:
                    mem, _ = run(spv)
                    for lane in sorted(set(range(32)) - HELPERS):
                        self.assertEqual(mem[16 * lane], v1(self.source(line, lane)), f"{line}: lane {lane}")

    @staticmethod
    def source(line, lane):
        """The lane each quad-confined line reads, from the ISA's rules written out here."""
        b, q = lane & ~3, lane & 3
        if "quad_perm" in line:
            return b + (2, 2, 0, 1)[q]
        if "dpp8" in line:
            sel = (1, 0, 3, 2, 6, 7, 4, 5)[lane & 7]
            return (lane & ~7) | sel
        if "SWAP,2" in line:
            return lane ^ 2
        if "BROADCAST,4,1" in line:
            return b + 1
        # BITMASK_PERM "ppp1i": bit 4..2 kept, bit 1 forced to 1, bit 0 inverted
        return (lane & 0x1C) | 2 | ((lane & 1) ^ 1)

    def test_mesh_and_vertex_shaders_keep_their_shuffles(self):
        # No helpers outside fragment shaders, and quad operations there need quadOperationsInAllStages.
        prog = ["v_mbcnt_lo_u32_b32 v0, -1, 0", "v_mov_b32_dpp v2, v0 quad_perm:[1,1,1,1] row_mask:0xf bank_mask:0xf",
                "exp pos0 v2, v2, v2, v2 done", "s_endpgm"]
        spv = support.translate_lines("vs_quad_kind", support.resolve_labels(prog))
        self.assertIn("OpGroupNonUniformShuffle", spv)
        self.assertNotIn("OpGroupNonUniformQuad", spv)


class HelperFirstLane(unittest.TestCase):
    def test_helper_left_out_of_its_ballot_reads_its_own_value(self):
        spv = translate("quad_helper_reads")
        mem, _ = run(spv)
        # Quads 0 and 2 read quad lane 1, a helper: run with its own value 1 and 9, as the console's waterfall does.
        for lane in (0, 8, 10):
            self.assertEqual(mem[16 * lane + 12], (lane & ~3) + 1)

    def test_without_the_rule_the_helper_is_never_served(self):
        # Control: the first lane's value for a helper too (the translation before the rule). The driver that leaves
        # helpers out never lets a helper's value come first, so quad lane 1 of quads 0 and 2 is never run and keeps
        # its old v5 (0). A driver whose vote counted the helpers but whose ballot did not would loop forever here.
        saved = rdna2spirv.Gen.helper_own_value
        rdna2spirv.Gen.helper_own_value = lambda self, ballot, own, first: first
        try:
            spv = translate("quad_helper_reads", "ps_quad_helper_reads_control")
        finally:
            rdna2spirv.Gen.helper_own_value = saved
        mem, _ = run(spv)
        self.assertEqual([mem[16 * lane + 12] for lane in (0, 8, 10)], [0, 0, 0])
        self.assertEqual(mem[16 * 4 + 12], 5)                     # a whole quad is unchanged

    def test_lanes_in_their_ballot_are_unchanged(self):
        # With no helper left out every lane is in its own ballot: v_readfirstlane is the first lane's value.
        spv = translate("quad_helper_reads")
        mem, _ = run(spv, frozenset())
        self.assertEqual([mem[16 * lane + 12] for lane in range(0, 32, 4)], list(range(1, 32, 4)))
        # Other stages have no helpers: their v_readfirstlane is unchanged (no own-bit test).
        vs = support.translate_lines("vs_rfl", ["v_readfirstlane_b32 s4, v0", "v_mov_b32 v1, s4",
                                                "exp pos0 v1, v1, v1, v1 done", "s_endpgm"])
        self.assertIn("OpGroupNonUniformShuffle", vs)
        self.assertNotIn("OpGroupNonUniformBallotBitExtract", vs)


class DiscardKeepsHelper(unittest.TestCase):
    def test_killed_lane_is_read_by_its_quad_and_discarded(self):
        spv = translate("discard_keeps_helper")
        for helpers in (frozenset(), frozenset({6, 7})):         # killed lanes are not driver helpers; these are
            with self.subTest(helpers=sorted(helpers)):
                _, outputs = run(spv, helpers)
                for lane in range(32):
                    b = lane & ~3
                    want = None if lane & 3 == 1 else {"%out_mrt0": (v1(b + 1),) * 4}
                    got = outputs[lane] or None
                    if lane in helpers:
                        continue                                    # a driver helper's output is dropped anyway
                    self.assertEqual(got, want, f"lane {lane}")


if __name__ == "__main__":
    unittest.main()
