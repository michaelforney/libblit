; s2 = red
; s3 = green
; s4 = blue
; s5 = alpha
fill:
	v_cvt_pkrtz_f16_f32 v0, s2, s3
	v_cvt_pkrtz_f16_f32 v1, s4, s5
	exp mrt0 v0, off, v1, off done compr vm
	s_endpgm
