#include <xcb/xcb.h>
#include <blt.h>
#include <blt-x11.h>
#include "priv.h"
#include "x11.h"

#ifdef WITH_VULKAN_X11
struct blt_context *blt_vulkan_x11_new(xcb_connection_t *);
#endif

struct blt_context *
blt_x11_new(xcb_connection_t *conn)
{
	static struct blt_context *(*const impls[])(xcb_connection_t *) = {
#ifdef WITH_VULKAN_X11
		blt_vulkan_x11_new,
#endif
		0,
	};
	struct blt_context *ctx;
	size_t i;

	for (i = 0; i < LEN(impls) - 1; ++i) {
		ctx = impls[i](conn);
		if (ctx)
			return ctx;
	}
	return NULL;
}

struct blt_surface *
blt_x11_new_surface(struct blt_context *ctx, xcb_window_t win)
{
	return ctx->x11 ? ctx->x11->impl->new_surface(ctx, win) : NULL;
}
