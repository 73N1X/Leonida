; A pixel's discard as the console compiles it (tests/semantic/test_quad_helper_lanes.py): the killed lanes leave
; the live mask s2, a wave with no live lane left exits, and whole-quad mode turns the killed lanes of a quad with a
; live lane back on, as helpers for the survivors' quad reads. Quad lane 1 of every quad is killed here; each lane
; then reads quad lane 1's value v1 = 16 * lane + 5, and only the live lanes export it.
v_mbcnt_lo_u32_b32 v0, -1, 0
v_lshlrev_b32 v1, 4, v0
v_add_nc_u32 v1, 5, v1
s_mov_b32 s2, exec_lo
v_and_b32 v8, 3, v0
v_cmp_eq_u32_e32 vcc_lo, 1, v8
s_andn2_b32 s2, s2, vcc_lo
s_cbranch_scc0 dead
s_wqm_b32 exec_lo, s2
v_mov_b32_dpp v2, v1 quad_perm:[1,1,1,1] row_mask:0xf bank_mask:0xf
s_mov_b32 exec_lo, s2
exp mrt0 v2, v2, v2, v2 done vm
s_endpgm
dead:
s_mov_b32 exec_lo, 0
exp null off, off, off, off done vm
s_endpgm
