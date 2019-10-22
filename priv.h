#include <stdint.h>

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

void *reallocarray(void *, size_t, size_t);

struct blt_context_impl {
	struct blt_image *(*new_image)(struct blt_context *, int, int, uint32_t, int);
	struct blt_image *(*new_solid)(struct blt_context *, const struct blt_color *);

	int (*setup)(struct blt_context *, int, struct blt_image *, struct blt_image *, struct blt_image *);
	int (*rect)(struct blt_context *, size_t, const struct pixman_box32 *);
	int (*region)(struct blt_context *, struct pixman_region32 *);
};

struct blt_image_impl {
	void (*destroy)(struct blt_image *);
};

struct blt_surface_impl {
	struct blt_image *(*acquire)(struct blt_context *, struct blt_surface *, int *);
	int (*present)(struct blt_context *, struct blt_surface *, struct blt_image *);
};

struct blt_surface {
	const struct blt_surface_impl *impl;
};
