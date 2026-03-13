#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pixman.h>
#include <wayland-client.h>
#include <blt.h>
#include <blt-wl.h>

static struct wl_compositor *compositor;
static struct wl_shell *shell;

static void
global(void *d, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, "wl_compositor") == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
	else if (strcmp(interface, "wl_shell") == 0)
		shell = wl_registry_bind(r, name, &wl_shell_interface, 1);
}

static void
global_remove(void *d, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = global,
	.global_remove = global_remove,
};

static void
fatal(const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	fprintf(stderr, fmt, args);
	va_end(args);
	if (fmt[0] && fmt[strlen(fmt) - 1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}
	exit(1);
}

int
main(void)
{
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_surface *surface;
	struct wl_shell_surface *shell_surface;
	struct blt_context *ctx;
	struct blt_surface *srf;
	struct blt_image *img, *black, *red, *green;

	dpy = wl_display_connect(NULL);
	if (!dpy)
		fatal("wl_display_connect:");
	reg = wl_display_get_registry(dpy);
	if (!reg)
		fatal("wl_display_get_registry:");
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor)
		fatal("no wl_compositor");
	if (!shell)
		fatal("no wl_shell");
	surface = wl_compositor_create_surface(compositor);
	if (!surface)
		fatal("wl_compositor_create_surface:");
	shell_surface = wl_shell_get_shell_surface(shell, surface);
	if (!shell_surface)
		fatal("wl_shell_get_shell_surface:");
	wl_shell_surface_set_toplevel(shell_surface);

	ctx = blt_wl_new(dpy);
	if (!ctx)
		fatal("blt_new:");
	srf = blt_wl_new_surface(ctx, surface, 400, 400);
	if (!srf)
		fatal("blt_new_surface_wl:");
	black = blt_new_solid(ctx, (struct blt_color){0x0000, 0x0000, 0x0000, 0xffff});
	red = blt_new_solid(ctx, (struct blt_color){0x8888, 0x3333, 0x3333, 0xffff});
	green = blt_new_solid(ctx, (struct blt_color){0x3333, 0x8888, 0x3333, 0xffff});
	if (!black || !red || !green)
		fatal("blt_new_color:");

	img = blt_acquire(ctx, srf, NULL);
	if (!img)
		fatal("blt_acquire:");
	blt_setup(ctx, BLT_OP_SRC, img, 0, 0, black, 0, 0, NULL, 0, 0);
	blt_rect(ctx, 1, &(struct blt_rect){0, 0, 400, 400});
	blt_src(ctx, red, 0, 0);
	blt_rect(ctx, 1, &(struct blt_rect){50, 50, 350, 350});
	blt_src(ctx, green, 0, 0);
	blt_rect(ctx, 1, &(struct blt_rect){0, 0, 100, 200});
	blt_dst(ctx, NULL, 0, 0);
	blt_present(ctx, srf, img);
	wl_display_flush(dpy);

	pause();
}
