#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <blt.h>
#include <blt-drm.h>
#include <pixman.h>

struct framebuffer {
	uint32_t id;
	struct blt_image *image;
};

struct plane {
	int type;
};

static noreturn void
fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (fmt[0] && fmt[strlen(fmt) - 1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}
	exit(1);
}

static void
dumpmod(struct drm_format_modifier *fmtmod)
{
	uint64_t mod = fmtmod->modifier;
	const char *str;

	printf("modifier %#"PRIx64"\n", mod);
	printf("\tformats %#"PRIx64"\n", (uint64_t)fmtmod->formats);
	printf("\toffset %#"PRIx32"\n", (uint32_t)fmtmod->offset);
	if (!IS_AMD_FMT_MOD(mod))
		return;
	switch (AMD_FMT_MOD_GET(TILE_VERSION, mod)) {
	case AMD_FMT_MOD_TILE_VER_GFX9: str = "GFX9"; break;
	case AMD_FMT_MOD_TILE_VER_GFX10: str = "GFX10"; break;
	case AMD_FMT_MOD_TILE_VER_GFX10_RBPLUS: str = "GFX10 RBPLUS"; break;
	default: str = "unknown";
	}
	printf("\tTILE_VERSION: %s\n", str);
	switch (AMD_FMT_MOD_GET(TILE, mod)) {
	case AMD_FMT_MOD_TILE_GFX9_64K_S: str = "64K_S"; break;
	case AMD_FMT_MOD_TILE_GFX9_64K_D: str = "64K_D"; break;
	case AMD_FMT_MOD_TILE_GFX9_64K_S_X: str = "64K_S_X"; break;
	case AMD_FMT_MOD_TILE_GFX9_64K_D_X: str = "64K_D_X"; break;
	case AMD_FMT_MOD_TILE_GFX9_64K_R_X: str = "64K_R_X"; break;
	default: str = "unknown";
	}
	printf("\tTILE: %s\n", str);
	printf("\tDCC: %d\n", (int)AMD_FMT_MOD_GET(DCC, mod));
	printf("\tDCC_RETILE: %d\n", (int)AMD_FMT_MOD_GET(DCC_RETILE, mod));
	printf("\tDCC_PIPE_ALIGN: %d\n", (int)AMD_FMT_MOD_GET(DCC_PIPE_ALIGN, mod));
	printf("\tDCC_INDEPENDENT_64B: %d\n", (int)AMD_FMT_MOD_GET(DCC_INDEPENDENT_64B, mod));
	printf("\tDCC_INDEPENDENT_128B: %d\n", (int)AMD_FMT_MOD_GET(DCC_INDEPENDENT_128B, mod));
	switch (AMD_FMT_MOD_GET(DCC_MAX_COMPRESSED_BLOCK, mod)) {
	case AMD_FMT_MOD_DCC_BLOCK_64B: str = "64B"; break;
	case AMD_FMT_MOD_DCC_BLOCK_128B: str = "128B"; break;
	case AMD_FMT_MOD_DCC_BLOCK_256B: str = "256"; break;
	default: str = "unknown";
	}
	printf("\tDCC_MAX_COMPRESSED_BLOCK: %s\n", str);
	printf("\tDCC_CONSTANT_ENCODE: %d\n", (int)AMD_FMT_MOD_GET(DCC_CONSTANT_ENCODE, mod));
	printf("\tPIPE_XOR_BITS: %d\n", (int)AMD_FMT_MOD_GET(PIPE_XOR_BITS, mod));
	printf("\tBANK_XOR_BITS: %d\n", (int)AMD_FMT_MOD_GET(BANK_XOR_BITS, mod));
	printf("\tPACKERS: %d\n", (int)AMD_FMT_MOD_GET(PACKERS, mod));
	printf("\tRB: %d\n", (int)AMD_FMT_MOD_GET(RB, mod));
	printf("\tPIPE: %d\n", (int)AMD_FMT_MOD_GET(PIPE, mod));
}

static int
checkplane(int fd, uint32_t id, uint32_t crtc_index)
{
	size_t i, j;
	int type = -1, ret = 0;
	drmModePlane *plane;
	drmModeObjectProperties *props;
	drmModePropertyRes *prop;
	drmModePropertyBlobRes *blob;
	struct drm_format_modifier_blob *fmtmod;

	plane = drmModeGetPlane(fd, id);
	if (!plane)
		fatal("drmModeGetPlane:");
	printf("possible_crtcs: %"PRIu32", crtc_index: %"PRIu32"\n", plane->possible_crtcs, crtc_index);
	if (!(plane->possible_crtcs & 1 << crtc_index))
		goto done;
	props = drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
	if (!props)
		fatal("drmModeObjectGetProperties:");
	for (i = 0; i < props->count_props; ++i) {
		prop = drmModeGetProperty(fd, props->props[i]);
		if (!prop)
			fatal("drmModeGetProperty:");
		printf("name: %s\n", prop->name);
		if (strcmp(prop->name, "type") == 0) {
			type = props->prop_values[i];
		} else if (strcmp(prop->name, "IN_FORMATS") == 0) {
			blob = drmModeGetPropertyBlob(fd, props->prop_values[i]);
			if (!blob)
				fatal("drmModeGetPropertyBlob:");
			fmtmod = blob->data;
			for (j = 0; j < fmtmod->count_formats; ++j) {
				uint32_t fmt = ((uint32_t *)((char *)blob->data + fmtmod->formats_offset))[j];
				printf("format[%zu]: %c%c%c%c\n", j, fmt & 0xff, fmt >> 8 & 0xff, fmt >> 16 & 0xff, fmt >> 24);
			}
			for (j = 0; j < fmtmod->count_modifiers; ++j)
				dumpmod(&((struct drm_format_modifier *)((char *)blob->data + fmtmod->modifiers_offset))[j]);
		}
	}
	if (type == DRM_PLANE_TYPE_PRIMARY)
		ret = 1;
done:
	drmModeFreePlane(plane);
	return ret;
}

int
main(int argc, char *argv[])
{
	drmModeRes *res;
	drmModeConnector *con;
	drmModeEncoder *enc;
	drmModeCrtc *crtc;
	drmModePlaneRes *planes;
	int fd, crtc_index;
	uint32_t handle[4] = {0}, offset[4] = {0}, stride[4] = {0};
	uint32_t flags;
	size_t i;
	uint64_t mod[4] = {0};
	struct blt_context *ctx;
	struct blt_image *color, *black;
	struct blt_plane plane[4];
	struct framebuffer fb;

	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0)
		fatal("open:");
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
		fatal("drmSetClientCap:");
	res = drmModeGetResources(fd);
	if (!res)
		fatal("drmModeGetResources:");
	for (i = 0; i < res->count_connectors; ++i) {
		con = drmModeGetConnector(fd, res->connectors[i]);
		if (!con)
			fatal("drmModeGetConnector:");
		if (con->connection == DRM_MODE_CONNECTED && con->count_encoders > 0)
			break;
		drmModeFreeConnector(con);
	}
	if (i == res->count_connectors)
		fatal("no connected connector");
	enc = drmModeGetEncoder(fd, con->encoders[0]);
	if (!enc)
		fatal("drmModeGetEncoder:");
	for (i = 0; i < res->count_crtcs; ++i) {
		if (enc->possible_crtcs & 1 << i)
			break;
	}
	if (i == res->count_crtcs)
		fatal("could not find crtc");
	crtc_index = i;
	crtc = drmModeGetCrtc(fd, res->crtcs[i]);
	if (!crtc)
		fatal("drmModeGetCrtc:");
	planes = drmModeGetPlaneResources(fd);
	if (!planes)
		fatal("drmModeGetPlaneRes:");
	for (i = 0; i < planes->count_planes; ++i) {
		if (checkplane(fd, planes->planes[i], crtc_index))
			break;
	}
	if (i == planes->count_planes)
		fatal("could not find primary plane");

	printf("creating context\n");
	ctx = blt_drm_new(fd);
	if (!ctx)
		fatal("blt_drm_new:");
	printf("created context\n");
	//fb.image = blt_new_image(ctx, crtc->mode.hdisplay, crtc->mode.vdisplay, BLT_IMAGE_DST | BLT_IMAGE_SCANOUT, BLT_FMT('X', 'R', '2', '4'), BLT_MOD_INVALID, NULL);
	fb.image = blt_new_image(ctx, crtc->mode.hdisplay, crtc->mode.vdisplay, BLT_FMT('X', 'R', '2', '4'), BLT_IMAGE_DST | BLT_IMAGE_SCANOUT);
	if (!fb.image)
		fatal("blt_new_image:");
	printf("created image\n");
	if (blt_image_export_dmabuf(ctx, fb.image, plane, &mod[0]) != 1)
		fatal("blt_export_dmabuf:");
	if (drmPrimeFDToHandle(fd, plane[0].fd, &handle[0]) != 0)
		fatal("drmModePrimeFDToHandle:");
	flags = 0;
	if (mod[0] != DRM_FORMAT_MOD_INVALID)
		flags |= DRM_MODE_FB_MODIFIERS;
	else
		mod[0] = 0;
	fprintf(stderr, "flags: %"PRIu32"\n", flags);
	offset[0] = plane[0].offset;
	stride[0] = plane[0].stride;
	//mod[0] = /*DRM_FORMAT_MOD_INVALID;//*/AMD_FMT_MOD | AMD_FMT_MOD_TILE_VER_GFX9 | AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S);
	printf("offset[0]: %u\n", offset[0]);
	printf("stride[0]: %u\n", stride[0]);
	printf("mod[0]: %"PRIx64"\n", mod[0]);
	if (drmModeAddFB2WithModifiers(fd, fb.image->width, fb.image->height, DRM_FORMAT_XRGB8888, handle, stride, offset, mod, &fb.id, flags) != 0)
		fatal("drmModeAddFB2:");
	printf("fb.id: %"PRIu32"\n", fb.id);

	black = blt_new_solid(ctx, (struct blt_color){0x0000, 0x0000, 0x0000, 0xffff});
	if (!black)
		fatal("blt_new_image failed");
	color = blt_new_solid(ctx, (struct blt_color){0x3333, 0x8888, 0x3333, 0xffff});
	if (!color)
		fatal("blt_new_image failed");
	blt_setup(ctx, BLT_OP_SRC, fb.image, 0, 0, black, 0, 0, NULL, 0, 0);
	blt_rect(ctx, 1, (struct blt_rect[]){{0, 0, fb.image->width, fb.image->height}});
	blt_src(ctx, color, 0, 0);
	blt_rect(ctx, 1, (struct blt_rect[]){{100, 100, 800, 600}});
	blt_dst(ctx, NULL, 0, 0);

	if (drmModeSetCrtc(fd, crtc->crtc_id, fb.id, 0, 0, &con->connector_id, 1, &crtc->mode) != 0)
		fatal("drmModeSetCrtc:");

	sleep(5);
}
