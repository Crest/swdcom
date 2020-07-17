CSTD=c99
WARN=-Wall -Wextra -Wno-unknown-pragmas
WARN=-Weverything

CFLAGS?=-O2 -pipe
CFLAGS+=-std=$(CSTD)
CFLAGS+=$(WARN)
CFLAGS+=-I/usr/local/include
CFLAGS+=-I/usr/local/include/stlink
CFLAGS+=-L/usr/local/lib
#CFLAGS+=-D_XOPEN_SOURCE=500
LDFLAGS+=-lstlink

all: swd2 swdd

swd2: swd2.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

swdd: swdd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread


.PHONY: clean
clean:
	rm -f swd2 swdd
