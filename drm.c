#include <blt.h>
#include "priv.h"

struct blt_context *
blt_drm_new(int fd)
{
	static struct blt_context *(*const impls[])(int) = {
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
