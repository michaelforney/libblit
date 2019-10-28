#include <wayland-client.h>
#include <blt.h>
#include "priv.h"
#include "wl.h"

#ifdef WITH_VULKAN_WAYLAND
struct blt_context *blt_vulkan_wl_new(struct wl_display *);
#endif

struct blt_context *
blt_wl_new(struct wl_display *dpy)
{
	static struct blt_context *(*const impls[])(struct wl_display *) = {
#ifdef WITH_VULKAN_WAYLAND
		blt_vulkan_wl_new,
#endif
		0,
	};
	struct blt_context *ctx;
	size_t i;

	for (i = 0; i < LEN(impls) - 1; ++i) {
		ctx = impls[i](dpy);
		if (ctx)
			return ctx;
	}
	return NULL;
}

struct blt_surface *
blt_wl_new_surface(struct blt_context *ctx, struct wl_surface *srf, int width, int height)
{
	return ctx->wl ? ctx->wl->impl->new_surface(ctx, srf, width, height) : NULL;
}
