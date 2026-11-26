; SHADER-10 compute tier A: a wave64 compute program (64 threads) whose cross-lane reads stay inside 32-lane halves.
; Natively it runs one half per 32-lane subgroup with the thread index relabelled from the subgroup. Lane t stores
; ((t - 1 if t % 16 else 0) << 8) + (t ^ 16) at 4 * t: a DPP row shift right by 1 (bound_ctrl: 0 off the row's start)
; and a ds_swizzle swap of 16-lane groups.
v_mbcnt_lo_u32_b32 v1, -1, 0
v_mbcnt_hi_u32_b32 v1, -1, v1
s_mov_b64 s[6:7], exec
v_mov_b32_dpp v2, v1 row_shr:1 row_mask:0xf bank_mask:0xf bound_ctrl:0
ds_swizzle_b32 v3, v1 offset:swizzle(SWAP,16)
s_waitcnt lgkmcnt(0)
v_lshl_add_u32 v4, v2, 8, v3
v_lshlrev_b32_e32 v21, 2, v0
buffer_store_dword v4, v21, s[0:3], 0 offen
s_endpgm