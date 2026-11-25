; --loop-limit, a loop that leaves from the middle and ends with a jump back (tests/semantic/test_loop_limit.py).
; s4 counts the passes; the exit is taken when s4 reaches s5 (push-constant dword 5). With s5 = 0 it never is.
; At the cap the jump back has no other way: the program ends there, and a pixel is discarded.
s_mov_b32 s4, 0
loop:
s_add_u32 s4, s4, 1
s_cmp_eq_u32 s4, s5
s_cbranch_scc1 done
s_branch loop
done:
v_mov_b32 v0, s4
exp mrt0 v0, v0, v0, v0 done vm
s_endpgm
