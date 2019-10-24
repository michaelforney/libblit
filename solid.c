#include <limits.h>
#include <stdlib.h>
#include <blt.h>
#include "priv.h"

static void
destroy(struct blt_context *ctx, struct blt_image *img)
{
	free(img);
}

const struct blt_image_impl blt_solid_image_impl = {
	.destroy = destroy,
};

struct blt_image *
blt_new_solid_image(struct blt_context *ctx, const struct blt_color *color)
{
	struct blt_solid *img;

	img = malloc(sizeof(*img));
	if (!img)
		return NULL;
	img->base = (struct blt_image){
		.impl = &blt_solid_image_impl,
		.width = INT_MAX,
		.height = INT_MAX,
	};
	img->color = *color;
	return &img->base;
}
