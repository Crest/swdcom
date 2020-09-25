CSTD=c99
WARN=-Wall -Wextra -Wno-unknown-pragmas

CFLAGS?=-O2 -pipe
CFLAGS+=-std=$(CSTD)
CFLAGS+=$(WARN)
CFLAGS+=-Istlink/inc
CFLAGS+=-Istlink/build/Release/inc
CFLAGS+=-Istlink/src/stlink-lib
CFLAGS+=-D_XOPEN_SOURCE=500
LDFLAGS+=-lstlink
LDFLAGS+=stlink/build/Release/lib/libstlink.a -lusb

all: swd2

stlink/build/Release/lib/libstlink.a:
	[ -d stlink/.git ] || git submodule update --init
	make -C stlink

swd2: swd2.c stlink/build/Release/lib/libstlink.a
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f swd2
	make -C stlink clean
