# Build ept for ELKS with the ia16-elf-gcc cross compiler.
# CC must find the ELKS libc; override it if your toolchain lives elsewhere.

CC = ia16-elf-gcc
CFLAGS = -mcmodel=small -melks-libc -mtune=i8086 -Wall -Os
# stdio buffers for up to four open files come from the heap
LDFLAGS = -maout-heap=8192 -maout-stack=4096

ept: ept.c md5.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ept.c

# tiny ept for floppy images, see tiny/tinyept.c.
# ELKS libc from before in_resolv() took three arguments needs TINYFLAGS=-DOLD_RESOLV.
# -mregparmcall passes arguments in registers and links the matching libc and saves about 10%

TINYFLAGS =
tiny/tinyept: tiny/tinyept.c
	$(CC) $(CFLAGS) -mregparmcall $(TINYFLAGS) -maout-heap=256 -maout-stack=2048 -o $@ tiny/tinyept.c

# eptdev.c: the next full ept, built up from tiny ept; EPTDEVFLAGS=-DTINY
# builds it as tiny ept
EPTDEVFLAGS =
eptdev: eptdev.c
	$(CC) $(CFLAGS) -mregparmcall $(TINYFLAGS) $(EPTDEVFLAGS) -maout-heap=256 -maout-stack=2048 -o $@ eptdev.c

clean:
	rm -f ept tiny/tinyept eptdev
