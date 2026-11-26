"""The wave size of a console pipeline stage (device slot 57), which no header gives.

Its wave32 code moves vcc as an SGPR pair with _b64 ops, which the loose code test reads as wave64, but its
*_saveexec_b32 exist only in wave32 code and it has none of the strict wave64 markers: such a vertex or pixel program
with an FSPX code file beside it is wave32, so its compare masks in vcc_hi translate. Hand-written program, no game
data.

    python -I tests/semantic/test_console_stage_wave32.py -v
"""
import gc
import os
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from support import STUB_DXBC, rdna2spirv  # noqa: E402

PROGRAM = [
    "s_mov_b64 vcc, 0",                       # vcc as a pair: the loose wave64 test
    "s_or_saveexec_b32 s0, -1",               # a wave32-only op: every lane stays on
    "v_mov_b32_e32 v1, 1.0",
    "v_mov_b32_e32 v2, 2.0",
    "v_cmp_lt_f32_e64 vcc_hi, v1, v2",
    "v_cndmask_b32_e64 v3, 0, 1.0, vcc_hi",
    "v_mov_b32_e32 v4, 0",
    "exp mrt0 v3, v4, v4, v4 done vm",
    "s_endpgm",
]


def translate(name, lines, with_code):
    text = f"; {name}  hand-written test program\n" + "".join("\t" + ln + "\n" for ln in lines)
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        with open(os.path.join(tmp, "disasm", name + ".s"), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        with open(os.path.join(tmp, name + ".dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        if with_code:  # the console stage's kind-6 bytes beside it, as tools/fspx_unpack.py writes them
            os.mkdir(os.path.join(tmp, "fspx"))
            with open(os.path.join(tmp, "fspx", name + ".code"), "wb") as f:
                f.write(b"\0" * 64)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            result = rdna2spirv.translate(os.path.join(tmp, "disasm", name + ".s"))
            gc.collect()
        return result


class ConsoleStageWave32(unittest.TestCase):
    def test_console_stage_is_wave32(self):
        # Translated as wave32: its vcc_hi compare mask is accepted (as wave64 it is refused, the test below). The
        # value through vcc_hi is test_vcmp_vcc_hi.py's; the saveexec here needs a subgroup this runner does not model.
        spvasm = translate("ps_console_wave32", PROGRAM, with_code=True)
        self.assertIn("%vcc_hi", spvasm)

    def test_without_a_code_file_it_stays_refused(self):
        with self.assertRaises(rdna2spirv.Unsupported):
            translate("ps_console_wave64", PROGRAM, with_code=False)


if __name__ == "__main__":
    unittest.main()
