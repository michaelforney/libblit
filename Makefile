.POSIX:
.PHONY: all clean
.SUFFIXES: .glsl .spv .inc

all: libblit.a

include config.mk

CFLAGS-$(WITH_WAYLAND)+=-D WITH_WAYLAND
CFLAGS-$(WITH_X11)+=-D WITH_X11

OBJ-y=\
	blt.o\
	damage.o\
	drm.o\
	image.o\
	solid.o\
	surface.o\
	util.o
OBJ-$(WITH_WAYLAND)+=wl.o
OBJ-$(WITH_X11)+=x11.o

# vulkan
CFLAGS-$(WITH_VULKAN)+=-D WITH_VULKAN
CFLAGS-$(WITH_VULKAN_WAYLAND)+=-D WITH_VULKAN_WAYLAND
CFLAGS-$(WITH_VULKAN_X11)+=-D WITH_VULKAN_X11

OBJ-$(WITH_VULKAN)+=vulkan/impl.o
OBJ-$(WITH_VULKAN_WAYLAND)+=vulkan/wl.o
OBJ-$(WITH_VULKAN_X11)+=vulkan/x11.o

vulkan/impl.o vulkan/wl.o vulkan/x11.o: vulkan/priv.h
vulkan/impl.o: vulkan/vert.vert.inc vulkan/fill.frag.inc vulkan/copy.frag.inc

.glsl.spv:
	$(GLSLANG) --target-env vulkan1.1 -o $@ $<

.spv.inc:
	od -v -A n -t x4 $< | sed 's/[[:xdigit:]]\+/0x&,/g' >$@

CFLAGS+=-D _POSIX_C_SOURCE=200809L -I include $(CFLAGS-y)

$(OBJ-y): include/blt.h priv.h
wl.o vulkan/wl.o: include/blt-wl.h wl.h
x11.o vulkan/x11.o: include/blt-x11.h x11.h

libblit.a: $(OBJ-y)
	$(AR) cr $@ $(OBJ-y)

clean:
	rm -f libblit.a $(OBJ-y) $(EXAMPLES-y)
