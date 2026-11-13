# Build ept for ELKS with the ia16-elf-gcc cross compiler.
# CC must find the ELKS libc; override it if your toolchain lives elsewhere.

CC = ia16-elf-gcc
CFLAGS = -mcmodel=small -melks-libc -mtune=i8086 -Wall -Os
# stdio buffers for up to four open files come from the heap
LDFLAGS = -maout-heap=8192 -maout-stack=4096

ept: ept.c md5.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ept.c

clean:
	rm -f ept
