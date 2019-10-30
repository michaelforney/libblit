#include <blt.h>
#include "priv.h"

void
blt_surface_destroy(struct blt_context *ctx, struct blt_surface *srf)
{
	return srf->impl->destroy(ctx, srf);
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
