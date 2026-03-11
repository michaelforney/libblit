#include <blt.h>
#include "priv.h"

void
blt_image_destroy(struct blt_context *ctx, struct blt_image *img)
{
	struct blt_userdata *data;

	while (img->data) {
		data = img->data;
		img->data = data->next;
		data->destroy(data);
	}
	img->impl->destroy(ctx, img);
}

void
blt_image_add_userdata(struct blt_image *img, struct blt_userdata *data)
{
	data->next = img->data;
	img->data = data;
}

struct blt_userdata *
blt_image_get_userdata(struct blt_image *img, void destroy(struct blt_userdata *))
{
	struct blt_userdata *data;

	for (data = img->data; data; data = data->next) {
		if (data->destroy == destroy)
			return data;
	}
	return NULL;
}

int
blt_image_export_dmabuf(struct blt_context *ctx, struct blt_image *img, struct blt_plane plane[static 4], uint64_t *mod)
{
	return img->impl->export_dmabuf(ctx, img, plane, mod);
}
