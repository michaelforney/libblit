.POSIX:
.PHONY: all clean

all: libblit.a

include config.mk

CFLAGS-$(WITH_WAYLAND)+=-D WITH_WAYLAND
CFLAGS-$(WITH_X11)+=-D WITH_X11
CFLAGS-$(WITH_VULKAN)+=-D WITH_VULKAN
CFLAGS-$(WITH_VULKAN_WAYLAND)+=-D WITH_VULKAN_WAYLAND
CFLAGS-$(WITH_VULKAN_X11)+=-D WITH_VULKAN_X11

CFLAGS+=-D _POSIX_C_SOURCE=200809L -I include $(CFLAGS-y)

OBJ-y=\
	blt.o\
	solid.o\
	damage.o\
	util.o
OBJ-$(WITH_WAYLAND)+=wl.o
OBJ-$(WITH_X11)+=x11.o
OBJ-$(WITH_VULKAN)+=vulkan/impl.o
OBJ-$(WITH_VULKAN_WAYLAND)+=vulkan/wl.o
OBJ-$(WITH_VULKAN_X11)+=vulkan/x11.o

$(OBJ-y): include/blt.h include/blt-wl.h include/blt-x11.h priv.h
vulkan/impl.o: vulkan/vert.vert.inc vulkan/fill.frag.inc vulkan/copy.frag.inc

libblit.a: $(OBJ-y)
	$(AR) cr $@ $(OBJ-y)

clean:
	rm -f libblit.a $(OBJ-y) $(EXAMPLES-y)

# vulkan
.SUFFIXES: .glsl .spv .inc

.glsl.spv:
	$(GLSLANG) --target-env vulkan1.1 -o $@ $<

.spv.inc:
	od -v -An -t x4 $< | sed 's/[[:xdigit:]]\+/0x&,/g' >$@
