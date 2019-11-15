; s[0:3] = texture descriptor
; s4     = M# Memory descriptor (implicit, after USER_SGPR)
copy:
	; setup memory descriptor (M#) register (required for interpolation)
	s_mov_b32 m0, s4

	; switch to whole quad mode
	s_mov_b32 s4, exec_lo
	s_wqm_b32 exec_lo, exec_lo

	; interpolate texture coordinates
	v_interp_p1_f32_e32 v2, v0, attr0.x
	v_interp_p1_f32_e32 v3, v0, attr0.y
	v_interp_p2_f32_e32 v2, v1, attr0.x
	v_interp_p2_f32_e32 v3, v1, attr0.y

	; switch back to exact mode
	s_mov_b32 exec_lo, s4

	; compute sampler descriptor in s[0:3]
	s_mov_b32 s4, 0x8036 ; clamp x = ClampBorder, clamp y = ClampBorder, force unnormalized
	s_mov_b32 s5, 0
	s_brev_b32 s6, 36
	s_mov_b32 s7, 0

	; sample image
	; pass s[0:7], even though we set R128 and really mean s[0:3],
	; since the LLVM AMDGCN assembly parser requires it
	image_sample v[0:3], v[2:3], s[0:7], s[4:7] dmask:0xf dim:SQ_RSRC_IMG_2D r128
	s_waitcnt vmcnt(0)

	; export color
	v_cvt_pkrtz_f16_f32_e32 v0, v0, v1
	v_cvt_pkrtz_f16_f32_e32 v1, v2, v3
	exp mrt0 v0, off, v1, off done compr vm

	s_endpgm
