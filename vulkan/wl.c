#include <stdlib.h>
#include <blt.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include "../priv.h"
#include "../wl.h"
#include "priv.h"

static struct blt_surface *
new_surface(struct blt_context *ctx, struct wl_surface *wlsrf, int width, int height)
{
	VkInstance instance;
	VkResult res;
	VkSurfaceKHR vksrf;
	struct blt_surface *srf;

	instance = blt_vulkan_instance(ctx);
	res = vkCreateWaylandSurfaceKHR(instance, &(VkWaylandSurfaceCreateInfoKHR){
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = ctx->wl->dpy,
		.surface = wlsrf,
	}, NULL, &vksrf);
	if (res != VK_SUCCESS)
		goto error0;
	srf = blt_vulkan_new_surface(ctx, vksrf, width, height, BLT_FMT('X', 'R', '2', '4'));
	if (!srf)
		goto error1;

	return srf;

error1:
	vkDestroySurfaceKHR(instance, vksrf, NULL);
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
	struct blt_wl *wl;

	wl = malloc(sizeof(*wl));
	if (!wl)
		goto error0;
	wl->impl = &wl_impl;
	wl->dpy = dpy;
	ctx = blt_vulkan_new(0, BLT_VULKAN_WAYLAND);
	if (!ctx)
		goto error1;
	ctx->wl = wl;
	return ctx;

error1:
	free(wl);
error0:
	return NULL;
}
