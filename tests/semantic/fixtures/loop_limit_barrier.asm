; --loop-limit with a barrier after the loop (tests/semantic/test_loop_limit.py), a compute shader of two waves.
; The first wave (its first thread is thread 0) leaves the loop after 3 passes; the other waits for a count that
; never comes (s7 = 0) and ends with a jump back, so at the cap it leaves the program. It must not hold the first
; wave at s_barrier: a wave that has ended no longer counts (RDNA), and the barrier rounds count it done.
v_readfirstlane_b32 s6, v0
s_cmp_eq_u32 s6, 0
s_cselect_b32 s7, 3, 0
s_mov_b32 s4, 0
loop:
s_add_u32 s4, s4, 1
s_cmp_eq_u32 s4, s7
s_cbranch_scc1 done
s_branch loop
done:
s_barrier
v_mov_b32 v2, s4
v_lshlrev_b32 v3, 2, v0
buffer_store_dword v2, v3, s[0:3], 0 offen
s_endpgm
