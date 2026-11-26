"""Wave64 compute forms the title's captured programs use (hand-written, no game data):

  * s_bitreplicate_b64_b32 with a constant source into EXEC (26 programs: `s_bitreplicate_b64_b32 exec,
    0xff00ff00`). D[2i+1] = D[2i] = S0[i], so the result is a constant 64-bit mask, the same as s_mov_b64 of it.
    0xff00ff00 sets source bits 8-15 and 24-31, so lanes 16-31 and 48-63 run. Executed on a 64-wide subgroup.
  * s_flbit_i32_b64 of EXEC (18 programs: the wave's last active lane as 63 - sN). The leading-zero count of the
    whole wave's mask: with lanes 0-39 on it is 24. Executed on a 64-wide subgroup.
  * ds_append / ds_consume in an emulated wave64 (--wave64-emulate; 24 programs). One atomic for the wave: the
    first active lane of 64 adds the count, the others add 0, and the first lane's old value goes to every lane
    through the exchange buffer. The emulated module uses Workgroup memory and a round loop, which the interpreter
    does not run, so this checks the translation and (with spirv-as and spirv-val on PATH) its validity.

    python -I tests/semantic/test_wave64_append_bitreplicate.py -v
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_wave64_compute_tiers as tiers  # noqa: E402  (puts tools/ on sys.path)
import rdna2spirv  # noqa: E402

BITREPLICATE = [
    "s_mov_b64 s[10:11], exec",
    "s_bitreplicate_b64_b32 exec, 0xff00ff00",
    "v_mov_b32_e32 v4, 7",
    *tiers.STORE,
    "s_mov_b64 exec, s[10:11]",
    "s_endpgm",
]


FLBIT_EXEC = [
    "v_cmp_gt_u32_e32 vcc, 40, v0",
    "s_and_saveexec_b64 s[10:11], vcc",
    "s_flbit_i32_b64 s12, exec",
    "s_ff1_i32_b64 s13, exec",
    "s_mov_b64 exec, s[10:11]",
    "v_mov_b32_e32 v4, s12",
    "v_lshl_or_b32 v4, s13, 8, v4",
    *tiers.STORE,
    "s_endpgm",
]


def append_program(op):
    return [
        "s_mov_b64 s[10:11], exec",                 # a 64-bit EXEC read: the program is wave64
        "s_mov_b32 m0, 0x40",
        f"{op} v5 offset:4" if op else "v_mov_b32_e32 v5, 0",
        "s_waitcnt lgkmcnt(0)",
        "v_mov_b32_e32 v4, v5",
        *tiers.STORE,
        "s_endpgm",
    ]


class BitreplicateExec(unittest.TestCase):
    def test_constant_source_sets_exec(self):
        spv, gen = tiers.translate("cs_bitreplicate_exec", BITREPLICATE, emulate=False)
        self.assertTrue(gen.wave64)
        words = tiers.run(spv, 64, 64)
        want = [7 if 16 <= lane < 32 or lane >= 48 else None for lane in range(64)]
        self.assertEqual(words, want)


class FlbitExec(unittest.TestCase):
    def test_last_and_first_lane_of_the_wave(self):
        spv, gen = tiers.translate("cs_flbit_exec", FLBIT_EXEC, emulate=False)
        self.assertTrue(gen.wave64)
        self.assertEqual(tiers.run(spv, 64, 64), [24] * 64)       # flbit 63 - 39 = 24, ff1 0


class AppendEmulated(unittest.TestCase):
    def translate(self, op):
        spv, gen = tiers.translate("cs_append_w64", append_program(op), tiers=False)
        self.assertTrue(gen.wave64 and gen.emulate)
        return spv

    def test_append_translates_with_one_counted_atomic(self):
        spv = self.translate("ds_append")
        # The emulation's own scheduler counts with OpAtomicIAdd too: the program adds exactly one.
        base, _ = tiers.translate("cs_append_w64", append_program(None), tiers=False)
        self.assertEqual(spv.count("OpAtomicIAdd") - base.count("OpAtomicIAdd"), 1)
        self.assertIn("OpBitCount", spv)

    def test_consume_subtracts(self):
        self.assertIn("OpAtomicISub", self.translate("ds_consume"))

    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "needs spirv-as and spirv-val")
    def test_valid(self):
        spv = self.translate("ds_append")
        with tempfile.TemporaryDirectory() as tmp:
            src, out = os.path.join(tmp, "a.spvasm"), os.path.join(tmp, "a.spv")
            with open(src, "w", encoding="utf-8", newline="\n") as f:
                f.write(spv)
            for cmd in (["spirv-as", "--target-env", "vulkan1.3", src, "-o", out],
                        ["spirv-val", "--target-env", "vulkan1.3", out]):
                r = subprocess.run(cmd, capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr + r.stdout)

    def test_not_emulated_wave64_is_refused(self):
        with self.assertRaises(rdna2spirv.Unsupported):
            tiers.translate("cs_append_w64", append_program("ds_append"), tiers=False, emulate=False)


if __name__ == "__main__":
    unittest.main()
