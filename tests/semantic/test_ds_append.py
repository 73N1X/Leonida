"""ds_append / ds_consume: one atomic per wave on the LDS dword at M0[15:0] + offset.

The counter moves by the number of active lanes and every active lane gets the value before it. The translation lets
the lowest active lane do the atomic and broadcasts its result; wave32 only. 28 of the title's captured compute
programs use it. Hand-written programs, no game data.

    python -I tests/semantic/test_ds_append.py -v
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import lds_run  # noqa: E402
from support import rdna2spirv  # noqa: E402

ADDR = 0x40


def program(op):
    return [
        f"v_mov_b32_e32 v1, {ADDR:#x}", "v_mov_b32_e32 v2, 10", "ds_write_b32 v1, v2", "s_waitcnt lgkmcnt(0)",
        f"s_mov_b32 m0, {ADDR:#x}", f"{op} v5", "s_waitcnt lgkmcnt(0)",
        *lds_run.store("v5", 0),
        f"v_mov_b32_e32 v1, {ADDR:#x}", "ds_read_b32 v6, v1", "s_waitcnt lgkmcnt(0)", *lds_run.store("v6", 1),
    ]


class DsAppend(unittest.TestCase):
    def test_append_returns_the_old_count_to_every_lane(self):
        mem = lds_run.run(program("ds_append"), lanes=4)
        for lane in range(4):
            self.assertEqual(lds_run.out(mem, lane, 0), 10, f"lane {lane}")
        self.assertEqual(lds_run.out(mem, 0, 1), 14)  # the lane that did the atomic sees all four added

    def test_consume_subtracts(self):
        mem = lds_run.run(program("ds_consume"), lanes=4)
        self.assertEqual(lds_run.out(mem, 1, 0), 10)
        self.assertEqual(lds_run.out(mem, 0, 1), 6)

    def test_gds_is_refused(self):
        with self.assertRaises(rdna2spirv.Unsupported):
            lds_run.translate(lds_run.prologue() + ["ds_append v5 gds"])


if __name__ == "__main__":
    unittest.main()
