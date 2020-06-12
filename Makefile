CC=gcc

CFLAGS+=-O0 -g
CFLAGS+=-I/usr/local/include
CFLAGS+=-L/usr/local/lib
LDFLAGS+=-lstlink

swd2: swd2.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f swd2
