; v0 = vertex index
; s2 = vertex buffer descriptor (low)
; s3 = vertex offset
; s4 = dst_x
; s5 = dst_y
; s6 = src_x
; s7 = src_y
vert:
	v_add_nc_u32 v0, s3, v0

	; load vertex buffer descriptor
	s_movk_i32 s3, 0x8000
	s_load_dwordx4 s[0:3], s[2:3], 0x0
	s_waitcnt lgkmcnt(0)

	; load vertex
	tbuffer_load_format_xy v[4:5], v0, s[0:3], format:63, 0 idxen
	s_waitcnt vmcnt(0)
	v_cvt_f32_i32 v4, v4
	v_cvt_f32_i32 v5, v5

	; compute dst coordinates
	v_add_f32 v0, s4, v4
	v_add_f32 v1, s5, v5
	v_mov_b32 v2, 0.0
	v_mov_b32 v3, 1.0
	exp pos0 v0, v1, v2, v3 done vm

	; compute src coordinates
	v_add_f32 v0, s6, v4
	v_add_f32 v1, s7, v5
	exp param0 v0, v1, off, off

	s_endpgm
