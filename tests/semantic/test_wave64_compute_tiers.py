"""SHADER-10 for compute and mesh: emulate a wave64 CS/MS only when its 32-lane halves are not exact as subgroups.

With --wave64-emulate the translator runs tools/wave64_halves.py on every wave64 compute and mesh shader
(rdna2spirv.compute_halves, module flag WAVE64_TIERS that main() sets) and keeps the workgroup-memory emulation for
tier C only. Tiers A and B run natively, one console half per subgroup: the thread index comes from the subgroup
(SubgroupId * SubgroupSize + SubgroupLocalInvocationId), LDS or memory accesses the wave's lockstep ordered get a
workgroup barrier in front, and with no memory write at all s_barrier is left out (docs/design.md, "Wave64 compute
and mesh: partition invariance"). Hand-written programs (no game data):

  * Classification: the tier, reasons and idioms in the translator's result, and whether it emulated.
  * Execution on spirv_mini_interp.run_workgroup: the native module on 32- and 64-wide subgroups, with invocations
    placed in invocation order and in a shuffled order (a host that does not give a subgroup consecutive
    LocalInvocationIndex values), and with the subgroups run one after another between barriers
    (subgroup_order) instead of in lockstep. Expected words come from the ISA. Negative controls: the old native
    translation (no relabelling) under the shuffled order, and the LDS program without its barrier.
"""
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402  (path set above)
import spirv_mini_interp as si  # noqa: E402
import rdna2spirv  # noqa: E402

VSHARP = [0, 0, 0x10000, 0x31016FAC]       # a raw buffer at address 0 for s[0:3]
LANE_ID = ["v_mbcnt_lo_u32_b32 v1, -1, 0", "v_mbcnt_hi_u32_b32 v1, -1, v1"]
STORE = ["v_lshlrev_b32_e32 v21, 2, v0", "buffer_store_dword v4, v21, s[0:3], 0 offen"]


def fssm(threads):
    """A compute FSSM header: RSRC2 with 4 user SGPRs, TGID_X and an LDS allocation of 128 dwords."""
    h = bytearray(0x148)
    struct.pack_into("<4I", h, 0x138, 0x8088, threads, 1, 1)
    return bytes(h)


def mesh_dxbc(threads=128, maxv=128, maxp=128):
    psv = bytearray(48)
    struct.pack_into("<HH", psv, 12, maxv, maxp)
    psv[24], psv[27] = 13, 2
    struct.pack_into("<III", psv, 36, threads, 1, 1)
    part = struct.pack("<I", len(psv)) + bytes(psv)
    body = b"PSV0" + struct.pack("<I", len(part)) + part
    head = bytearray(b"DXBC") + bytes(16) + struct.pack("<III", 1, 36 + len(body), 1) + struct.pack("<I", 36)
    return bytes(head) + body


def translate(name, lines, threads=64, tiers=True, emulate=True):
    """(SPIR-V text, Gen) of a hand-written wave64 program; `name` starts with cs_ or ms_ (the stage)."""
    prev = rdna2spirv.WAVE64_EMULATE, rdna2spirv.WAVE64_TIERS
    rdna2spirv.WAVE64_EMULATE, rdna2spirv.WAVE64_TIERS = emulate, tiers
    try:
        listing = support.resolve_labels(lines)
        if name.startswith("ms_"):
            spv = support.translate_lines(name, listing, dxbc=mesh_dxbc(threads))
        else:
            spv = support.translate_lines(name, listing, fssm=fssm(threads))
        return spv, rdna2spirv.translate.last_gen
    finally:
        rdna2spirv.WAVE64_EMULATE, rdna2spirv.WAVE64_TIERS = prev


def run(spv, threads, sg, perm=None, order=None, mesh=False):
    """Buffer words (or, for a mesh shader, each invocation's outputs) of one workgroup. Invocation i sits at slot
    perm[i] (subgroup perm[i] // sg, lane perm[i] % sg) and reads LocalInvocationId i."""
    perm = list(range(threads)) if perm is None else perm

    def inputs(i):
        d = {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%local_index": i, "%num_wg": (1, 1, 1),
             "%sg_lane": perm[i] % sg, "%sg_id": perm[i] // sg, "%sg_size": sg}
        return d
    memory = {}
    res = si.run_workgroup(spv, threads, sg, inputs_for=inputs, push_constants=VSHARP + [0] * 28, memory=memory,
                           lanes=perm, subgroup_order=order, return_outputs=mesh)
    if mesh:
        return res[1]
    return [memory.get(4 * k) for k in range(threads)]


def shuffled(n, seed=1):
    perm = list(range(n))
    random.Random(seed).shuffle(perm)
    return perm


def barriers(spv, semantics=0x948):
    """The OpControlBarrier lines with these memory semantics (the ones compute_halves puts before an access)."""
    consts = dict(re.findall(r"(%\S+) = OpConstant %uint (\d+)", spv))
    return [ln for ln in spv.splitlines() if "OpControlBarrier" in ln and
            consts.get(ln.split()[-1]) == str(semantics)]


# ---- programs ---------------------------------------------------------------------------------------------------
# Tier A: a DPP row shift and a ds_swizzle swap of 16-lane groups stay inside 32-lane halves; the lane id is exact.
CS_HALF_LOCAL = support.asm_fixture("cs_w64_half_local")
HALF_LOCAL_WORDS = [((t - 1 if t % 16 else 0) << 8) + (t ^ 16) for t in range(64)]

# Tier B: a one-wave group shares LDS without barriers (the console compiler leaves them out): lane l writes
# LDS[l], then reads LDS[l ^ 32], the other half's word. Natively it needs a barrier before the read.
CS_LDS_SWAP = support.asm_fixture("cs_w64_lds_swap")
LDS_SWAP_WORDS = [100 + (t ^ 32) for t in range(64)]

# Tier C: the same exchange inside a lane-masked region (a half whose lanes are all off skips it): no barrier can go
# there, so it stays emulated.
CS_LDS_IN_BRANCH = LANE_ID + [
    "s_mov_b64 s[6:7], exec", "v_lshlrev_b32_e32 v2, 2, v1", "v_mov_b32_e32 v4, 0",
    "v_cmp_gt_u32_e32 vcc, 48, v1", "s_and_saveexec_b64 s[8:9], vcc", "s_cbranch_execz skip",
    "ds_write_b32 v2, v1", "v_xor_b32_e32 v3, 0x80, v2", "ds_read_b32 v4, v3", "s_waitcnt lgkmcnt(0)",
    "skip:", "s_or_b64 exec, exec, s[8:9]"] + STORE + ["s_endpgm"]

# Tier C: v_readlane of a varying value at lane 40 crosses halves.
CS_READLANE = LANE_ID + ["s_mov_b64 s[6:7], exec", "v_readlane_b32 s10, v1, 40", "v_mov_b32_e32 v4, s10"] + STORE + \
    ["s_endpgm"]

# Tier B: a wave index from the first lane's thread id (bits 6 and up of D + lane are the wave's).
CS_WAVE_BASE = LANE_ID + [
    "s_mov_b64 s[6:7], exec", "v_readfirstlane_b32 s10, v0", "s_lshr_b32 s11, s10, 6",
    "v_mov_b32_e32 v4, s11"] + STORE + ["s_endpgm"]

# Tier C: an s_barrier where a vote decides (a half whose lanes are all off skips it), with an LDS write.
CS_VOTE_BARRIER = LANE_ID + [
    "s_mov_b64 s[6:7], exec", "v_lshlrev_b32_e32 v2, 2, v1", "v_mov_b32_e32 v4, 0",
    "v_cmp_gt_u32_e32 vcc, 8, v1", "s_and_saveexec_b64 s[8:9], vcc", "s_cbranch_execz skip",
    "ds_write_b32 v2, v1", "s_barrier", "skip:", "s_or_b64 exec, exec, s[8:9]"] + STORE + ["s_endpgm"]

# Tier A: a two-wave group exchanging LDS words across waves with s_barrier inside a scalar loop (no vote): natively
# the barriers run in barrier rounds (needs_barrier_rounds), as in a wave32 compute shader.
CS_BARRIER_LOOP = LANE_ID + [
    "s_mov_b64 s[6:7], exec", "v_lshlrev_b32_e32 v2, 2, v0", "s_mov_b32 s20, 0", "v_mov_b32_e32 v4, 0", "loop:",
    "v_add_nc_u32_e32 v5, s20, v0", "ds_write_b32 v2, v5", "s_waitcnt lgkmcnt(0)", "s_barrier",
    "v_xor_b32_e32 v3, 0x100, v2", "ds_read_b32 v6, v3", "s_waitcnt lgkmcnt(0)", "v_add_nc_u32_e32 v4, v4, v6",
    "s_barrier", "s_add_u32 s20, s20, 1", "s_cmp_lt_u32 s20, 3", "s_cbranch_scc1 loop"] + STORE + ["s_endpgm"]

# Tier C: a scalar load of a word another lane stored, in a lane-masked region (mem-in-wave).
CS_STORE_SLOAD = LANE_ID + [
    "s_mov_b64 s[6:7], exec", "v_mov_b32_e32 v4, 0", "v_cmp_gt_u32_e32 vcc, 40, v1", "s_and_saveexec_b64 s[8:9], vcc",
    "s_cbranch_execz skip", "v_mov_b32_e32 v5, 512", "buffer_store_dword v1, v5, s[0:3], 0 offen",
    "s_buffer_load_dword s12, s[0:3], 0x200", "s_waitcnt lgkmcnt(0)", "v_mov_b32_e32 v4, s12",
    "skip:", "s_or_b64 exec, exec, s[8:9]"] + STORE + ["s_endpgm"]

# Tier A: a 64-bit compare printed in wave32 syntax (one SGPR for the pair), ANDed with EXEC and branched on: the
# compare of a uniform value under a narrowed EXEC, inside the execz region of that EXEC, votes as the wave does.
CS_UNIFORM_UNDER_EXEC = LANE_ID + [
    "s_mov_b64 s[6:7], exec", "v_mov_b32_e32 v4, 1", "v_cmp_gt_u32_e32 vcc_lo, 40, v1",
    "s_and_saveexec_b64 s[8:9], vcc", "s_cbranch_execz done", "v_cmp_lt_u32_e64 s4, s20, 5",
    "s_and_b64 vcc, exec, s[4:5]", "s_cbranch_vccnz done", "v_mov_b32_e32 v4, 7",
    "done:", "s_or_b64 exec, exec, s[8:9]"] + STORE + ["s_endpgm"]

# A mesh shader shaped like the corpus ones: the group index from the first lane's v5 >> 7, the thread index from
# the wave id in s3, the allocation by thread 0 under an execz skip with its null-primitive path behind a vccnz and
# an s_barrier, then a barrier and the exports. No memory write: the barriers are left out natively.
MS_SKELETON = support.asm_fixture("ms_w64_skeleton")

# Tier C: the allocation's counts come from a lane count (each half would send its own).
MS_COUNTED = [
    "s_mov_b64 exec, -1", "v_mbcnt_lo_u32_b32 v0, -1, 0", "s_bfe_u32 s1, s3, 0x40018",
    "v_mbcnt_hi_u32_b32 v0, -1, v0", "v_lshl_add_u32 v0, s1, 6, v0", "v_cmp_gt_u32_e32 vcc_lo, 70, v0",
    "s_bcnt1_i32_b64 s32, vcc", "s_mov_b64 s[0:1], exec", "v_cmpx_eq_u32_e32 0, v0", "s_cbranch_execz done",
    "s_mov_b32 m0, s32", "s_sendmsg sendmsg(MSG_GS_ALLOC_REQ)", "done:", "s_mov_b64 exec, s[0:1]", "s_barrier",
    "s_endpgm"]


class Classification(unittest.TestCase):
    def tier(self, name, lines, tier, reasons=(), idioms=None, threads=64, native=None):
        spv, g = translate(name, lines, threads)
        w = g.w64c
        self.assertIsNotNone(w, name)
        self.assertEqual((w["tier"], sorted(w["reasons"])), (tier, sorted(reasons)), f"{name}: {w['sites']}")
        if idioms is not None:
            self.assertEqual(sorted(w["idioms"]), sorted(idioms), name)
        self.assertEqual(g.emulate, tier == "C", name)
        self.assertEqual(g.relabel, tier != "C", name)
        if tier == "C":
            self.assertNotIn("OpGroupNonUniform", spv)
        return spv, g

    def test_half_local_cross_lane_is_tier_a(self):
        self.tier("cs_half_local", CS_HALF_LOCAL, "A", idioms=[])

    def test_lds_exchange_in_a_one_wave_group_gets_a_barrier(self):
        spv, g = self.tier("cs_lds_swap", CS_LDS_SWAP, "B", idioms=["wave-memory-order"])
        self.assertEqual(len(g.w64_sync), 1)
        self.assertEqual(len(barriers(spv)), 1)

    def test_lds_exchange_under_a_lane_branch_is_tier_c(self):
        self.tier("cs_lds_branch", CS_LDS_IN_BRANCH, "C", ["lds-in-wave"])

    def test_readlane_of_varying_data_is_tier_c(self):
        self.tier("cs_readlane40", CS_READLANE, "C", ["cross-lane"])

    def test_wave_base_from_first_lane_thread_id(self):
        self.tier("cs_wave_base", CS_WAVE_BASE, "B", idioms=["readfirstlane-wave-base"])
        # bits below 6 are the half's: a shift by 5 keeps the half (2 * wave + half), tier C
        lines = [ln.replace("s_lshr_b32 s11, s10, 6", "s_lshr_b32 s11, s10, 5") for ln in CS_WAVE_BASE]
        self.tier("cs_wave_half", lines, "C", ["first-lane"])
        # x is D + lane only when the group's x size is a multiple of 64
        spv, g = translate("cs_wave_base32", CS_WAVE_BASE, threads=32)
        self.assertEqual(g.w64c["tier"], "C")

    def test_barrier_in_a_scalar_loop_runs_in_rounds(self):
        spv, g = self.tier("cs_bar_loop", CS_BARRIER_LOOP, "A", threads=128)
        self.assertTrue(g.bar_rounds)
        self.assertFalse(g.w64_sync)

    def test_barrier_a_vote_decides_is_tier_c(self):
        self.tier("cs_vote_barrier", CS_VOTE_BARRIER, "C", ["vote-barrier"], threads=128)

    def test_scalar_load_of_a_stored_word_is_tier_c(self):
        self.tier("cs_store_sload", CS_STORE_SLOAD, "C", ["mem-in-wave"])

    def test_uniform_compare_under_a_narrowed_exec(self):
        self.tier("cs_uniform_exec", CS_UNIFORM_UNDER_EXEC, "A", idioms=[])
        # without the execz in front, a half whose lanes are all off votes no while the wave votes yes
        lines = [ln for ln in CS_UNIFORM_UNDER_EXEC if ln != "s_cbranch_execz done"]
        self.tier("cs_uniform_noexecz", lines, "C", ["unsafe-vote-branch"])

    def test_partial_subgroup_group_size_is_tier_c(self):
        self.tier("cs_group48", CS_HALF_LOCAL, "C", ["group-size"], threads=48)

    def test_mesh_skeleton(self):
        spv, g = self.tier("ms_skeleton", MS_SKELETON, "B", threads=128,
                           idioms=["readfirstlane-wave-base"])
        self.assertTrue(g.drop_barriers)
        self.assertIn("%mli = OpCopyObject %uint %rl_t", spv)
        self.assertNotIn("OpLoad %uint %local_index", spv)
        self.assertEqual(sum("OpControlBarrier" in ln for ln in spv.splitlines()), 1)   # the epilogue's own

    def test_mesh_allocation_from_a_lane_count_is_tier_c(self):
        spv, g = translate("ms_counted", MS_COUNTED, 128)
        self.assertEqual(g.w64c["tier"], "C")
        self.assertIn("tainted-message", g.w64c["reasons"])
        self.assertTrue(g.emulate)

    def test_without_tiers_every_shader_is_emulated(self):
        for name, lines in (("cs_half_local_all", CS_HALF_LOCAL), ("ms_skeleton_all", MS_SKELETON)):
            with self.subTest(name):
                _, g = translate(name, lines, 128 if name.startswith("ms") else 64, tiers=False)
                self.assertTrue(g.emulate)
                self.assertIsNone(g.w64c)


class Execution(unittest.TestCase):
    def test_half_local_is_exact_in_halves_and_any_placement(self):
        spv, _ = translate("cs_half_local", CS_HALF_LOCAL)
        for sg in (64, 32):
            for perm in (None, shuffled(64)):
                for order in (None, [1, 0]):
                    with self.subTest(subgroup=sg, shuffled=perm is not None, order=order):
                        self.assertEqual(run(spv, 64, sg, perm, order), HALF_LOCAL_WORDS)

    def test_relabelling_is_what_makes_the_shuffled_placement_exact(self):
        # negative control: the native translation without tiers takes the lane from LocalInvocationIndex and the
        # shuffle from the subgroup lane, which disagree once a subgroup is not 32 consecutive threads
        spv, g = translate("cs_half_local_native", CS_HALF_LOCAL, emulate=False, tiers=False)
        self.assertFalse(g.relabel)
        self.assertEqual(run(spv, 64, 32), HALF_LOCAL_WORDS)
        self.assertNotEqual(run(spv, 64, 32, shuffled(64)), HALF_LOCAL_WORDS)

    def test_lds_exchange_with_and_without_its_barrier(self):
        spv, _ = translate("cs_lds_swap", CS_LDS_SWAP)
        for sg in (64, 32):
            for order in (None, [1, 0], [0, 1]):
                with self.subTest(subgroup=sg, order=order):
                    self.assertEqual(run(spv, 64, sg, shuffled(64, 3), order), LDS_SWAP_WORDS)
        # negative control: drop the barrier; the upper half, run first, reads words the lower half has not written
        bare = spv
        for ln in barriers(spv):
            bare = bare.replace(ln + "\n", "")
        self.assertEqual(len(barriers(bare)), 0)
        with self.assertRaises((AssertionError, si.UndefinedResult, si.UnsupportedConstruct, TypeError)):
            self.assertEqual(run(bare, 64, 32, None, [1, 0]), LDS_SWAP_WORDS)

    def test_uniform_compare_under_exec_is_exact(self):
        spv, _ = translate("cs_uniform_exec", CS_UNIFORM_UNDER_EXEC)
        for s20 in (3, 9):
            want = [7 if t < 40 and s20 >= 5 else 1 for t in range(64)]
            push = VSHARP + [0] * 16 + [s20] + [0] * 11
            for sg in (64, 32):
                with self.subTest(s20=s20, subgroup=sg):
                    memory = {}
                    si.run_workgroup(spv, 64, sg, push_constants=push, memory=memory,
                                     inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0),
                                                           "%sg_lane": i % sg, "%sg_id": i // sg, "%sg_size": sg})
                    self.assertEqual([memory.get(4 * k) for k in range(64)], want)

    def test_wave_base(self):
        spv, _ = translate("cs_wave_base", CS_WAVE_BASE, threads=128)
        for sg in (64, 32):
            with self.subTest(subgroup=sg):
                self.assertEqual(run(spv, 128, sg, shuffled(128, 5)), [t // 64 for t in range(128)])

    def test_mesh_skeleton_exports(self):
        spv, _ = translate("ms_skeleton", MS_SKELETON, 128)
        for sg in (64, 32):
            for perm in (None, shuffled(128, 7)):
                with self.subTest(subgroup=sg, shuffled=perm is not None):
                    # thread 0's subgroup alone runs the allocation: subgroups in turn, not in lockstep
                    outs = run(spv, 128, sg, perm, order=list(range(128 // sg)), mesh=True)
                    self.assertEqual({o["OpSetMeshOutputsEXT"] for o in outs}, {(3, 1)})
                    pos = {}
                    for i, o in enumerate(outs):
                        t = perm[i] if perm else i         # the relabelled thread index of invocation i
                        words = o["%ms_out_pos"]
                        if t < 3:
                            pos[t] = words[4 * t:4 * t + 4]
                    self.assertEqual(sorted(pos), [0, 1, 2])
                    for t, w in pos.items():
                        self.assertEqual(struct.unpack("<f", struct.pack("<I", w[0]))[0], float(t))


@unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "needs spirv-as and spirv-val")
class Validation(unittest.TestCase):
    def test_native_modules_validate(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, lines, threads in (("cs_half_local", CS_HALF_LOCAL, 64), ("cs_lds_swap", CS_LDS_SWAP, 64),
                                         ("cs_wave_base", CS_WAVE_BASE, 128), ("ms_skeleton", MS_SKELETON, 128),
                                         ("cs_bar_loop", CS_BARRIER_LOOP, 128)):
                with self.subTest(name):
                    spv, g = translate(name, lines, threads)
                    self.assertTrue(g.relabel)
                    asm, out = os.path.join(tmp, name + ".spvasm"), os.path.join(tmp, name + ".spv")
                    with open(asm, "w", encoding="utf-8", newline="\n") as f:
                        f.write(spv)
                    for cmd in (["spirv-as", "--target-env", "vulkan1.3", asm, "-o", out],
                                ["spirv-val", "--target-env", "vulkan1.3", out]):
                        r = subprocess.run(cmd, capture_output=True, text=True)
                        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
