#include <xcb/xcb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <blt.h>
#include <blt-x11.h>
#include <pixman.h>

static void
fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

int
main(int argc, char *argv[])
{
	xcb_connection_t *conn;
	xcb_screen_t *screen;
	const xcb_setup_t *setup;
	xcb_window_t win;
	xcb_generic_event_t *event;
	struct blt_context *ctx;
	struct blt_surface *srf;
	struct blt_image *dst, *black, *red, *green, *blue;

	conn = xcb_connect(NULL, NULL);
	if (!conn)
		fatal("xcb_connect failed");
	setup = xcb_get_setup(conn);
	screen = xcb_setup_roots_iterator(setup).data;
	ctx = blt_x11_new(conn);
	if (!ctx)
		fatal("blt_x11_new failed");
	win = xcb_generate_id(conn);

	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0, 400, 400, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACKING_STORE, (uint32_t[]){XCB_BACKING_STORE_WHEN_MAPPED});
	xcb_map_window(conn, win);
	xcb_flush(conn);

	srf = blt_x11_new_surface(ctx, win);
	if (!srf)
		fatal("blt_x11_new_surface failed");
	black = blt_new_solid(ctx, (struct blt_color){.alpha = 0xffff});
	red = blt_new_solid(ctx, (struct blt_color){.red = 0x8888, .green = 0x3333, .blue = 0x3333, .alpha = 0xffff});
	green = blt_new_solid(ctx, (struct blt_color){.red = 0x3333, .green = 0x8888, .blue = 0x3333, .alpha = 0xffff});
	blue = blt_new_solid(ctx, (struct blt_color){.red = 0x3333, .green = 0x3333, .blue = 0x8888, .alpha = 0xffff});
	if (!black || !red || !green || !blue)
		fatal("blt_new_solid failed");

	dst = blt_acquire(ctx, srf, NULL);
	if (!dst)
		fatal("blt_acquire failed");
	blt_setup(ctx, BLT_OP_SRC, dst, 0, 0, black, 0, 0, NULL, 0, 0);
	blt_rect(ctx, 1, &(struct blt_rect){0, 0, 400, 400});
	blt_src(ctx, red, 0, 0);
	blt_rect(ctx, 1, &(struct blt_rect){50, 50, 350, 350});
	blt_src(ctx, blue, 0, 0);
	blt_rect(ctx, 1, &(struct blt_rect){0, 0, 100, 200});
	blt_dst(ctx, NULL, 0, 0);
	blt_present(ctx, srf, dst);

	sleep(1);

	dst = blt_acquire(ctx, srf, NULL);

	for (;;) {
		event = xcb_wait_for_event(conn);
		if (!event)
			break;
		if (event->response_type == 0) {
			xcb_generic_error_t *error = (void *)event;
			printf("x11 error, code %d, major %d, minor %d\n", error->error_code, error->major_code, error->minor_code);
		}
		free(event);
	}
}
