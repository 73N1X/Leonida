"""Wave64 VALU lane masks printed in wave32 syntax are SGPR pairs (rdna2spirv.widen_wave64_masks).

The corpus listings come from llvm-mc's gfx10 default, wave32, so a wave64 program's VOP3 compare destination, carry
out and v_cndmask mask print as one SGPR (v_cmp_gt_u32_e64 s4, ...) where the hardware writes and reads s[4:5].
Until this rule the translator wrote s4 only, and a later 64-bit read of s[4:5] (s_and_b64 vcc, exec, s[4:5]) took
lanes 32..63 from the stale s5. fixtures/wave64_cmp_sgpr_pair.asm runs on all 64 lanes of a 64-wide subgroup
(spirv_mini_interp.run_workgroup); the expected words follow the RDNA2 ISA. The same program written with the pairs
spelled out must give the same words, and a wave32 program keeps the single SGPR.
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402  (path set above)
import spirv_mini_interp as si  # noqa: E402

VSHARP = [0, 0, 0x10000, 0x31016FAC]


def fssm(threads, wave64=True):
    h = bytearray(0x148)
    struct.pack_into("<I", h, 0, 0x4D535346)                     # "FSSM"
    struct.pack_into("<I", h, 0xD4, 0 if wave64 else 0x20)       # wave size flag
    struct.pack_into("<4I", h, 0x138, 0x8088, threads, 1, 1)
    return bytes(h)


def run(lines, threads=64, wave=64):
    spv = support.translate_lines("cs_mask_pairs", lines, fssm=fssm(threads, wave == 64))
    memory = {}
    si.run_workgroup(spv, threads, wave, push_constants=VSHARP + [0] * 28, memory=memory,
                     inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i % wave})
    return [[memory.get(12 * lane + 4 * k) for k in range(3)] for lane in range(threads)]


WANT = [[int(lane < 40), 9 * (lane & 1), int(lane >= 48)] for lane in range(64)]


class MaskPairs(unittest.TestCase):
    def test_wave32_syntax_writes_and_reads_the_pair(self):
        self.assertEqual(run(support.asm_fixture("wave64_cmp_sgpr_pair")), WANT)

    def test_same_as_the_pairs_spelled_out(self):
        lines = support.asm_fixture("wave64_cmp_sgpr_pair")
        spelled = [ln.replace("_e64 s4,", "_e64 s[4:5],").replace("_e64 v4, 0, 9, s8", "_e64 v4, 0, 9, s[8:9]")
                   .replace("_e64 s8,", "_e64 s[8:9],").replace("v5, s12,", "v5, s[12:13],") for ln in lines]
        self.assertNotEqual(spelled, lines)
        self.assertEqual(run(spelled), WANT)

    def test_wave32_keeps_one_sgpr(self):
        # wave32 (FSSM flag): the same compare writes s4 alone; s5 keeps its 0, read as data by the 64-bit op
        lines = [ln for ln in support.asm_fixture("wave64_cmp_sgpr_pair") if not ln.startswith("s_mov_b64")]
        lines = [ln.replace("s_and_b64 vcc, exec, s[4:5]", "s_and_b32 vcc_lo, exec_lo, s4")
                 .replace("s_and_b64 vcc, exec, s[12:13]", "s_and_b32 vcc_lo, exec_lo, s12")
                 .replace("v_mbcnt_hi_u32_b32 v1, -1, v1", "s_nop 0") for ln in lines]
        got = run(lines, threads=32, wave=32)
        self.assertEqual(got, WANT[:32])


if __name__ == "__main__":
    unittest.main()
