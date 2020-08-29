CSTD=c99
WARN=-Wall -Wextra -Wno-unknown-pragmas
WARN=-Weverything

CFLAGS?=-O2 -pipe
CFLAGS+=-std=$(CSTD)
CFLAGS+=$(WARN)
CFLAGS+=-Istlink/inc
CFLAGS+=-Istlink/build/Release/inc
CFLAGS+=-Istlink/src/stlink-lib
#CFLAGS+=-D_XOPEN_SOURCE=500
LDFLAGS+=stlink/build/Release/lib/libstlink.a -lusb

all: swd2 # swdd

stlink/build/Release/lib/libstlink.a:
	make -C stlink

swd2: swd2.c stlink/build/Release/lib/libstlink.a
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

swdd: swdd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread


.PHONY: clean
clean:
	rm -f swd2 swdd
	make -C stlink clean
