#include <stdlib.h>
#include <blt.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include "../priv.h"
#include "../wl.h"
#include "priv.h"

struct wl {
	struct blt_wl base;
	struct wl_display *dpy;
};

static struct blt_surface *
new_surface(struct blt_context *ctx_base, struct wl_surface *wlsrf, int width, int height)
{
	struct context *ctx = (void *)ctx_base;
	struct wl *wl = (void *)ctx->base.wl;
	VkResult res;
	VkSurfaceKHR vksrf;
	struct blt_surface *srf;

	res = vkCreateWaylandSurfaceKHR(ctx->instance, &(VkWaylandSurfaceCreateInfoKHR){
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = wl->dpy,
		.surface = wlsrf,
	}, NULL, &vksrf);
	if (res != VK_SUCCESS)
		goto error0;
	srf = blt_vulkan_new_surface(ctx, vksrf, width, height, BLT_FMT('X', 'R', '2', '4'));
	if (!srf)
		goto error1;

	return srf;

error1:
	vkDestroySurfaceKHR(ctx->instance, vksrf, NULL);
error0:
	return NULL;
}

static const struct blt_wl_impl wl_impl = {
	.new_surface = new_surface,
};

struct blt_context *
blt_vulkan_wl_new(struct wl_display *dpy)
{
	struct blt_context *ctx;
	struct wl *wl;

	wl = malloc(sizeof(*wl));
	if (!wl)
		goto error0;
	wl->base = (struct blt_wl){.impl = &wl_impl};
	wl->dpy = dpy;
	ctx = blt_vulkan_new(BLT_VULKAN_WAYLAND);
	if (!ctx)
		goto error1;
	ctx->wl = &wl->base;
	return ctx;

error1:
	free(wl);
error0:
	return NULL;
}
