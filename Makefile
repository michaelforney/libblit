.POSIX:
.PHONY: all clean
.SUFFIXES: .bin .glsl .inc .spv

all: libblit.a

include config.mk

BIN_TO_HEX=od -v -A n -t x4 $< | sed 's/[[:xdigit:]]\+/0x&,/g' >$@

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
	$(BIN_TO_HEX)

# amdgpu
CFLAGS-$(WITH_AMDGPU)+=-D WITH_AMDGPU

OBJ-$(WITH_AMDGPU)+=amdgpu/impl.o

AMDGPU_ASSEMBLE=$(LLVM_MC) --arch=amdgcn --mcpu=gfx1010 --assemble --filetype=obj $< | $(LLVM_OBJCOPY) -j .text -O binary - $@
AMDGPU_JSON=\
	amdgpu/amdgfxregs.json\
	amdgpu/pkt3.json\
	amdgpu/gfx10.json\
	amdgpu/gfx10-rsrc.json

amdgpu/amdgfxregs.h: amdgpu/makeregheader.py amdgpu/regdb.py $(AMDGPU_JSON)
	$(PYTHON) amdgpu/makeregheader.py --sort address --guard AMDGFXREGS_H $(AMDGPU_JSON) >$@.tmp
	mv $@.tmp $@

amdgpu/vert-gfx10.bin: amdgpu/vert-gfx10.s
	$(AMDGPU_ASSEMBLE)

amdgpu/fill-gfx10.bin: amdgpu/fill-gfx10.s
	$(AMDGPU_ASSEMBLE)

amdgpu/copy-gfx10.bin: amdgpu/copy-gfx10.s
	$(AMDGPU_ASSEMBLE)

amdgpu/impl.o: amdgpu/vert-gfx10.inc amdgpu/fill-gfx10.inc amdgpu/copy-gfx10.inc

CFLAGS+=-D _POSIX_C_SOURCE=200809L -I include $(CFLAGS-y)

$(OBJ-y): include/blt.h priv.h
wl.o vulkan/wl.o: include/blt-wl.h wl.h
x11.o vulkan/x11.o: include/blt-x11.h x11.h

.bin.inc:
	$(BIN_TO_HEX)

libblit.a: $(OBJ-y)
	$(AR) cr $@ $(OBJ-y)

clean:
	rm -f libblit.a $(OBJ-y) $(EXAMPLES-y)
