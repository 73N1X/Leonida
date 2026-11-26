; SHADER-10 mesh tier B, shaped like the corpus' mesh entry (128 threads, two waves): the group index is the first
; lane's v5 >> 7 (v5 = group * 128 + thread), the thread index the wave id from s3[27:24] * 64 + the lane id; thread 0
; sends the allocation (3 vertices, 1 primitive) under an s_cbranch_execz skip, where a vccnz on an all-or-none mask
; guards the null-primitive path and its s_barrier; then a barrier and the exports (threads 0..2: vertex t at
; x = t). No memory write: natively the barriers are left out.
s_mov_b64 exec, -1
v_mbcnt_lo_u32_b32 v0, -1, 0
v_readfirstlane_b32 s0, v5
s_bfe_u32 s1, s3, 0x40018
v_mbcnt_hi_u32_b32 v0, -1, v0
s_lshr_b32 s10, s0, 7
v_lshl_add_u32 v0, s1, 6, v0
s_mov_b32 s32, 3
s_mov_b32 s14, 1
s_cmp_eq_u32 s10, 99
s_cselect_b64 s[2:3], exec, 0
v_cvt_f32_u32_e32 v1, v0
v_mov_b32_e32 v2, 0
v_mov_b32_e32 v3, 0.5
v_mov_b32_e32 v4, 1.0
v_mov_b32_e32 v10, 0x200400
s_mov_b64 s[0:1], exec
v_cmpx_eq_u32_e32 0, v0
s_cbranch_execz alloc_done
s_lshl_b32 s7, s14, 12
s_or_b32 s6, s7, s32
s_mov_b32 m0, s6
s_sendmsg sendmsg(MSG_GS_ALLOC_REQ)
s_andn2_b64 vcc, exec, s[2:3]
s_cbranch_vccnz alloc_done
s_barrier
v_bfrev_b32_e32 v11, 1
exp pos0 off, off, off, off done
exp prim v11, off, off, off done
alloc_done:
s_mov_b64 exec, s[0:1]
s_barrier
v_cmp_gt_u32_e32 vcc_lo, s14, v0
s_and_saveexec_b64 s[6:7], vcc
s_cbranch_execz prims_done
exp prim v10, off, off, off done
prims_done:
s_mov_b64 exec, s[6:7]
v_cmp_gt_u32_e32 vcc_lo, s32, v0
s_and_saveexec_b64 s[8:9], vcc
s_cbranch_execz verts_done
exp pos0 v1, v2, v3, v4 done
exp param0 v1, v10, v3, v4
verts_done:
s_endpgm