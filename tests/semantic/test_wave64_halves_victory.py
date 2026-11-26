"""SHADER-10: forms tools/wave64_halves.py proves exact in halves, after VIctory's exact idioms (lower.rs lane_uniform,
the scalarised loops of own_bits.rs) and the console's own code shapes. All stages; the cases run as vertex or pixel
shaders, the classification on the translator's result, the vertex ones also on the strict wave interpreter as one
wave of 64 lanes and as two waves of 32 (test_wave64_halves.Execution.run_both), each with near misses that must stay
tier C. No game data: hand-written listings.

  * Uniform data (VIctory lane_uniform): a load from an address that is the same in every lane gives every lane one
    value, unless a store may have run before it.
  * All-or-none masks: EXEC or 0 picked by a clean SCC (s_cselect_b64), and bitwise mixes of such masks; a
    v_cndmask by one of them between uniform values is uniform, and an SCC branch on one is no lane vote.
  * A compare of uniform values under a narrowed EXEC, inside the s_cbranch_execz region of that EXEC (every half
    that runs it has an active lane) or under an EXEC that holds every launch lane (the entry EXEC, its WQM).
  * The waterfall that keeps the lanes still to do in an SGPR pair (v_cmpx_eq ... s_andn2_wrexec_b64 m, m), and loads
    in a waterfall body through the descriptor the waterfall scalar selects.
  * Wave64 compares printed in wave32 syntax (v_cmp_*_e64 s8, ...): the pair s[8:9] is a lane mask
    (rdna2spirv.widen_wave64_masks).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402,F401  (path set above)
import test_wave64_halves as h  # noqa: E402

UNIFORM, meta_of, ps, size, vs = h.UNIFORM, h.meta_of, h.ps, h.size, h.vs


def at(lines):
    """`lines` with `name:` labels and branches to them (s_branch name) as the dword offsets the listing's fake
    encodings (test_wave64_halves.size) give: target = PC of the next instruction + 4 * offset."""
    pos, pc = {}, 0
    for ln in lines:
        if ln.endswith(":"):
            pos[ln[:-1]] = pc
        else:
            pc += size(ln)
    out, pc = [], 0
    for ln in lines:
        if ln.endswith(":"):
            continue
        pc += size(ln)
        w = ln.split()
        if w[0].startswith(("s_branch", "s_cbranch")) and len(w) == 2 and w[1] in pos:
            ln = f"{w[0]} {(pos[w[1]] - pc) // 4}"
        out.append(ln)
    return out


class VictoryIdioms(unittest.TestCase):
    run_both = h.Execution.run_both

    def tier(self, name, lines, tier, reasons=None, idioms=None):
        (t, r, i), _ = meta_of(name, lines)
        self.assertEqual(t, tier, (name, r))
        if reasons is not None or tier != "C":
            self.assertEqual(sorted(r), sorted(reasons or ()), name)
        if idioms is not None:
            self.assertEqual(sorted(i), sorted(idioms), name)

    def same(self, name, body, tier, want, idioms=None):
        """Tier `tier`, and the vertex shader's words equal at 64 and at 2 x 32 for every UNIFORM push set."""
        for push in UNIFORM:
            t, whole, halves = self.run_both(name, vs(body), push)
            self.assertEqual(t, tier, name)
            self.assertEqual(whole, halves, (name, push))
            self.assertEqual([w[:2] for w in whole], [want(push[4], k) for k in range(64)], (name, push))
        if idioms is not None:
            (_, _, got), _ = meta_of(name, vs(body))
            self.assertEqual(sorted(got), sorted(idioms))

    def differs(self, name, body, reason, push=UNIFORM[0]):
        t, whole, halves = self.run_both(name, vs(body), push)
        (_, reasons, _), _ = meta_of(name, vs(body))
        self.assertEqual(t, "C", name)
        self.assertIn(reason, reasons, name)
        self.assertNotEqual(whole, halves, name)

    # -- uniform data
    def test_b_load_from_a_uniform_address(self):
        load = ["v_mov_b32_e32 v1, 0", "buffer_load_dword v2, v1, s[0:3], 0 offen", "s_waitcnt vmcnt(0)",
                "v_readfirstlane_b32 s20, v2", "v_add_nc_u32_e32 v6, s20, v0"]
        self.tier("ps_b_uniform_load", ps(load), "B", idioms=["readfirstlane-uniform"])
        # a store before it: the other half's store may land between the two halves' loads
        self.tier("ps_c_load_after_store", ps(["buffer_store_dword v0, v0, s[0:3], 0 offen"] + load), "C")
        # a per-lane address: lanes load different words
        self.tier("ps_c_load_per_lane", ps(["v_lshlrev_b32_e32 v1, 2, v0", "buffer_load_dword v2, v1, s[0:3], 0 offen",
                                            "s_waitcnt vmcnt(0)", "v_readfirstlane_b32 s20, v2",
                                            "v_add_nc_u32_e32 v6, s20, v0"]), "C")

    # -- all-or-none masks
    def test_b_select_by_exec_or_zero(self):
        body = ["s_cmp_lt_u32 s4, 5", "s_cselect_b64 s[0:1], exec, 0", "v_cndmask_b32_e64 v1, 3, 9, s0",
                "v_readfirstlane_b32 s20, v1", "v_add_nc_u32_e32 v6, s20, v0", "v_mov_b32_e32 v7, s20"]
        self.same("vs_b_cselect_exec", body, "B", lambda s4, k: (9 + k, 9) if s4 < 5 else (3 + k, 3),
                  ["readfirstlane-uniform"])

    def test_a_scc_branch_on_an_all_or_none_mask(self):
        body = at(["s_cmp_lt_u32 s4, 5", "s_cselect_b64 s[2:3], exec, 0", "s_and_b64 s[0:1], s[2:3], exec",
                   "s_cbranch_scc0 no", "v_mov_b32_e32 v6, 7", "s_mov_b32 s20, 1", "s_branch join", "no:",
                   "v_mov_b32_e32 v6, 5", "s_mov_b32 s20, 2", "join:", "v_add_nc_u32_e32 v7, s20, v0"])
        self.same("vs_a_scc_allnone", body, "A", lambda s4, k: (7, 1 + k) if s4 < 5 else (5, 2 + k))

    # -- uniform compare under a narrowed EXEC
    UNDER = ["v_mov_b32_e32 v6, 3", "v_mov_b32_e32 v7, 0", "v_cmp_gt_u32_e32 vcc_lo, 40, v0",
             "s_and_saveexec_b64 s[12:13], vcc", "s_cbranch_execz join", "v_cmp_lt_u32_e64 s14, s4, 5",
             "s_and_b64 vcc, exec, s[14:15]", "s_cbranch_vccnz yes", "v_mov_b32_e32 v6, 5", "s_mov_b32 s20, 2",
             "s_branch done", "yes:", "v_mov_b32_e32 v6, 7", "s_mov_b32 s20, 1", "done:", "v_mov_b32_e32 v7, s20",
             "join:", "s_or_b64 exec, exec, s[12:13]"]

    def test_a_uniform_compare_inside_an_execz_region(self):
        self.same("vs_a_uniform_under_exec", at(self.UNDER), "A",
                  lambda s4, k: ((7, 1) if s4 < 5 else (5, 2)) if k < 40 else (3, 0))

    def test_c_uniform_compare_without_the_execz(self):
        # no skip, and only lanes below 16 on: the upper half has no active lane, votes no and keeps s20 = 2, which
        # its lanes read once EXEC is restored; the wave voted yes (s4 < 5) and set s20 = 1
        body = [ln.replace("40, v0", "16, v0") for ln in self.UNDER if ln not in ("s_cbranch_execz join", "join:")]
        body = [ln if ln != "s_or_b64 exec, exec, s[12:13]" else "s_mov_b64 exec, s[10:11]" for ln in body] + \
            ["v_mov_b32_e32 v7, s20"]
        self.differs("vs_c_uniform_no_execz", at(body), "unsafe-vote-branch", push=[p for p in UNIFORM if p[4] < 5][0])

    def test_a_uniform_compare_under_the_launch_exec_after_wqm(self):
        body = at(["s_wqm_b64 exec, exec", "v_cmp_lt_u32_e64 s14, s4, 5", "s_and_b64 vcc, exec, s[14:15]",
                   "s_cbranch_vccnz yes", "v_mov_b32_e32 v6, 5", "s_mov_b32 s20, 2", "s_branch done", "yes:",
                   "v_mov_b32_e32 v6, 7", "s_mov_b32 s20, 1", "done:", "v_add_nc_u32_e32 v7, s20, v0"])
        self.tier("ps_a_uniform_wqm", ps(body), "A")

    # -- waterfall forms
    def test_b_waterfall_with_andn2_wrexec(self):
        body = at(["v_and_b32_e32 v4, 7, v0", "v_mov_b32_e32 v6, 0", "s_mov_b64 s[14:15], exec", "loop:",
                   "v_readfirstlane_b32 s20, v4", "v_cmpx_eq_u32_e32 s20, v4", "s_mul_i32 s21, s20, 17",
                   "v_add_nc_u32_e32 v6, s21, v0", "s_andn2_wrexec_b64 s[14:15], s[14:15]", "s_cbranch_execnz loop",
                   "s_mov_b64 exec, s[10:11]"])
        self.same("vs_b_wf_wrexec", body, "B", lambda s4, k: (17 * (k & 7) + k, 0), ["waterfall"])
        # the remaining-lanes mask read outside the loop is a stale scalar
        self.tier("vs_c_wf_wrexec_read", vs(body + ["s_bcnt1_i32_b64 s22, s[14:15]", "v_mov_b32_e32 v7, s22"]), "C")

    def test_b_load_through_the_waterfall_descriptor(self):
        head = ["v_and_b32_e32 v4, 3, v0", "v_lshlrev_b32_e32 v4, 4, v4", "v_mov_b32_e32 v6, 0",
                "s_mov_b64 s[14:15], exec", "loop:", "v_readfirstlane_b32 s20, v4", "v_cmpx_eq_u32_e32 s20, v4",
                "s_load_dwordx4 s[24:27], s[2:3], s20", "s_waitcnt lgkmcnt(0)"]
        load = ["buffer_load_dword v6, v0, s[24:27], 0 offen"]
        tail = ["s_andn2_wrexec_b64 s[14:15], s[14:15]", "s_cbranch_execnz loop", "s_mov_b64 exec, s[10:11]"]
        use = ["s_waitcnt vmcnt(0)", "v_add_nc_u32_e32 v6, 1, v6"]
        self.tier("ps_b_wf_desc_load", ps(at(head + load + tail + use)), "B", idioms=["waterfall"])
        # the same load after the loop: the descriptor is then the last pass's, not each lane's
        self.tier("ps_c_wf_desc_outside", ps(at(head + tail + load + use)), "C")

    # -- wave32-syntax mask pairs
    def test_a_compare_printed_as_one_sgpr(self):
        body = at(["v_cmp_lt_u32_e64 s8, s4, 5", "s_and_b64 vcc, exec, s[8:9]", "s_cbranch_vccnz yes",
                   "v_mov_b32_e32 v6, 5", "s_mov_b32 s20, 2", "s_branch done", "yes:", "v_mov_b32_e32 v6, 7",
                   "s_mov_b32 s20, 1", "done:", "v_add_nc_u32_e32 v7, s20, v0"])
        self.same("vs_a_cmp_one_sgpr", body, "A", lambda s4, k: (7, 1 + k) if s4 < 5 else (5, 2 + k))


if __name__ == "__main__":
    unittest.main()
