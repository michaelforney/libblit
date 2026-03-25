#include <stdlib.h>
#include <blt.h>
#include <xcb/xcb.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include "../priv.h"
#include "../x11.h"
#include "priv.h"

static struct blt_surface *
new_surface(struct blt_context *ctx, xcb_window_t win)
{
	VkInstance instance;
	VkResult res;
	VkSurfaceKHR vksrf;
	struct blt_surface *srf;

	instance = blt_vulkan_instance(ctx);
	res = vkCreateXcbSurfaceKHR(instance, &(VkXcbSurfaceCreateInfoKHR){
		.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
		.connection = ctx->x11->conn,
		.window = win,
	}, NULL, &vksrf);
	if (res != VK_SUCCESS)
		goto error0;
	srf = blt_vulkan_new_surface(ctx, vksrf, -1, -1, BLT_FMT('X', 'R', '2', '4'));
	if (!srf)
		goto error1;

	return srf;

error1:
	vkDestroySurfaceKHR(instance, vksrf, NULL);
error0:
	return NULL;
}

static const struct blt_x11_impl x11_impl = {
	.new_surface = new_surface,
};

struct blt_context *
blt_vulkan_x11_new(xcb_connection_t *conn)
{
	struct blt_context *ctx;
	struct blt_x11 *x11;

	x11 = malloc(sizeof(*x11));
	if (!x11)
		goto error0;
	x11->impl = &x11_impl;
	x11->conn = conn;
	ctx = blt_vulkan_new(0, BLT_VULKAN_X11);
	if (!ctx)
		goto error1;
	ctx->x11 = x11;
	return ctx;

error1:
	free(x11);
error0:
	return NULL;
}
