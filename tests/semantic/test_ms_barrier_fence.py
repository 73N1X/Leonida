"""s_barrier that not every invocation may reach is a memory fence only (barriers_off_top).

VIctory lowers a barrier outside the top level of the structured tree as a fence (victory-shader emit/lower.rs
`fence_only`), and VItoriaBr's NVIDIA fork does the same for its mesh shaders ("BAR.SYNC deadlocks in wave32",
911267c): an OpControlBarrier some invocations never reach hangs an NVIDIA GPU. Compute shaders meet such barriers in
barrier rounds instead (needs_barrier_rounds), so this covers mesh shaders. Hand-written program, no game data.

    python -I tests/semantic/test_ms_barrier_fence.py -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_wave64_compute_tiers as tiers  # noqa: E402  (puts tools/ on sys.path)

PROGRAM = [
    "v_cmp_gt_u32_e32 vcc_lo, 16, v0",
    "s_and_saveexec_b32 s0, vcc_lo",
    "s_cbranch_execz skip",
    "s_barrier",                 # only lanes 0-15 run this block
    "skip:",
    "s_or_b32 exec_lo, exec_lo, s0",
    "s_barrier",                 # every lane again, but after a branch: the tree guards it too
    "s_endpgm",
]


class MeshBarrierFence(unittest.TestCase):
    def test_guarded_barrier_is_a_fence(self):
        spv, g = tiers.translate("ms_barrier_fence", PROGRAM, threads=32, emulate=False)
        self.assertEqual(g.stage, "ms")
        # The structured tree guards every block after a branch, the join too, so both are fences: conservative (a
        # mesh shader that exchanges LDS across a join barrier may read early) but never a hang.
        self.assertEqual(len(g.fence_only_barriers), 2)
        self.assertEqual(spv.count("OpMemoryBarrier"), 2)
        self.assertEqual(spv.count("OpControlBarrier"), 1)   # the output epilogue's, which every invocation reaches

    def test_top_level_barrier_only_stays_a_barrier(self):
        spv, g = tiers.translate("ms_barrier_top", ["s_barrier", "s_endpgm"], threads=32, emulate=False)
        self.assertEqual(g.fence_only_barriers, set())
        self.assertEqual(spv.count("OpMemoryBarrier"), 0)
        self.assertEqual(spv.count("OpControlBarrier"), 2)   # the program's and the output epilogue's


if __name__ == "__main__":
    unittest.main()
