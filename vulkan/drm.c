#include <sys/stat.h>
#include <blt.h>
#include <vulkan/vulkan.h>
#include "../priv.h"
#include "priv.h"

struct blt_context *
blt_vulkan_drm_new(int fd)
{
	struct stat st;

	if (fstat(fd, &st) != 0)
		return NULL;
	return blt_vulkan_new(st.st_rdev, 0);
}
