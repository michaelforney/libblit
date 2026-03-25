#include <sys/types.h>

enum {
	BLT_VULKAN_X11      = 1<<0,
	BLT_VULKAN_WAYLAND  = 1<<1,
};

VkInstance blt_vulkan_instance(struct blt_context *);

struct blt_context *blt_vulkan_new(dev_t dev, int flags);
struct blt_surface *blt_vulkan_new_surface(struct blt_context *, VkSurfaceKHR, int, int, uint32_t);
