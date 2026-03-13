#include <blt.h>
#include <vulkan/vulkan.h>
#include "../priv.h"
#include "priv.h"

struct blt_context *
blt_vulkan_drm_new(int fd)
{
	return blt_vulkan_new(fd, 0);
}
