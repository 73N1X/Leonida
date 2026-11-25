; tfe on an image_load (tests/semantic/test_mimg_tfe_status.py): the load returns its data and then one more dword,
; the fetch status, which is 0 for a resident texel. v5 and v9 are seeded with 1000 so a status the translation
; leaves alone shows. The T# is read from s[0:1]; the test's image returns one texel everywhere.
s_load_dwordx8 s[8:15], s[0:1], 0
s_waitcnt lgkmcnt(0)
v_mov_b32_e32 v2, 0
v_mov_b32_e32 v3, 0
v_mov_b32_e32 v5, 0x3e8
v_mov_b32_e32 v9, 0x3e8
image_load v[4:5], v[2:3], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm tfe
image_load v[6:9], v[2:3], s[8:15] dmask:0x7 dim:SQ_RSRC_IMG_2D unorm tfe
s_waitcnt vmcnt(0)
exp mrt0 v4, v5, v6, v9 done vm
s_endpgm
