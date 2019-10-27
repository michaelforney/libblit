#ifndef BLT_WL_H
#define BLT_WL_H

struct wl_display;
struct wl_surface;

struct blt_context *blt_wl_new(struct wl_display *dpy);
struct blt_surface *blt_wl_new_surface(struct blt_context *ctx, struct wl_surface *srf, int width, int height);

#endif
