struct blt_wl_impl {
	struct blt_surface *(*new_surface)(struct blt_context *, struct wl_surface *, int, int);
};

struct blt_wl {
	const struct blt_wl_impl *impl;
};
