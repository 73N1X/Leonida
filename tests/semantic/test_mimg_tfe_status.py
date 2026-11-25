"""MIMG tfe / lwe status dword (SHADER-1266; docs/design.md, "MIMG fetch status (tfe, lwe)").

With tfe (or lwe) an MIMG read returns one more dword after its data, the fetch status (RDNA2 ISA, MIMG TFE/LWE):
non-zero for a texel that is not resident (tfe) or an LOD clamped by the resource (lwe). Every texel a Vulkan view
reads without sparse residency is resident, so the translation writes 0 there, after the data: dmask's bit count of
dwords, halved and rounded up under d16, four for a gather. Before, the status VGPR kept its old value, which the GPU
fixtures runtime/test/shaders/cs_imgload_tfe.asm and cs_imgload_d16_tfe.asm show (each status is seeded with 1000:
vk_store_test exited 1 on an RTX 4060 Ti). The fixture here is hand-written (fixtures/image_load_tfe.asm); the
image the test binds returns one texel, so every expected word follows from it.

    python -m unittest discover -s tests/semantic -p test_mimg_tfe_status.py
"""
import os
import struct
import sys
import unittest

import support
import spirv_mini_interp as si

TEXEL = (0.25, 0.5, 0.75, 1.0)


def fbits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def run_ps(lines):
    spv = support.translate_lines("ps_image_load_tfe", lines)
    tsharp = [0 if w == 1 else 0x11110000 + w for w in range(8)]      # word 1 = 0: a float format
    base = 0x1000
    memory = {base + 4 * w: word for w, word in enumerate(tsharp)}
    desc_map = [0, *tsharp, 1]                                          # one slot, bindless index 1
    images = [None, lambda coord, lod: TEXEL]
    _, outs = si.run_workgroup(spv, 1, 32, inputs_for=lambda i: {"%sg_lane": i}, push_constants=[base, 0],
                               memory=memory, resources={"%desc_map": desc_map, "%images": images},
                               return_outputs=True)
    return spv, list(outs[0]["%out_mrt0"])


class TfeStatus(unittest.TestCase):
    def test_status_after_the_data_is_zero(self):
        spv, got = run_ps(support.asm_fixture("image_load_tfe"))
        # v4 = texel.x, v5 = the first load's status, v6 = texel.x of the second (dmask 0x7: v6..v8), v9 its status
        self.assertEqual(got, [fbits(TEXEL[0]), 0, fbits(TEXEL[0]), 0])

    def test_d16_status_follows_the_packed_data(self):
        # Under d16 two 16-bit values share a VGPR: dmask 0x1 and 0x3 have one data VGPR, so the status is v5.
        for dmask in ("0x1", "0x3"):
            with self.subTest(dmask=dmask):
                lines = [ln.replace("dmask:0x1 dim:SQ_RSRC_IMG_2D unorm tfe", f"dmask:{dmask} dim:SQ_RSRC_IMG_2D unorm tfe d16")
                         for ln in support.asm_fixture("image_load_tfe")]
                lines = [ln for ln in lines if "v[6:9]" not in ln]
                lines = [ln.replace("exp mrt0 v4, v5, v6, v9", "exp mrt0 v4, v5, v5, v5") for ln in lines]
                _, got = run_ps(lines)
                self.assertEqual(got[1:], [0, 0, 0])

    def test_without_the_rule_the_status_keeps_its_seed(self):
        saved = support.rdna2spirv.Gen.mimg_status
        support.rdna2spirv.Gen.mimg_status = lambda self, op, a, mods: None
        try:
            _, got = run_ps(support.asm_fixture("image_load_tfe"))
        finally:
            support.rdna2spirv.Gen.mimg_status = saved
        self.assertEqual([got[1], got[3]], [1000, 1000])

    def test_without_tfe_nothing_is_written_after_the_data(self):
        lines = [ln.replace(" tfe", "") for ln in support.asm_fixture("image_load_tfe")]
        _, got = run_ps(lines)
        self.assertEqual(got[1], 1000)
        self.assertEqual(got[3], 1000)

    def test_gpu_fixtures_write_their_status(self):
        sys.path.insert(0, os.path.join(support.ROOT, "runtime", "test", "shaders"))
        from compute_fixture import compute_header
        for name in ("cs_imgload_tfe", "cs_imgload_d16_tfe"):
            with self.subTest(name=name):
                path = os.path.join(support.ROOT, "runtime", "test", "shaders", name + ".asm")
                with open(path, encoding="utf-8") as f:
                    lines = [ln.strip() for ln in f if ln.strip() and not ln.lstrip().startswith(";")]
                spv = support.translate_lines(name, lines, fssm=bytes(compute_header(path)))
                # v8 and v12 (the status of v7 and v11) get the constant 0 after their loads.
                for reg in (8, 12):
                    self.assertIn(f"OpStore %reg_v{reg} %c0", spv)


if __name__ == "__main__":
    unittest.main()
