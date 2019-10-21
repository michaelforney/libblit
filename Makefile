.POSIX:

all: libblit.a

include config.mk

CFLAGS+=-D _POSIX_C_SOURCE=200809L -I include

OBJ=\
	blt.o\
	damage.o\
	util.o

$(OBJ): include/blt.h priv.h

libblit.a: $(OBJ)
	$(AR) cr $@ $(OBJ)

clean:
	rm -f libblit.a $(OBJ)

.PHONY: all clean
