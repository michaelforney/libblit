/*
Terminology:
IB : Indirect Buffer
CS : Command Stream?

USER_SGPR : Number of SGPRs passed to shader

TODO:
Double check BO alignment and flags
*/
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <pixman.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <drm_fourcc.h>

#include <blt.h>
#include "../priv.h"
#include "amd_family.h"
#include "sid.h"

struct bo {
	amdgpu_bo_handle handle;
	amdgpu_va_handle va;
	size_t size;
	uint64_t addr;
};

struct shader_info {
	uint32_t rsrc1;
	uint32_t rsrc2;
};

struct shader {
	struct bo bo;
	const struct shader_info *info;
};

struct cmdbuf {
	struct bo bo;
	uint32_t *buf;
	unsigned len, cap;
};

struct vertbuf {
	struct bo bo;
	uint32_t *buf;
	unsigned len, cap, pos;
};

struct draw {
	struct cmdbuf cmd;
	struct vertbuf vert;
};

struct context {
	struct blt_context base;
	int fd;
	amdgpu_device_handle dev;
	amdgpu_context_handle cs;
	struct amdgpu_gpu_info info;
	struct {
		enum radeon_family family;
		enum chip_class class;
	} chip;
	struct {
		struct bo vert, fill, copy;
	} shader;
	
	struct cmdbuf init;
};

struct image {
	struct blt_image base;
	struct bo bo;
	uint32_t stride;
	struct draw *draw;
	uint32_t desc[4];
	int swizzle;
};

static const struct shader_info vert_info = {
	.rsrc1 = S_00B128_VGPRS(1) | S_00B028_SGPRS(0),
	/* s2 = buffer descriptor, s3 = vertex offset, s4 = dst_x, s5 = dst_y, s6 = src_x, s7 = src_y */
	.rsrc2 = S_00B12C_USER_SGPR(8),
};

static const uint32_t vert_code[] = {
#include "vert-gfx10.inc"
	0xbf9f0000, 0xbf9f0000, 0xbf9f0000, 0xbf9f0000, 0xbf9f0000,
};

static const struct shader_info fill_info = {
	.rsrc1 = S_00B028_VGPRS(5) | S_00B028_SGPRS(0),
	/* s2 = red, s3 = green, s4 = blue, s5 = alpha */
	.rsrc2 = S_00B02C_USER_SGPR(6),
};

static const uint32_t fill_code[] = {
#include "fill-gfx10.inc"
	0xbf9f0000, 0xbf9f0000, 0xbf9f0000, 0xbf9f0000, 0xbf9f0000,
};

static const struct shader_info copy_info = {
	.rsrc1 = S_00B028_VGPRS(0) | S_00B028_SGPRS(0),
	/* s2 = texture descriptor */
	.rsrc2 = S_00B02C_USER_SGPR(4),
};

static const uint32_t copy_code[] = {
#include "copy-gfx10.inc"
	0xbf9f0000, 0xbf9f0000, 0xbf9f0000, 0xbf9f0000, 0xbf9f0000,
};

#define ALIGN_UP(x, a) (((x) + (a) - 1) & (-(a)))
#define ARG16(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, ...) a16
#define NARG(...) ARG16(__VA_ARGS__, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define SET_CONTEXT_REG_IDX(r, i, ...) \
	PKT3(PKT3_SET_CONTEXT_REG, NARG(__VA_ARGS__), 0), \
	((r) - SI_CONTEXT_REG_OFFSET) >> 2 | (i) << 28, \
	__VA_ARGS__
#define SET_CONTEXT_REG(r, ...) SET_CONTEXT_REG_IDX(r, 0, __VA_ARGS__)
#define SET_SH_REG(r, ...) \
	PKT3(PKT3_SET_SH_REG, NARG(__VA_ARGS__), 0), \
	(r - SI_SH_REG_OFFSET) >> 2, \
	__VA_ARGS__
#define SET_UCONFIG_REG(r, ...) \
	PKT3(PKT3_SET_UCONFIG_REG, NARG(__VA_ARGS__), 0), \
	(r - CIK_UCONFIG_REG_OFFSET) >> 2, \
	__VA_ARGS__

static inline uint32_t
ftou(float f)
{
	union {float f; uint32_t u;} x = {f};
	return x.u;
}

static const uint32_t pipeline[] = {
	SET_CONTEXT_REG(R_028800_DB_DEPTH_CONTROL, 0),
	SET_CONTEXT_REG(R_02842C_DB_STENCIL_CONTROL, 0),
	SET_CONTEXT_REG(R_028000_DB_RENDER_CONTROL, 0),
	SET_CONTEXT_REG(R_02800C_DB_RENDER_OVERRIDE,
		S_02800C_FORCE_HIS_ENABLE0(V_02800C_FORCE_DISABLE) |
		S_02800C_FORCE_HIS_ENABLE1(V_02800C_FORCE_DISABLE)
	),
	SET_CONTEXT_REG(R_028010_DB_RENDER_OVERRIDE2, 0),
	SET_CONTEXT_REG(R_028780_CB_BLEND0_CONTROL, 0, 0, 0, 0, 0, 0, 0, 0),
	SET_CONTEXT_REG(R_028808_CB_COLOR_CONTROL,
		S_028808_MODE(V_028808_CB_NORMAL) |
		S_028808_ROP3(V_028808_ROP3_COPY)
	),
	SET_CONTEXT_REG(R_028B70_DB_ALPHA_TO_MASK,
		S_028B70_ALPHA_TO_MASK_OFFSET0(3) |
		S_028B70_ALPHA_TO_MASK_OFFSET1(1) |
		S_028B70_ALPHA_TO_MASK_OFFSET2(0) |
		S_028B70_ALPHA_TO_MASK_OFFSET3(2) |
		S_028B70_OFFSET_ROUND(1)
	),
	SET_CONTEXT_REG(R_028760_SX_MRT0_BLEND_OPT,
		S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED),
		S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED),
		S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED),
		S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED),
		S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED),
		S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED),
		S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED),
		S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED)
	),
	SET_CONTEXT_REG(R_028714_SPI_SHADER_COL_FORMAT, S_028714_COL0_EXPORT_FORMAT(V_028714_SPI_SHADER_FP16_ABGR)),
	SET_CONTEXT_REG(R_028238_CB_TARGET_MASK, S_028238_TARGET0_ENABLE(0xf)),
	SET_CONTEXT_REG(R_02823C_CB_SHADER_MASK, S_02823C_OUTPUT0_ENABLE(0xf)),
	SET_CONTEXT_REG(R_028810_PA_CL_CLIP_CNTL,
		S_028810_DX_CLIP_SPACE_DEF(1) | // vulkan uses DX conventions.
		S_028810_ZCLIP_NEAR_DISABLE(0) |
		S_028810_ZCLIP_FAR_DISABLE(0) |
		S_028810_DX_RASTERIZATION_KILL(0) |
		S_028810_DX_LINEAR_ATTR_CLIP_ENA(1)
	),
	SET_CONTEXT_REG(R_0286D4_SPI_INTERP_CONTROL_0,
		S_0286D4_FLAT_SHADE_ENA(1) |
		S_0286D4_PNT_SPRITE_ENA(1) |
		S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S) |
		S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T) |
		S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0) |
		S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1) |
		S_0286D4_PNT_SPRITE_TOP_1(0)
	),
	SET_CONTEXT_REG(R_028BE4_PA_SU_VTX_CNTL,
		S_028BE4_PIX_CENTER(1) | // TODO verify
		S_028BE4_ROUND_MODE(V_028BE4_X_ROUND_TO_EVEN) |
		S_028BE4_QUANT_MODE(V_028BE4_X_16_8_FIXED_POINT_1_256TH)
	),
	SET_CONTEXT_REG(R_028814_PA_SU_SC_MODE_CNTL,
		S_028814_FACE(0) |
		S_028814_CULL_FRONT(0) |
		S_028814_CULL_BACK(0) |
		S_028814_POLY_MODE(V_028814_X_DISABLE_POLY_MODE) |
		S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_TRIANGLES) |
		S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_TRIANGLES) |
		S_028814_POLY_OFFSET_FRONT_ENABLE(0) |
		S_028814_POLY_OFFSET_BACK_ENABLE(0) |
		S_028814_POLY_OFFSET_PARA_ENABLE(0)
	),
	SET_CONTEXT_REG(R_028C4C_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,
		S_028C4C_NULL_SQUAD_AA_MASK_ENABLE(1)
	),
	SET_CONTEXT_REG(R_028C38_PA_SC_AA_MASK_X0Y0_X1Y0,
		S_028C38_AA_MASK_X0Y0(0xffff) | S_028C38_AA_MASK_X1Y0(0xffff),
		S_028C3C_AA_MASK_X0Y1(0xffff) | S_028C3C_AA_MASK_X1Y1(0xffff)
	),
	SET_CONTEXT_REG(R_028804_DB_EQAA,
		S_028804_HIGH_QUALITY_INTERSECTIONS(1) |
		S_028804_INCOHERENT_EQAA_READS(1) |
		S_028804_INTERPOLATE_COMP_Z(1) |
		S_028804_STATIC_ANCHOR_ASSOCIATIONS(1)
	),
	SET_CONTEXT_REG(R_028A4C_PA_SC_MODE_CNTL_1,
		S_028A4C_WALK_ALIGN8_PRIM_FITS_ST(1) |
		S_028A4C_WALK_FENCE_ENABLE(1) |
		S_028A4C_WALK_FENCE_SIZE(2) | // XXX
		S_028A4C_SUPERTILE_WALK_ORDER_ENABLE(1) |
		S_028A4C_TILE_WALK_ORDER_ENABLE(1) |
		S_028A4C_MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE(1) |
		S_028A4C_FORCE_EOV_CNTDWN_ENABLE(1) |
		S_028A4C_FORCE_EOV_REZ_ENABLE(1) |
		S_028A4C_OUT_OF_ORDER_WATER_MARK(7)
	),
	SET_CONTEXT_REG(R_02882C_PA_SU_PRIM_FILTER_CNTL,
		S_02882C_XMAX_RIGHT_EXCLUSION(1) |
		S_02882C_YMAX_BOTTOM_EXCLUSION(1)
	),
	SET_CONTEXT_REG(R_028A84_VGT_PRIMITIVEID_EN, 0),
	SET_CONTEXT_REG(R_028A40_VGT_GS_MODE, 0),
	SET_CONTEXT_REG(R_0286C4_SPI_VS_OUT_CONFIG, S_0286C4_VS_EXPORT_COUNT(0) | S_0286C4_NO_PC_EXPORT(0)),
	SET_CONTEXT_REG(R_02870C_SPI_SHADER_POS_FORMAT,
		S_02870C_POS0_EXPORT_FORMAT(V_02870C_SPI_SHADER_4COMP) |
		S_02870C_POS1_EXPORT_FORMAT(V_02870C_SPI_SHADER_NONE) |
		S_02870C_POS2_EXPORT_FORMAT(V_02870C_SPI_SHADER_NONE) |
		S_02870C_POS3_EXPORT_FORMAT(V_02870C_SPI_SHADER_NONE)
	),
	SET_CONTEXT_REG(R_028818_PA_CL_VTE_CNTL,
		S_028818_VTX_W0_FMT(1) |
		S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
		S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) |
		S_028818_VPORT_Z_SCALE_ENA(1) | S_028818_VPORT_Z_OFFSET_ENA(1)
	),
	SET_CONTEXT_REG(R_02881C_PA_CL_VS_OUT_CNTL,
		S_02881C_USE_VTX_POINT_SIZE(0) |
		S_02881C_USE_VTX_RENDER_TARGET_INDX(0) |
		S_02881C_USE_VTX_VIEWPORT_INDX(0) |
		S_02881C_VS_OUT_MISC_VEC_ENA(0) |
		S_02881C_VS_OUT_MISC_SIDE_BUS_ENA(0) |
		S_02881C_VS_OUT_CCDIST0_VEC_ENA(0) |
		S_02881C_VS_OUT_CCDIST1_VEC_ENA(0) |
		0 /* cull_dist_mask */ << 8 | 0 /* clip_dist_mask */
	),
	SET_CONTEXT_REG(R_02880C_DB_SHADER_CONTROL,
		S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z) |
		S_02880C_DUAL_QUAD_DISABLE(1)
	),
	SET_CONTEXT_REG(R_0286CC_SPI_PS_INPUT_ENA, S_0286CC_LINEAR_CENTER_ENA(1)),
	SET_CONTEXT_REG(R_0286D0_SPI_PS_INPUT_ADDR, S_0286D0_LINEAR_CENTER_ENA(1)),
	SET_CONTEXT_REG(R_0286D8_SPI_PS_IN_CONTROL, S_0286D8_NUM_INTERP(1)),
	SET_CONTEXT_REG(R_0286E0_SPI_BARYC_CNTL, S_0286E0_FRONT_FACE_ALL_BITS(1)),
	SET_CONTEXT_REG(R_028710_SPI_SHADER_Z_FORMAT, S_028710_Z_EXPORT_FORMAT(V_028710_SPI_SHADER_ZERO)),
	SET_CONTEXT_REG(R_028644_SPI_PS_INPUT_CNTL_0, 0),
	SET_UCONFIG_REG(R_03096C_GE_CNTL,
		S_03096C_PRIM_GRP_SIZE(128) |
		S_03096C_VERT_GRP_SIZE(0) |
		S_03096C_BREAK_WAVE_AT_EOI(0)
	),
	SET_CONTEXT_REG(R_0286E8_SPI_TMPRING_SIZE,
		S_0286E8_WAVES(1152) |  // XXX
		S_0286E8_WAVESIZE(0 >> 10)
	),
	SET_CONTEXT_REG(R_028B54_VGT_SHADER_STAGES_EN, S_028B54_MAX_PRIMGRP_IN_WAVE(2)),
	SET_CONTEXT_REG(R_028A6C_VGT_GS_OUT_PRIM_TYPE, S_028A6C_OUTPRIM_TYPE(V_028A6C_OUTPRIM_TYPE_TRISTRIP)),
	SET_CONTEXT_REG(R_02820C_PA_SC_CLIPRECT_RULE, S_02820C_CLIP_RULE(0xffff)),
};

static void
image_destroy(struct blt_context *ctx, struct blt_image *img)
{
}

static const struct blt_image_impl image_impl = {
	.destroy = image_destroy,
};

/*
Flags used by radv:
- RADEON_FLAG_READ_ONLY : AMDGPU_VM_PAGE_WRITEABLE in amdgpu_bo_va_op
- RADEON_FLAG_32BIT : AMDGPU_VA_RANGE_32_BIT in amdgpu_va_range_alloc
- RADEON_FLAG_CPU_ACCESS : AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED in amdgpu_bo_alloc
- RADEON_FLAG_NO_CPU_ACCESS : AMDGPU_GEM_CREATE_NO_CPU_ACCESS in amdgpu_bo_alloc
- !RADEON_FLAG_IMPLICIT_SYNC : AMDGPU_GEM_CREATE_EXPLICIT_SYNC in amdgpu_bo_alloc
- RADEON_FLAG_NO_INTERPROCESS_SHARING : AMDGPU_GEM_CREATE_VM_ALWAYS_VALID in amdgpu_bo_alloc
*/
static int bo_alloc(struct context *ctx, struct bo *bo, uint64_t size, unsigned align, uint32_t domain, uint64_t flags, uint32_t vaflags)
{
	int ret;

	bo->size = size;
	/* XXX: when to pass AMDGPU_VA_RANGE_HIGH and AMDGPU_VA_RANGE_32_BIT? */
	ret = amdgpu_va_range_alloc(ctx->dev, amdgpu_gpu_va_range_general, size, align, 0, &bo->addr, &bo->va, AMDGPU_VA_RANGE_HIGH|vaflags);
	if (ret < 0)
		goto error0;
	ret = amdgpu_bo_alloc(ctx->dev, &(struct amdgpu_bo_alloc_request){
		.alloc_size = size,
		.phys_alignment = align,
		.preferred_heap = domain,
		.flags = flags,
	}, &bo->handle);
	if (ret < 0)
		goto error1;
	/* XXX: AMDGPU_VM_PAGE_* ? */
	ret = amdgpu_bo_va_op(bo->handle, 0, bo->size, bo->addr, AMDGPU_VM_PAGE_READABLE|AMDGPU_VM_PAGE_WRITEABLE|AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP);
	if (ret < 0)
		goto error2;
	return 0;

error2:
	amdgpu_bo_free(bo->handle);
error1:
	amdgpu_va_range_free(bo->va);
error0:
	errno = -ret;
	return -1;
}

static int
bo_free(struct bo *bo)
{
	amdgpu_bo_va_op(bo->handle, 0, bo->size, bo->addr, 0, AMDGPU_VA_OP_UNMAP);
	amdgpu_bo_free(bo->handle);
	amdgpu_va_range_free(bo->va);
	return 0;
}

static struct draw *
new_draw(struct context *ctx)
{
	struct draw *drw;
	void *map;
	int ret;

	drw = malloc(sizeof(*drw));
	if (!drw)
		goto error0;

	ret = bo_alloc(ctx, &drw->cmd.bo, 80 * 1024, 0x100, AMDGPU_GEM_DOMAIN_GTT, AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, 0);
	if (ret < 0)
		goto error1;
	ret = amdgpu_bo_cpu_map(drw->cmd.bo.handle, &map);
	if (ret < 0)
		goto error2;
	drw->cmd.buf = map;
	drw->cmd.len = 0;
	drw->cmd.cap = drw->cmd.bo.size / 4;

	ret = bo_alloc(ctx, &drw->vert.bo, 80 * 1024, 0x400, AMDGPU_GEM_DOMAIN_GTT, AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, AMDGPU_VA_RANGE_32_BIT);
	if (ret < 0)
		goto error3;
	ret = amdgpu_bo_cpu_map(drw->vert.bo.handle, &map);
	if (ret < 0)
		goto error4;
	drw->vert.buf = map;
	drw->vert.len = 0;
	drw->vert.cap = drw->vert.bo.size / sizeof(drw->vert.buf[0]);

	drw->vert.buf[drw->vert.len++] = drw->vert.bo.addr + 16;
	drw->vert.buf[drw->vert.len++] =
		S_008F04_BASE_ADDRESS_HI((drw->vert.bo.addr + 16) >> 32) |
		S_008F04_STRIDE(8);
	drw->vert.buf[drw->vert.len++] = drw->vert.bo.size - 16;
	drw->vert.buf[drw->vert.len++] =
		S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
		S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
		S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
		S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
		S_008F0C_FORMAT(V_008F0C_IMG_FORMAT_32_UINT) |
		S_008F0C_OOB_SELECT(1) |
		S_008F0C_RESOURCE_LEVEL(1);
	drw->vert.pos = drw->vert.len;

	return drw;

error4:
	bo_free(&drw->vert.bo);
error3:
	amdgpu_bo_cpu_unmap(drw->cmd.bo.handle);
error2:
	bo_free(&drw->cmd.bo);
error1:
	free(drw);
error0:
	return NULL;
}

static struct blt_image *
new_image(struct blt_context *ctx_base, int w, int h, uint32_t format, int flags)
{
	struct context *ctx = (void *)ctx_base;
	struct image *img;
	size_t size;
	int ret;
	struct amdgpu_bo_metadata metadata = {0};

	img = malloc(sizeof(*img));
	img->base = (struct blt_image){
		.impl = &image_impl,
		.width = w,
		.height = h,
		.format = format,
	};
	img->swizzle = 21; //flags & BLT_IMAGE_LINEAR ? 0 : 21;
	img->stride = ALIGN_UP(w, 128) * 4;
	size = img->stride * ALIGN_UP(h, 128);
	ret = bo_alloc(ctx, &img->bo, size, 0x40000, AMDGPU_GEM_DOMAIN_VRAM, 0, 0);
	if (ret < 0)
		goto error0;
	if (ctx->chip.class >= GFX9) {
		img->desc[0] = img->bo.addr >> 8; // XXX tile swizzle?
		img->desc[1] =
			S_00A004_BASE_ADDRESS_HI(img->bo.addr >> 40) |
			S_00A004_FORMAT(V_00A004_IMG_FORMAT_8_8_8_8_UNORM) |
			S_00A004_WIDTH_LO(img->stride / 4 - 1);
		img->desc[2] =
			S_00A008_WIDTH_HI((img->stride / 4 - 1) >> 2) |
			S_00A008_HEIGHT(img->base.height - 1) |
			S_00A008_RESOURCE_LEVEL(1);
		img->desc[3] =
			S_00A00C_DST_SEL_X(V_008F0C_SQ_SEL_Z) |
			S_00A00C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
			S_00A00C_DST_SEL_Z(V_008F0C_SQ_SEL_X) |
			S_00A00C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
			S_00A00C_BASE_LEVEL(0) |
			S_00A00C_LAST_LEVEL(0) |
			S_00A00C_SW_MODE(img->swizzle) |
			S_00A00C_BC_SWIZZLE(V_00A00C_BC_SWIZZLE_ZYXW) |
			S_00A00C_TYPE(V_008F1C_SQ_RSRC_IMG_2D);
		metadata.tiling_info = AMDGPU_TILING_SET(SWIZZLE_MODE, img->swizzle);
	} else {
		metadata.tiling_info =
			AMDGPU_TILING_SET(ARRAY_MODE, 4) |
			AMDGPU_TILING_SET(PIPE_CONFIG, 5) |
			AMDGPU_TILING_SET(TILE_SPLIT, 3) |
			AMDGPU_TILING_SET(BANK_WIDTH, 0) |
			AMDGPU_TILING_SET(BANK_HEIGHT, 2) |
			AMDGPU_TILING_SET(MACRO_TILE_ASPECT, 2) |
			AMDGPU_TILING_SET(NUM_BANKS, 3);
	}
	ret = amdgpu_bo_set_metadata(img->bo.handle, &metadata);
	if (ret < 0)
		goto error1;
	if (flags & BLT_IMAGE_DST) {
		img->draw = new_draw(ctx);
		if (!img->draw)
			goto error1;
	}

	return &img->base;

error1:
	bo_free(&img->bo);
error0:
	free(img);
	return NULL;
}

static void
emit(struct cmdbuf *cmd, uint32_t val)
{
	assert(cmd->len < cmd->cap);
	cmd->buf[cmd->len++] = val;
}

static void
emit_array(struct cmdbuf *cmd, size_t len, const uint32_t *val)
{
	assert(cmd->len + len <= cmd->cap);
	memcpy(&cmd->buf[cmd->len], val, len * sizeof(val[0]));
	cmd->len += len;
}

static void
set_context_reg_idx(struct cmdbuf *cmd, uint32_t reg, int idx, uint32_t val)
{
	emit(cmd, PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
	emit(cmd, (reg - SI_CONTEXT_REG_OFFSET) >> 2 | idx << 28);
	emit(cmd, val);
}

static void
set_context_reg(struct cmdbuf *cmd, uint32_t reg, uint32_t val)
{
	set_context_reg_idx(cmd, reg, 0, val);
}

static void
set_context_reg_seq(struct cmdbuf *cmd, uint32_t reg, int len, uint32_t *val)
{
	emit(cmd, PKT3(PKT3_SET_CONTEXT_REG, len, 0));
	emit(cmd, (reg - SI_CONTEXT_REG_OFFSET) >> 2);
	for (; len; --len, ++val)
		emit(cmd, *val);
}

static void
set_sh_reg_idx(struct context *ctx, struct cmdbuf *cmd, uint32_t reg, int idx, uint32_t val)
{
	emit(cmd, PKT3(ctx->chip.class >= GFX10 ? PKT3_SET_SH_REG_INDEX : PKT3_SET_SH_REG, 1, 0));
	emit(cmd, (reg - SI_SH_REG_OFFSET) >> 2 | idx << 28);
	emit(cmd, val);
}

static void
set_sh_reg(struct cmdbuf *cmd, uint32_t reg, uint32_t val)
{
	emit(cmd, PKT3(PKT3_SET_SH_REG, 1, 0));
	emit(cmd, (reg - SI_SH_REG_OFFSET) >> 2);
	emit(cmd, val);
}

static void
set_sh_reg_seq(struct cmdbuf *cmd, uint32_t reg, int len, uint32_t *val)
{
	emit(cmd, PKT3(PKT3_SET_SH_REG, len, 0));
	emit(cmd, (reg - SI_SH_REG_OFFSET) >> 2);
	for (; len; --len, ++val)
		emit(cmd, *val);
}

static void
set_uconfig_reg_idx(struct context *ctx, struct cmdbuf *cmd, uint32_t reg, int idx, uint32_t val)
{
	emit(cmd, PKT3(ctx->chip.class >= GFX10 ? PKT3_SET_UCONFIG_REG_INDEX : PKT3_SET_UCONFIG_REG, 1, 0));
	emit(cmd, (reg - CIK_UCONFIG_REG_OFFSET) >> 2 | idx << 28);
	emit(cmd, val);
}

static void
set_uconfig_reg(struct context *ctx, struct cmdbuf *cmd, uint32_t reg, uint32_t val)
{
	emit(cmd, PKT3(PKT3_SET_UCONFIG_REG, 1, 0));
	emit(cmd, (reg - CIK_UCONFIG_REG_OFFSET) >> 2);
	emit(cmd, val);
}

static int
draw(struct context *ctx)
{
	struct image *dst = (void *)ctx->base.dst;
	struct cmdbuf *cmd = &dst->draw->cmd;

	if (0) {
		set_context_reg_idx(cmd, R_028AA8_IA_MULTI_VGT_PARAM, 1,
			S_028AA8_PRIMGROUP_SIZE(127) |
			S_028AA8_WD_SWITCH_ON_EOP(1) |
			S_028AA8_MAX_PRIMGRP_IN_WAVE(2));
	}
	if (ctx->chip.class >= GFX9)
		set_uconfig_reg(ctx, cmd, R_03092C_VGT_MULTI_PRIM_IB_RESET_EN, 0);
	else
		set_context_reg(cmd, R_028A94_VGT_MULTI_PRIM_IB_RESET_EN, 0);
	set_sh_reg_seq(cmd, R_00B13C_SPI_SHADER_USER_DATA_VS_3, 5, (uint32_t[]){
		(dst->draw->vert.pos - 4) / 2,
		ftou(ctx->base.dst_x),
		ftou(ctx->base.dst_y),
		ftou(ctx->base.src_x),
		ftou(ctx->base.src_y),
	});
	emit(cmd, PKT3(PKT3_NUM_INSTANCES, 0, 0));
	emit(cmd, 1);
	emit(cmd, PKT3(PKT3_DRAW_INDEX_AUTO, 1, 0));
	emit(cmd, (dst->draw->vert.len - dst->draw->vert.pos) / 2); /* vertex count */
	emit(cmd, V_0287F0_DI_SRC_SEL_AUTO_INDEX | S_0287F0_USE_OPAQUE(0));
	dst->draw->vert.pos = dst->draw->vert.len;

	return 0;
}

static int
submit(struct context *ctx)
{
	struct image *dst = (void *)ctx->base.dst;
	struct cmdbuf *cmd = &dst->draw->cmd;
	amdgpu_bo_list_handle resources;
	int ret;

	draw(ctx);

	while (cmd->len % 8)
		emit(cmd, 0xffff1000);

	ret = amdgpu_bo_list_create(ctx->dev, 6, (amdgpu_bo_handle[]){
		cmd->bo.handle,
		dst->draw->vert.bo.handle,
		ctx->shader.vert.handle,
		ctx->shader.fill.handle,
		ctx->init.bo.handle,
		dst->bo.handle,
	}, NULL, &resources);
	if (ret < 0)
		return ret;
	ret = amdgpu_cs_submit(ctx->cs, 0, &(struct amdgpu_cs_request){
		.ip_type = AMDGPU_HW_IP_GFX,
		.ring = 0,
		.resources = resources,
		.number_of_dependencies = 0,
		.dependencies = NULL,
		.number_of_ibs = 1,
		.ibs = &(struct amdgpu_cs_ib_info){
			.ib_mc_address = cmd->bo.addr,
			.size = cmd->len,
		},
	}, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int
setup(struct blt_context *ctx_base, int op, struct blt_image *dst_base, struct blt_image *src_base, struct blt_image *mask)
{
	struct context *ctx = (void *)ctx_base;
	struct image *dst;
	struct cmdbuf *cmd;

	if (mask)
		return -1;
	if (ctx->base.dst && dst_base != ctx->base.dst)
		submit(ctx);
	if (!dst_base)
		return 0;
	if (dst_base->impl != &image_impl)
		return -1;
	dst = (void *)dst_base;
	if (!dst->draw)
		return -1;
	cmd = &dst->draw->cmd;

	if (dst_base != ctx->base.dst) {
		ctx->base.src = NULL;

		/* radv_init_graphics_state */
		emit(cmd, PKT3(PKT3_INDIRECT_BUFFER_CIK, 2, 0));
		emit(cmd, ctx->init.bo.addr);
		emit(cmd, ctx->init.bo.addr >> 32);
		emit(cmd, ctx->init.len);

		/* si_cs_emit_cache_flush */
		emit(cmd, PKT3(PKT3_EVENT_WRITE, 0, 0));
		emit(cmd, EVENT_TYPE(V_028A90_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));
		emit(cmd, PKT3(PKT3_EVENT_WRITE, 0, 0));
		emit(cmd, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH) | EVENT_INDEX(4));

		if (0) {
			emit(cmd, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
			emit(cmd, 0);
		}

		/* si_emit_acquire_mem */
		if (1) {
			emit_array(cmd, 8, (uint32_t[]){
				PKT3(PKT3_ACQUIRE_MEM, 6, 0) | PKT3_SHADER_TYPE_S(0),
				S_0301F0_TC_WB_ACTION_ENA(0) |
				S_0085F0_TCL1_ACTION_ENA(0) |
				S_0085F0_TC_ACTION_ENA(0) |
				S_0085F0_SH_KCACHE_ACTION_ENA(0) |
				S_0085F0_SH_ICACHE_ACTION_ENA(0),
				0xffffffff,
				0x00ffffff,
				0,
				0,
				10,
				S_586_GLI_INV(V_586_GLI_ALL) |
				S_586_GL1_INV(1) |
				S_586_GLK_INV(1) |
				S_586_GL2_INV(1) |
				S_586_GL2_WB(1) |
				S_586_GLM_INV(1) |
				S_586_GLM_WB(1) |
				S_586_GLV_INV(1),
			});
		} else {
			emit(cmd, PKT3(PKT3_SURFACE_SYNC, 3, 0));
			emit(cmd,
			     S_0301F0_TC_WB_ACTION_ENA(1) |
			     S_0085F0_TCL1_ACTION_ENA(1) |
			     S_0085F0_TC_ACTION_ENA(1) |
			     S_0085F0_SH_KCACHE_ACTION_ENA(1) |
			     S_0085F0_SH_ICACHE_ACTION_ENA(1));
			emit(cmd, 0xffffffff); /* CP_COHER_SIZE */
			emit(cmd, 0x00000000); /* CP_COHER_BASE */
			emit(cmd, 0x0000000a); /* POLL_INTERVAL */
		}

		emit(cmd, PKT3(PKT3_EVENT_WRITE, 0, 0));
		emit(cmd, EVENT_TYPE(V_028A90_PIPELINESTAT_START) | EVENT_INDEX(0));

		set_sh_reg(cmd, R_00B138_SPI_SHADER_USER_DATA_VS_2, dst->draw->vert.bo.addr);

		/* radv_update_multisample_state */
		set_context_reg_seq(cmd, R_028BDC_PA_SC_LINE_CNTL, 2, (uint32_t[]){S_028BDC_DX10_DIAMOND_TEST_ENA(1), 0});
		set_context_reg(cmd, R_028A48_PA_SC_MODE_CNTL_0,
		                S_028A48_VPORT_SCISSOR_ENABLE(1) | S_028A48_ALTERNATE_RBS_PER_TILE(ctx->chip.class >= GFX9));
		set_context_reg_seq(cmd, R_028BD4_PA_SC_CENTROID_PRIORITY_0, 2, (uint32_t[]){0, 0});
		set_context_reg(cmd, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, 0);
		set_context_reg(cmd, R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0, 0);
		set_context_reg(cmd, R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0, 0);
		set_context_reg(cmd, R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0, 0);

		/* radv_update_binning_state */
		if (ctx->chip.class >= GFX9) {
			set_context_reg(cmd, R_028C44_PA_SC_BINNER_CNTL_0,
			                S_028C44_BIN_SIZE_X_EXTEND(V_028C44_BIN_SIZE_256_PIXELS) |
			                S_028C44_BIN_SIZE_Y_EXTEND(V_028C44_BIN_SIZE_256_PIXELS) |
			                S_028C44_DISABLE_START_OF_PRIM(1) |
			                S_028C44_FPOVS_PER_BATCH(63) |
			                S_028C44_OPTIMAL_BIN_SELECTION(1) |
			                S_028C44_FLUSH_ON_BINNING_TRANSITION(1));
			if (ctx->chip.class >= GFX10)
				set_context_reg(cmd, R_028038_DB_DFSM_CONTROL, S_028038_PUNCHOUT_MODE(V_028038_FORCE_OFF));
			else
				set_context_reg(cmd, R_028060_DB_DFSM_CONTROL, S_028060_PUNCHOUT_MODE(V_028060_FORCE_OFF));
		}

		set_sh_reg_seq(cmd, R_00B120_SPI_SHADER_PGM_LO_VS, 4, (uint32_t[]){
			ctx->shader.vert.addr >> 8,
			S_00B124_MEM_BASE(ctx->shader.vert.addr >> 40),
			vert_info.rsrc1 | S_00B128_FLOAT_MODE(V_00B028_FP_64_DENORMS) | S_00B128_DX10_CLAMP(1) | S_00B128_VGPR_COMP_CNT(0) | S_00B128_MEM_ORDERED(ctx->chip.class >= GFX10),
			vert_info.rsrc2,
		});
		set_uconfig_reg_idx(ctx, cmd, R_030908_VGT_PRIMITIVE_TYPE, 1, V_008958_DI_PT_RECTLIST);

		/* radv_emit_fb_color_state */
		if (ctx->chip.class >= GFX10) {
			set_context_reg_seq(cmd, R_028C60_CB_COLOR0_BASE, 11, (uint32_t[]){
				dst->bo.addr >> 8,
				0,
				0,
				0,
				S_028C70_FORMAT(V_028C70_COLOR_8_8_8_8) | S_028C70_COMP_SWAP(1) | S_028C70_BLEND_CLAMP(1) | S_028C70_SIMPLE_FLOAT(1),
				0,
				0,
				dst->bo.addr >> 8,
				0,
				dst->bo.addr >> 8,
				0,
			});
			set_context_reg_seq(cmd, R_028C94_CB_COLOR0_DCC_BASE, 1, (uint32_t[]){
				dst->bo.addr >> 8,
			});
			set_context_reg(cmd, R_028E40_CB_COLOR0_BASE_EXT, dst->bo.addr >> 40);
			set_context_reg(cmd, R_028E60_CB_COLOR0_CMASK_BASE_EXT, dst->bo.addr >> 40);
			set_context_reg(cmd, R_028E80_CB_COLOR0_FMASK_BASE_EXT, dst->bo.addr >> 40);
			set_context_reg(cmd, R_028EA0_CB_COLOR0_DCC_BASE_EXT, dst->bo.addr >> 40);
			set_context_reg(cmd, R_028EC0_CB_COLOR0_ATTRIB2, S_028EC0_MIP0_WIDTH(dst->base.width - 1) | S_028EC0_MIP0_HEIGHT(dst->base.height - 1));
			set_context_reg(cmd, R_028EE0_CB_COLOR0_ATTRIB3, S_028EE0_COLOR_SW_MODE(dst->swizzle) | S_028EE0_FMASK_SW_MODE(20) | S_028EE0_RESOURCE_TYPE(1) | S_028EE0_RESOURCE_LEVEL(1));
		} else {
			set_context_reg_seq(cmd, R_028C60_CB_COLOR0_BASE, 11, (uint32_t[]){
				dst->bo.addr >> 8,

				S_028C64_TILE_MAX(0x3f) | S_028C64_FMASK_TILE_MAX(0x3f),

				S_028C68_TILE_MAX(0xfff),

				0,//S_028C6C_SLICE_START(0) | S_028C6C_SLICE_MAX(0),

				S_028C70_FORMAT(V_028C70_COLOR_8_8_8_8) |
				S_028C70_LINEAR_GENERAL(0) |
				S_028C70_NUMBER_TYPE(V_028C70_NUMBER_UNORM) |
				S_028C70_COMP_SWAP(V_028C70_SWAP_ALT) |
				S_028C70_FAST_CLEAR(0) |
				S_028C70_COMPRESSION(0) |
				S_028C70_BLEND_CLAMP(1) |
				S_028C70_BLEND_BYPASS(0) |
				S_028C70_SIMPLE_FLOAT(1) |
				S_028C70_ROUND_MODE(0) |
				S_028C70_CMASK_IS_LINEAR(0) |
				S_028C70_BLEND_OPT_DONT_RD_DST(V_028C70_FORCE_OPT_AUTO) |
				S_028C70_BLEND_OPT_DISCARD_PIXEL(V_028C70_FORCE_OPT_AUTO) |
				S_028C70_FMASK_COMPRESSION_DISABLE(0) |
				S_028C70_FMASK_COMPRESS_1FRAG_ONLY(0) |
				S_028C70_DCC_ENABLE(0) |
				S_028C70_CMASK_ADDR_TYPE(0),

				S_028C74_TILE_MODE_INDEX(0) |
				S_028C74_FMASK_TILE_MODE_INDEX(0),

				/* dcc control */
				0,

				/* cmask */
				dst->bo.addr >> 8,
				S_028C80_TILE_MAX(0xf),

				/* fmask */
				dst->bo.addr >> 8,
				S_028C88_TILE_MAX(0xfff)
			});
			set_context_reg(cmd, R_028C94_CB_COLOR0_DCC_BASE, dst->bo.addr >> 8);
		}

		emit_array(cmd, LEN(pipeline), pipeline);

		set_context_reg(cmd, R_028CAC_CB_COLOR1_INFO, S_028C70_FORMAT(V_028C70_COLOR_INVALID));
		set_context_reg(cmd, R_028CE8_CB_COLOR2_INFO, S_028C70_FORMAT(V_028C70_COLOR_INVALID));
		set_context_reg(cmd, R_028D24_CB_COLOR3_INFO, S_028C70_FORMAT(V_028C70_COLOR_INVALID));
		set_context_reg(cmd, R_028D60_CB_COLOR4_INFO, S_028C70_FORMAT(V_028C70_COLOR_INVALID));
		set_context_reg(cmd, R_028D9C_CB_COLOR5_INFO, S_028C70_FORMAT(V_028C70_COLOR_INVALID));
		set_context_reg(cmd, R_028DD8_CB_COLOR6_INFO, S_028C70_FORMAT(V_028C70_COLOR_INVALID));
		set_context_reg(cmd, R_028E14_CB_COLOR7_INFO, S_028C70_FORMAT(V_028C70_COLOR_INVALID));
		set_context_reg_seq(cmd, R_028040_DB_Z_INFO, 2, (uint32_t[]){
			S_028040_FORMAT(V_028040_Z_INVALID) /* DB_Z_INFO */,
			S_028044_FORMAT(V_028044_STENCIL_INVALID) /* DB_STENCIL_INFO */,
		});
		set_context_reg(cmd, R_028208_PA_SC_WINDOW_SCISSOR_BR,
		                S_028208_BR_X(dst->base.width) | S_028208_BR_Y(dst->base.height));
		set_context_reg(cmd, R_028424_CB_DCC_CONTROL,
		                S_028424_OVERWRITE_COMBINER_WATERMARK(6) |
		                S_028424_DISABLE_CONSTANT_ENCODE_REG(1));

		/* si_write_viewport */
		set_context_reg_seq(cmd, R_02843C_PA_CL_VPORT_XSCALE, 6, (uint32_t[]){
			ftou(1),
			ftou(0),
			ftou(1),
			ftou(0),
			ftou(0),
			ftou(0),
		});
		set_context_reg_seq(cmd, R_0282D0_PA_SC_VPORT_ZMIN_0, 2, (uint32_t[]){ftou(0), ftou(0)});

		/* si_write_scissors */
		set_context_reg_seq(cmd, R_028250_PA_SC_VPORT_SCISSOR_0_TL, 2, (uint32_t[]){
			S_028250_TL_X(0) | S_028250_TL_Y(0) | S_028250_WINDOW_OFFSET_DISABLE(1),
			S_028254_BR_X(dst->base.width) | S_028254_BR_Y(dst->base.height)
		});
		set_context_reg_seq(cmd, R_028BE8_PA_CL_GB_VERT_CLIP_ADJ, 4, (uint32_t[]){
			0x4322d5c3 /* 162.835007 */,
			0x3f800000 /* 0.0 */,
			0x4322d5c3 /* 162.835007 */,
			0x3f800000 /* 0.0 */
		});
	}
	if (src_base != ctx->base.src) {
		if (ctx->base.dst)
			draw(ctx);
		if (src_base->impl == &image_impl) {
			struct image *src = (void *)src_base;

			set_sh_reg_seq(cmd, R_00B030_SPI_SHADER_USER_DATA_PS_0, 4, src->desc);
			set_sh_reg_seq(cmd, R_00B020_SPI_SHADER_PGM_LO_PS, 4, (uint32_t[]){
				ctx->shader.copy.addr >> 8,
				S_00B024_MEM_BASE(ctx->shader.copy.addr >> 40),
				copy_info.rsrc1 | S_00B028_FLOAT_MODE(V_00B028_FP_64_DENORMS) | S_00B028_DX10_CLAMP(1) | S_00B028_MEM_ORDERED(ctx->chip.class >= GFX10),
				copy_info.rsrc2,
			});
		} else if (src_base->impl == &blt_solid_image_impl) {
			struct blt_solid *src = (void *)src_base;

			set_sh_reg_seq(cmd, R_00B038_SPI_SHADER_USER_DATA_PS_2, 4, (uint32_t[]){
				ftou((float)src->color.red / UINT16_MAX),
				ftou((float)src->color.green / UINT16_MAX),
				ftou((float)src->color.blue / UINT16_MAX),
				ftou(1),
			});
			set_sh_reg_seq(cmd, R_00B020_SPI_SHADER_PGM_LO_PS, 4, (uint32_t[]){
				ctx->shader.fill.addr >> 8,
				S_00B024_MEM_BASE(ctx->shader.fill.addr >> 40),
				fill_info.rsrc1 | S_00B028_FLOAT_MODE(V_00B028_FP_64_DENORMS) | S_00B028_DX10_CLAMP(1) | S_00B028_MEM_ORDERED(ctx->chip.class >= GFX10),
				fill_info.rsrc2,
			});
		}
	}

	return 0;
}

static int
rect(struct blt_context *ctx_base, size_t len, const struct pixman_box32 *rect)
{
	struct context *ctx = (void *)ctx_base;
	struct image *dst = (void *)ctx->base.dst;
	struct vertbuf *vert = &dst->draw->vert;

	for (; len; --len) {
		vert->buf[vert->len++] = rect->x1;
		vert->buf[vert->len++] = rect->y1;
		vert->buf[vert->len++] = rect->x1;
		vert->buf[vert->len++] = rect->y2;
		vert->buf[vert->len++] = rect->x2;
		vert->buf[vert->len++] = rect->y1;
	}

	return 0;
}

static const struct blt_context_impl impl = {
	.new_image = new_image,
	.new_solid = blt_new_solid_image,
	.setup = setup,
	.rect = rect,
};

/* XXX: figure out what this stuff does and why (if) it's necessary */
static void
gfx_init(struct context *ctx, struct cmdbuf *cmd)
{
	emit(cmd, PKT3(PKT3_CLEAR_STATE, 0, 0));
	emit(cmd, 0);
	if (ctx->chip.class <= GFX8) {
		// XXX: raster config
	}
	set_context_reg(cmd, R_028A18_VGT_HOS_MAX_TESS_LEVEL, ftou(64));
	if (ctx->chip.class <= GFX8) {
		set_context_reg(cmd, R_028A54_VGT_GS_PER_ES, 0x80);
		set_context_reg(cmd, R_028A58_VGT_ES_PER_GS, 0x40);
	}
	if (ctx->chip.class <= GFX9)
		set_context_reg(cmd, R_028AA0_VGT_INSTANCE_STEP_RATE_0, 1);
	set_context_reg(cmd, R_02800C_DB_RENDER_OVERRIDE,
	                S_02800C_FORCE_HIS_ENABLE0(V_02800C_FORCE_DISABLE) | S_02800C_FORCE_HIS_ENABLE1(V_02800C_FORCE_DISABLE));
	if (ctx->chip.class >= GFX10) {
		set_context_reg(cmd, R_028A98_VGT_DRAW_PAYLOAD_CNTL, 0);
		set_uconfig_reg(ctx, cmd, R_030964_GE_MAX_VTX_INDX, ~0);
		set_uconfig_reg(ctx, cmd, R_030924_GE_MIN_VTX_INDX, 0);
		set_uconfig_reg(ctx, cmd, R_030928_GE_INDX_OFFSET, 0);
		set_uconfig_reg(ctx, cmd, R_03097C_GE_STEREO_CNTL, 0);
		set_uconfig_reg(ctx, cmd, R_030988_GE_USER_VGPR_EN, 0);
	} else if (ctx->chip.class == GFX9) {
		set_uconfig_reg(ctx, cmd, R_030920_VGT_MAX_VTX_INDX, ~0);
		set_uconfig_reg(ctx, cmd, R_030924_VGT_MIN_VTX_INDX, 0);
		set_uconfig_reg(ctx, cmd, R_030928_VGT_INDX_OFFSET, 0);
	} else {
		set_context_reg(cmd, R_028400_VGT_MAX_VTX_INDX, ~0);
		set_context_reg(cmd, R_028404_VGT_MIN_VTX_INDX, 0);
		set_context_reg(cmd, R_028408_VGT_INDX_OFFSET, 0);
	}
	if (ctx->chip.class >= GFX10) {
		set_sh_reg_idx(ctx, cmd, R_00B404_SPI_SHADER_PGM_RSRC4_HS, 3, S_00B404_CU_EN(0xffff));
		set_sh_reg_idx(ctx, cmd, R_00B104_SPI_SHADER_PGM_RSRC4_VS, 3, S_00B104_CU_EN(0xffff));
		set_sh_reg_idx(ctx, cmd, R_00B004_SPI_SHADER_PGM_RSRC4_PS, 3, S_00B004_CU_EN(0xffff));
	}
	if (ctx->chip.class >= GFX9) {
		set_sh_reg_idx(ctx, cmd, R_00B41C_SPI_SHADER_PGM_RSRC3_HS, 3, S_00B41C_CU_EN(0xffff) | S_00B41C_WAVE_LIMIT(0x3f));
	} else {
		set_sh_reg(cmd, R_00B51C_SPI_SHADER_PGM_RSRC3_LS, S_00B51C_CU_EN(0xffff) | S_00B51C_WAVE_LIMIT(0x3f));
		set_sh_reg(cmd, R_00B41C_SPI_SHADER_PGM_RSRC3_HS, S_00B41C_WAVE_LIMIT(0x3F));
		set_sh_reg(cmd, R_00B31C_SPI_SHADER_PGM_RSRC3_ES, S_00B31C_CU_EN(0xffff) | S_00B31C_WAVE_LIMIT(0x3f));
		set_context_reg(cmd, R_028A44_VGT_GS_ONCHIP_CNTL, S_028A44_ES_VERTS_PER_SUBGRP(64) | S_028A44_GS_PRIMS_PER_SUBGRP(4));
	}
	set_sh_reg_idx(ctx, cmd, R_00B118_SPI_SHADER_PGM_RSRC3_VS, 3, S_00B118_CU_EN(0xfff3) | S_00B118_WAVE_LIMIT(0x3f));  // XXX
	set_sh_reg(cmd, R_00B11C_SPI_SHADER_LATE_ALLOC_VS, S_00B11C_LIMIT(28));  // XXX
	set_sh_reg_idx(ctx, cmd, R_00B21C_SPI_SHADER_PGM_RSRC3_GS, 3, S_00B21C_CU_EN(0xfff3) | S_00B21C_WAVE_LIMIT(0x3f));  // XXX
	if (ctx->chip.class >= GFX10)
		set_sh_reg_idx(ctx, cmd, R_00B204_SPI_SHADER_PGM_RSRC4_GS, 3, S_00B204_CU_EN(0xffff) | S_00B204_SPI_SHADER_LATE_ALLOC_GS_GFX10(28));  // XXX
	set_sh_reg_idx(ctx, cmd, R_00B01C_SPI_SHADER_PGM_RSRC3_PS, 3, S_00B01C_CU_EN(0xffff) | S_00B01C_WAVE_LIMIT(0x3f));
	if (ctx->chip.class >= GFX10) {
		set_context_reg(cmd, R_028C50_PA_SC_NGG_MODE_CNTL, S_028C50_MAX_DEALLOCS_IN_WAVE(512));
		set_context_reg(cmd, R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL, 14);
		set_context_reg(cmd, R_02807C_DB_RMI_L2_CACHE_CONTROL,
		                S_02807C_Z_WR_POLICY(V_02807C_CACHE_STREAM_WR) |
		                S_02807C_S_WR_POLICY(V_02807C_CACHE_STREAM_WR) |
		                S_02807C_HTILE_WR_POLICY(V_02807C_CACHE_STREAM_WR) |
		                S_02807C_ZPCPSD_WR_POLICY(V_02807C_CACHE_STREAM_WR) |
		                S_02807C_Z_RD_POLICY(V_02807C_CACHE_NOA_RD) |
		                S_02807C_S_RD_POLICY(V_02807C_CACHE_NOA_RD) |
		                S_02807C_HTILE_RD_POLICY(V_02807C_CACHE_NOA_RD));
		set_context_reg(cmd, R_028410_CB_RMI_GL2_CACHE_CONTROL,
		                S_028410_CMASK_WR_POLICY(V_028410_CACHE_STREAM_WR) |
		                S_028410_FMASK_WR_POLICY(V_028410_CACHE_STREAM_WR) |
		                S_028410_DCC_WR_POLICY(V_028410_CACHE_STREAM_WR) |
		                S_028410_COLOR_WR_POLICY(V_028410_CACHE_STREAM_WR) |
		                S_028410_CMASK_RD_POLICY(V_028410_CACHE_NOA_RD) |
		                S_028410_FMASK_RD_POLICY(V_028410_CACHE_NOA_RD) |
		                S_028410_DCC_RD_POLICY(V_028410_CACHE_NOA_RD) | 
		                S_028410_COLOR_RD_POLICY(V_028410_CACHE_NOA_RD));
		set_context_reg(cmd, R_028428_CB_COVERAGE_OUT_CONTROL, 0);
		set_sh_reg(cmd, R_00B0C0_SPI_SHADER_REQ_CTRL_PS, S_00B0C0_SOFT_GROUPING_EN(1) | S_00B0C0_NUMBER_OF_REQUESTS_PER_CU(4 - 1));
		set_sh_reg(cmd, R_00B1C0_SPI_SHADER_REQ_CTRL_VS, 0);
		emit(cmd, PKT3(PKT3_EVENT_WRITE, 0, 0));
		emit(cmd, EVENT_TYPE(V_028A90_SQ_NON_EVENT) | EVENT_INDEX(0));
		set_uconfig_reg(ctx, cmd, R_030980_GE_PC_ALLOC, S_030980_OVERSUB_EN(1) | S_030980_NUM_PC_LINES(255));
	}
	set_context_reg(cmd, R_028B50_VGT_TESS_DISTRIBUTION,
	                S_028B50_ACCUM_ISOLINE(32) | S_028B50_ACCUM_TRI(11) | S_028B50_ACCUM_QUAD(11) | S_028B50_DONUT_SPLIT(16) | S_028B50_TRAP_SPLIT(3));
	if (ctx->chip.class >= GFX9) {
		set_context_reg(cmd, R_028C48_PA_SC_BINNER_CNTL_1, S_028C48_MAX_ALLOC_COUNT(340) | S_028C48_MAX_PRIM_PER_BATCH(1023));  // XXX
		set_context_reg(cmd, R_028C4C_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL, S_028C4C_NULL_SQUAD_AA_MASK_ENABLE(1));
		set_uconfig_reg(ctx, cmd, R_030968_VGT_INSTANCE_BASE_ID, 0);
	}
	set_context_reg(cmd, R_028A00_PA_SU_POINT_SIZE, S_028A00_HEIGHT(8) | S_028A00_WIDTH(8));
	set_context_reg(cmd, R_028A04_PA_SU_POINT_MINMAX, S_028A04_MIN_SIZE(0) | S_028A04_MAX_SIZE(0xffff));
	if (ctx->chip.family >= CHIP_POLARIS10) {
		set_context_reg(cmd, R_028830_PA_SU_SMALL_PRIM_FILTER_CNTL,
		                S_028830_SMALL_PRIM_FILTER_ENABLE(1) | S_028830_LINE_FILTER_DISABLE(ctx->chip.family <= CHIP_POLARIS12));
	}
	set_sh_reg_seq(cmd, R_00B810_COMPUTE_START_X, 3, (uint32_t[]){0, 0, 0});
	set_sh_reg_seq(cmd, R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0, 2, (uint32_t[]){
		S_00B858_SH0_CU_EN(0xffff) | S_00B858_SH1_CU_EN(0xffff),
		S_00B858_SH0_CU_EN(0xffff) | S_00B858_SH1_CU_EN(0xffff)
	});
	set_sh_reg_seq(cmd, R_00B864_COMPUTE_STATIC_THREAD_MGMT_SE2, 2, (uint32_t[]){
		S_00B858_SH0_CU_EN(0xffff) | S_00B858_SH1_CU_EN(0xffff),
		S_00B858_SH0_CU_EN(0xffff) | S_00B858_SH1_CU_EN(0xffff)
	});
	if (ctx->chip.class >= GFX10)
		set_sh_reg(cmd, R_00B8A0_COMPUTE_PGM_RSRC3, 0);
	while (cmd->len % 8)
		emit(cmd, 0xffff1000);
}

struct blt_context *
blt_amdgpu_new(int fd)
{
	struct context *ctx;
	int ret;
	uint32_t maj, min;
	void *map;

	if (fd < 0) {
		fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		if (fd < 0)
			goto error0;
	}
	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		goto error1;
	ctx->base.impl = &impl;
	ctx->base.op = BLT_OP_SRC;
	ctx->base.dst = NULL;
	ctx->base.src = NULL;
	ctx->base.msk = NULL;
	ctx->fd = fd;

	ret = amdgpu_device_initialize(fd, &maj, &min, &ctx->dev);
	if (ret < 0)
		goto error2;
	ret = amdgpu_cs_ctx_create(ctx->dev, &ctx->cs);
	if (ret < 0)
		goto error3;
	ret = amdgpu_query_gpu_info(ctx->dev, &ctx->info);
	if (ret < 0)
		goto error3;
	switch (ctx->info.family_id) {
	case AMDGPU_FAMILY_VI:
		ctx->chip.class = GFX8;
		goto error4;
	case AMDGPU_FAMILY_NV:
		ctx->chip.class = GFX10;
		break;
	default:
		goto error4;
	}

	ret = bo_alloc(ctx, &ctx->shader.fill, ALIGN_UP(sizeof(fill_code) + 0xc0, 0x100), 0x100, AMDGPU_GEM_DOMAIN_VRAM, AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, 0);
	if (ret < 0)
		goto error4;
	ret = amdgpu_bo_cpu_map(ctx->shader.fill.handle, &map);
	if (ret < 0)
		goto error5;
	memcpy(map, fill_code, sizeof(fill_code));
	amdgpu_bo_cpu_unmap(ctx->shader.fill.handle);

	ret = bo_alloc(ctx, &ctx->shader.vert, ALIGN_UP(sizeof(vert_code) + 0xc0, 0x100), 0x100, AMDGPU_GEM_DOMAIN_VRAM, AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, 0);
	if (ret < 0)
		goto error4;
	ret = amdgpu_bo_cpu_map(ctx->shader.vert.handle, &map);
	if (ret < 0)
		goto error5;
	memcpy(map, vert_code, sizeof(vert_code));
	amdgpu_bo_cpu_unmap(ctx->shader.vert.handle);

	ret = bo_alloc(ctx, &ctx->shader.copy, ALIGN_UP(sizeof(copy_code) + 0xc0, 0x100), 0x100, AMDGPU_GEM_DOMAIN_VRAM, AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, 0);
	if (ret < 0)
		goto error4;
	ret = amdgpu_bo_cpu_map(ctx->shader.copy.handle, &map);
	if (ret < 0)
		goto error5;
	memcpy(map, copy_code, sizeof(copy_code));
	amdgpu_bo_cpu_unmap(ctx->shader.copy.handle);

	ret = bo_alloc(ctx, &ctx->init.bo, 0x4000, 0x1000, AMDGPU_GEM_DOMAIN_GTT, AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, 0);
	if (ret < 0)
		goto error6;
	ret = amdgpu_bo_cpu_map(ctx->init.bo.handle, &map);
	if (ret < 0)
		goto error6;
	ctx->init.buf = map;
	ctx->init.len = 0;
	ctx->init.cap = ctx->init.bo.size / 4;
	gfx_init(ctx, &ctx->init);
	amdgpu_bo_cpu_unmap(ctx->init.bo.handle);

	return &ctx->base;

error6:
	bo_free(&ctx->shader.vert);
error5:
	//bo_free(&ctx->init);
error4:
	amdgpu_cs_ctx_free(ctx->cs);
error3:
	amdgpu_device_deinitialize(ctx->dev);
error2:
	free(ctx);
	errno = -ret;
error1:
	close(fd);
error0:
	return NULL;
}
