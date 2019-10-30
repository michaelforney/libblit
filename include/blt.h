#ifndef BLT_H
#define BLT_H

#include <stddef.h>
#include <stdint.h>

#define BLT_FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

struct pixman_region32;
struct pixman_box32;

/* damage */
struct blt_damage *blt_new_damage(int max);
struct pixman_region32 *blt_damage(struct blt_damage *dmg, int age, struct pixman_region32 *new);
void blt_cycle_damage(struct blt_damage *dmg);

/* context */
struct blt_context {
	const struct blt_context_impl *impl;
	int op;
	struct blt_image *dst, *src, *msk;
	int dst_x, dst_y, src_x, src_y, msk_x, msk_y;
	struct blt_x11 *x11;
	struct blt_wl *wl;
};

/* misc types */
struct blt_color {
	uint16_t red;
	uint16_t green;
	uint16_t blue;
	uint16_t alpha;
};

/* image */
struct blt_image {
	const struct blt_image_impl *impl;
	int width, height;
	uint32_t format;
};

enum {
	/* image can be used as a destination */
	BLT_IMAGE_DST = 1<<0,
	/* image can be used as a source */
	BLT_IMAGE_SRC = 1<<1,
};

struct blt_image *blt_new_image(struct blt_context *ctx, int x, int y, uint32_t format, int flags);
struct blt_image *blt_new_solid(struct blt_context *ctx, const struct blt_color *color);

void blt_image_destroy(struct blt_context *ctx, struct blt_image *img);

/* surface */
struct blt_surface;

void blt_surface_destroy(struct blt_context *ctx, struct blt_surface *srf);
struct blt_image *blt_acquire(struct blt_context *ctx, struct blt_surface *srf, int *age);
int blt_present(struct blt_context *ctx, struct blt_surface *srf, struct blt_image *img);

/* rendering */
enum blt_op {
	BLT_OP_SRC,
	BLT_OP_OVER,
};

int blt_setup(struct blt_context *ctx, int op,
              struct blt_image *dst, int dst_x, int dst_y,
              struct blt_image *src, int src_x, int src_y,
              struct blt_image *msk, int msk_x, int msk_y);
int blt_src(struct blt_context *ctx, struct blt_image *src, int src_x, int src_y);
int blt_dst(struct blt_context *ctx, struct blt_image *dst, int dst_x, int dst_y);
int blt_msk(struct blt_context *ctx, struct blt_image *msk, int msk_x, int msk_y);

int blt_rect(struct blt_context *ctx, size_t len, const struct pixman_box32 *rect);
int blt_region(struct blt_context *ctx, struct pixman_region32 *region);

#endif
