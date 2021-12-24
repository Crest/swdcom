USB_CFLAGS!=pkg-config --cflags libusb-1.0
USB_LDFLAGS!=pkg-config --libs libusb-1.0

CSTD=c99
WARN=-Wall -Wextra -Wno-unknown-pragmas
WARN=-Weverything

CFLAGS?=-O2 -pipe
CFLAGS+=-std=$(CSTD)
CFLAGS+=$(WARN)
CFLAGS+=-Istlink/inc
CFLAGS+=-Istlink/build/Release/inc
CFLAGS+=-Istlink/src/stlink-lib
CFLAGS+=$(USB_CFLAGS)

LDFLAGS+=stlink/build/Release/lib/libstlink.a
LDFLAGS+=$(USB_LDFLAGS)

all: swd2 # swdd

stlink/build/Release/lib/libstlink.a:
	[ -d stlink/.git ] || git submodule update --init
	make -C stlink

swd2: swd2.c stlink/build/Release/lib/libstlink.a
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

swdd: swdd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread


.PHONY: clean
clean:
	rm -f swd2 swdd
	make -C stlink clean
