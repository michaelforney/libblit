#include <pixman.h>
#include "blt.h"
#include "priv.h"

struct blt_image *
blt_new_image(struct blt_context *ctx, int width, int height, uint32_t format, int flags)
{
	return ctx->impl->new_image(ctx, width, height, format, flags);
}

struct blt_image *
blt_new_solid(struct blt_context *ctx, const struct blt_color *color)
{
	return ctx->impl->new_solid(ctx, color);
}

struct blt_image *
blt_acquire(struct blt_context *ctx, struct blt_surface *srf, int *age)
{
	return srf->impl->acquire(ctx, srf, age);
}

int
blt_present(struct blt_context *ctx, struct blt_surface *srf, struct blt_image *img)
{
	return srf->impl->present(ctx, srf, img);
}

int
blt_setup(struct blt_context *ctx, int op,
          struct blt_image *dst, int dst_x, int dst_y,
          struct blt_image *src, int src_x, int src_y,
          struct blt_image *msk, int msk_x, int msk_y)
{
	if (ctx->impl->setup(ctx, op, dst, src, msk) < 0)
		return -1;
	ctx->dst = dst;
	ctx->dst_x = dst_x;
	ctx->dst_y = dst_y;
	ctx->src = src;
	ctx->src_x = src_x;
	ctx->src_y = src_y;
	ctx->msk = msk;
	ctx->msk_x = msk_x;
	ctx->msk_y = msk_y;
	return 0;
}

int
blt_op(struct blt_context *ctx, int op)
{
	if (ctx->impl->setup(ctx, op, ctx->dst, ctx->src, ctx->msk) < 0)
		return -1;
	ctx->op = op;
	return 0;
}

int
blt_dst(struct blt_context *ctx, struct blt_image *dst, int x, int y)
{
	if (ctx->impl->setup(ctx, ctx->op, dst, ctx->src, ctx->msk) < 0)
		return -1;
	ctx->dst = dst;
	ctx->dst_x = x;
	ctx->dst_y = y;
	return 0;
}

int
blt_src(struct blt_context *ctx, struct blt_image *src, int x, int y)
{
	if (ctx->impl->setup(ctx, ctx->op, ctx->dst, src, ctx->msk) < 0)
		return -1;
	ctx->src = src;
	ctx->src_x = x;
	ctx->src_y = y;
	return 0;
}

int
blt_mask(struct blt_context *ctx, struct blt_image *msk, int x, int y)
{
	if (ctx->impl->setup(ctx, ctx->op, ctx->dst, ctx->src, msk) < 0)
		return -1;
	ctx->msk = msk;
	ctx->msk_x = x;
	ctx->msk_y = y;
	return 0;
}

int
blt_rect(struct blt_context *ctx, size_t len, const struct pixman_box32 *rect)
{
	return ctx->impl->rect(ctx, len, rect);
}

int
blt_region(struct blt_context *ctx, struct pixman_region32 *region)
{
	int len;
	struct pixman_box32 *box;

	box = pixman_region32_rectangles(region, &len);
	return blt_rect(ctx, len, box);
}
