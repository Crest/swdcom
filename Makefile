CC?=clang
CSTD=c99
WARN=-Weverything # Excessive

CFLAGS+=-std=$(CSTD)
CFLAGS+=$(WARN)
CFLAGS+=-I/usr/local/include
CFLAGS+=-L/usr/local/lib
LDFLAGS+=-lstlink-shared

swd2: swd2.c
	$(CC) $(CFLAGS) -o $(.TARGET) $(.ALLSRC) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f swd2
