; Quad reads and a waterfall in a wave32 pixel shader whose helper lanes (pixels of a quad outside the triangle)
; a driver may leave out of ballots and shuffles (tests/semantic/test_quad_helper_lanes.py). v1 = 16 * lane + 5,
; a value in every lane, helpers included: a helper computes what its pixel would.
v_mbcnt_lo_u32_b32 v0, -1, 0
v_lshlrev_b32 v1, 4, v0
v_add_nc_u32 v1, 5, v1
s_wqm_b32 exec_lo, exec_lo
; Quad reads: a DPP broadcast of quad lane 1, a DPP8 reverse that stays in each quad, a ds_swizzle swap of the rows.
v_mov_b32_dpp v2, v1 quad_perm:[1,1,1,1] row_mask:0xf bank_mask:0xf
v_mov_b32_dpp v3, v1 dpp8:[3,2,1,0,7,6,5,4]
ds_swizzle_b32 v4, v1 offset:swizzle(QUAD_PERM,2,3,0,1)
s_waitcnt lgkmcnt(0)
; A waterfall over the lane number: each pass reads the first lane's value and runs the lanes that hold it, until
; EXEC is empty. v5 = the value a lane was run with.
s_mov_b32 s5, exec_lo
loop:
v_readfirstlane_b32 s4, v0
v_cmp_eq_u32_e64 s6, s4, v0
s_and_saveexec_b32 s7, s6
v_mov_b32 v5, s4
s_xor_b32 exec_lo, exec_lo, s7
s_cbranch_execnz loop
s_mov_b32 exec_lo, s5
; What quad lane 1 was run with, read by every lane of its quad.
v_mov_b32_dpp v6, v5 quad_perm:[1,1,1,1] row_mask:0xf bank_mask:0xf
v_lshlrev_b32 v7, 4, v0
buffer_store_dword v2, v7, s[0:3], 0 offen
buffer_store_dword v3, v7, s[0:3], 0 offen offset:4
buffer_store_dword v4, v7, s[0:3], 0 offen offset:8
buffer_store_dword v6, v7, s[0:3], 0 offen offset:12
exp mrt0 v1, v1, v1, v1 done vm
s_endpgm
