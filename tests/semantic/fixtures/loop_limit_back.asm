; --loop-limit, a loop closed by a branch back (tests/semantic/test_loop_limit.py). s4 counts the passes; the
; loop ends when s4 reaches s5, which comes from user data (push-constant dword 5). With s5 = 0 the end never
; comes, as in a loop over data the host has not mirrored yet (it reads as zeros). At the cap the branch back is
; not taken: the code after the loop runs and exports the count.
s_mov_b32 s4, 0
loop:
s_add_u32 s4, s4, 1
s_cmp_lg_u32 s4, s5
s_cbranch_scc1 loop
v_mov_b32 v0, s4
exp mrt0 v0, v0, v0, v0 done vm
s_endpgm
