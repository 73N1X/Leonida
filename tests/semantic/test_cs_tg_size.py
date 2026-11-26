"""The compute thread-group info SGPR (COMPUTE_PGM_RSRC2.TG_SIZE_EN, bit 10).

It follows the workgroup-id SGPRs and holds the wave's index in its group in bits 24:20 and the group's wave count in
bits 5:0 (Mesa ac_nir_lower_intrinsics_to_args: load_subgroup_id, load_num_subgroups). 91 of the title's captured
compute programs read it and were refused. Hand-written program, no game data.

    python -I tests/semantic/test_cs_tg_size.py -v
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import lds_run  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

# 4 user SGPRs (the output V#), workgroup id x in s4 (TGID_X_EN, bit 7), the thread-group info in s5 (bit 10).
RSRC2 = (4 << 1) | (1 << 7) | (1 << 10)
GROUP = (64, 1, 1)  # two wave32 waves


class ThreadGroupInfo(unittest.TestCase):
    def test_wave_index_and_count(self):
        body = lds_run.prologue() + ["v_mov_b32_e32 v2, s5"] + lds_run.store("v2", 0)
        spv = lds_run.translate(body, rsrc2=RSRC2, size=GROUP, name="cs_tg_size")
        mem = si.run_workgroup(spv, 64, 32,
                               inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0),
                                                     "%sg_lane": i % 32, "%sg_id": i // 32},
                               push_constants=lds_run.VSHARP + [0] * 28, memory={})
        for lane in (0, 31, 32, 63):
            self.assertEqual(lds_run.out(mem, lane, 0), ((lane // 32) << 20) | 2, f"lane {lane}")


if __name__ == "__main__":
    unittest.main()
