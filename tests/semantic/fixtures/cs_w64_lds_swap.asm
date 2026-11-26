; SHADER-10 compute tier B: a one-wave group (64 threads) shares LDS without a barrier, as the console compiler emits
; it: lane t writes 100 + t to LDS[t], then reads LDS[t ^ 32], the other half's word. The wave runs both in
; lockstep; in two 32-lane subgroups the read needs a workgroup barrier in front (wave64_halves `sync`). Lane t stores
; 100 + (t ^ 32) at 4 * t.
v_mbcnt_lo_u32_b32 v1, -1, 0
v_mbcnt_hi_u32_b32 v1, -1, v1
s_mov_b64 s[6:7], exec
v_lshlrev_b32_e32 v2, 2, v1
v_add_nc_u32_e32 v5, 100, v1
ds_write_b32 v2, v5
v_xor_b32_e32 v3, 0x80, v2
ds_read_b32 v4, v3
s_waitcnt lgkmcnt(0)
v_lshlrev_b32_e32 v21, 2, v0
buffer_store_dword v4, v21, s[0:3], 0 offen
s_endpgm