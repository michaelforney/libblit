#ifndef BLT_X11_H
#define BLT_X11_H

#include <xcb/xcb.h>

struct blt_context *blt_x11_new(xcb_connection_t *);
struct blt_surface *blt_x11_new_surface(struct blt_context *, xcb_window_t);

#endif
