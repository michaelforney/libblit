.POSIX:
.PHONY: all clean

all: libblit.a

include config.mk

CFLAGS-$(WITH_WAYLAND)+=-D WITH_WAYLAND
CFLAGS-$(WITH_X11)+=-D WITH_X11

CFLAGS+=-D _POSIX_C_SOURCE=200809L -I include $(CFLAGS-y)

OBJ-y=\
	blt.o\
	solid.o\
	damage.o\
	util.o
OBJ-$(WITH_WAYLAND)+=wl.o
OBJ-$(WITH_X11)+=x11.o


$(OBJ-y): include/blt.h include/blt-wl.h include/blt-x11.h priv.h

libblit.a: $(OBJ-y)
	$(AR) cr $@ $(OBJ-y)

clean:
	rm -f libblit.a $(OBJ-y) $(EXAMPLES-y)

# vulkan
.SUFFIXES: .glsl .spv

.glsl.spv:
	$(GLSLANG) --target-env vulkan1.1 -o $@ $<
