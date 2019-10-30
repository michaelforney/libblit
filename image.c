#include <blt.h>
#include "priv.h"

void
blt_image_destroy(struct blt_context *ctx, struct blt_image *img)
{
	img->impl->destroy(ctx, img);
}
