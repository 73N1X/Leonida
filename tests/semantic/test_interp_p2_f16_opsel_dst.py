"""v_interp_p2_f16 with VOP3 OP_SEL[3]: the f16 result goes to the high half of the destination.

llvm-mc can neither print nor assemble op_sel on this op (its `high` modifier is word 1 bit 8, the attribute's upper
half), so a listing shows the word without OP_SEL[3] (word 0 bit 14) where the console compiler set it (32 console
pipeline stages were refused on the printed-encoding check). The check now takes that one bit from the stored bytes and
the result is written to the high half, the low half kept. Hand-written program, no game data.

    python -I tests/semantic/test_interp_p2_f16_opsel_dst.py -v
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from support import rdna2spirv, translate_listing  # noqa: E402

# llvm-mc -mcpu=gfx1030 -show-encoding
PROGRAM = [
    ("v_mov_b32_e32 v5, 0", "80 02 0a 7e"),
    ("v_interp_p1ll_f16 v3, v0, attr0.x", "03 00 42 d7 00 00 02 00"),
    ("v_interp_p2_f16 v5, v1, attr0.x, v3", "05 00 5a d7 00 02 0e 04"),
    ("exp mrt0 v5, v5, v5, v5 done vm", "0f 18 00 f8 05 05 05 05"),
    ("s_endpgm", "00 00 81 bf"),
]
NAME = "ps_interp_p2_f16_opsel"


def listing():
    size = sum(len(bytes.fromhex(enc)) for _, enc in PROGRAM)
    lines = [f"; {NAME}  code 0x0..0x{size:x} (fspx), gfx1030, 0 invalid words"]
    for text, enc in PROGRAM:
        lines.append(f"\t{text:<40}; encoding: [{','.join(f'0x{b:02x}' for b in bytes.fromhex(enc))}]")
    return ("\n".join(lines) + "\n").encode("utf-8")


def stored(bit_byte):
    """The program's bytes with byte 1 of the v_interp_p2_f16 word set to `bit_byte` (0x40: OP_SEL[3])."""
    raw = b""
    for i, (_, enc) in enumerate(PROGRAM):
        b = bytearray(bytes.fromhex(enc))
        if i == 2:
            b[1] = bit_byte
        raw += bytes(b)
    return raw


def translate(code):
    import tempfile, warnings, gc
    with tempfile.TemporaryDirectory() as tmp:
        for sub in ("disasm", "fspx"):
            os.mkdir(os.path.join(tmp, sub))
        with open(os.path.join(tmp, "disasm", NAME + ".s"), "wb") as f:
            f.write(listing())
        with open(os.path.join(tmp, "fspx", NAME + ".code"), "wb") as f:
            f.write(code)
        from support import STUB_DXBC
        with open(os.path.join(tmp, NAME + ".dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            out = rdna2spirv.translate(os.path.join(tmp, "disasm", NAME + ".s"))
            gc.collect()
        return out


class InterpP2F16OpSelDst(unittest.TestCase):
    def test_op_sel_3_writes_the_high_half(self):
        plain, high = translate(stored(0x00)), translate(stored(0x40))
        # The high-half write shifts the f16 result up by 16 and keeps the low half (mask 0xFFFF); the plain one
        # keeps the high half (mask 0xFFFF0000) instead.
        self.assertNotEqual(plain, high)
        self.assertIn("OpShiftLeftLogical", high)

    def test_another_dropped_bit_is_still_refused(self):
        with self.assertRaises(rdna2spirv.Unsupported):
            translate(stored(0x20))  # OP_SEL[2]: not the destination bit


if __name__ == "__main__":
    unittest.main()
