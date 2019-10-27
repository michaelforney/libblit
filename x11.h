struct blt_x11_impl {
	struct blt_surface *(*new_surface)(struct blt_context *, xcb_window_t);
};

struct blt_x11 {
	const struct blt_x11_impl *impl;
};
