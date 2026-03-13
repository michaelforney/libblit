#include <blt.h>
#include "priv.h"

#ifdef WITH_VULKAN
struct blt_context *blt_vulkan_drm_new(int);
#endif
#ifdef WITH_AMDGPU
struct blt_context *blt_amdgpu_new(int);
#endif

struct blt_context *
blt_drm_new(int fd)
{
	static struct blt_context *(*const impls[])(int) = {
#ifdef WITH_VULKAN
		blt_vulkan_drm_new,
#endif
#ifdef WITH_AMDGPU
		blt_amdgpu_new,
#endif
		0,
	};
	struct blt_context *ctx;
	size_t i;

	for (i = 0; i < LEN(impls) - 1; ++i) {
		ctx = impls[0](fd);
		if (ctx)
			return ctx;
	}
	return NULL;
}
