; A wave64 compute program printed in wave32 syntax, as llvm-mc's gfx10 default prints the corpus: a VOP3 lane-mask
; destination or source is one SGPR (s4, s8, s12) where the wave64 hardware reads and writes the pair (s[4:5]).
; s5, s9 and s13 hold data first, which the 64-bit writes must replace. Lane l stores three words at 12 * l:
;   +0: 1 where l < 40      (v_cmp_gt_u32_e64 s4, 40, l; then s[4:5] as a 64-bit SALU operand)
;   +4: 9 where l is odd    (v_cmp_e64 s8; v_cndmask_b32_e64 with the mask printed as s8)
;   +8: carry of l + 0xffffffd0 (l >= 48), from v_add_co_u32_e64's carry printed as s12, read as the pair
s_mov_b64 s[6:7], exec
v_mbcnt_lo_u32_b32 v1, -1, 0
v_mbcnt_hi_u32_b32 v1, -1, v1
s_mov_b32 s5, 0
s_mov_b32 s9, 0
s_mov_b32 s13, 0
v_cmp_gt_u32_e64 s4, 40, v1
s_and_b64 vcc, exec, s[4:5]
v_cndmask_b32_e64 v2, 0, 1, vcc
v_and_b32_e32 v3, 1, v1
v_cmp_eq_u32_e64 s8, 1, v3
v_cndmask_b32_e64 v4, 0, 9, s8
v_add_co_u32_e64 v5, s12, 0xffffffd0, v1
s_and_b64 vcc, exec, s[12:13]
v_cndmask_b32_e64 v6, 0, 1, vcc
v_mul_u32_u24_e32 v7, 12, v1
buffer_store_dword v2, v7, s[0:3], 0 offen
buffer_store_dword v4, v7, s[0:3], 0 offen offset:4
buffer_store_dword v6, v7, s[0:3], 0 offen offset:8
s_endpgm
