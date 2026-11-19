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
# SMALL turns off optimizations that make these programs bigger, not faster where it matters:
# about 200 bytes off the full build (a 1K block on MINIX) and 48 off tiny ept

SMALL = -mregparmcall -fno-inline-small-functions -fno-caller-saves -fno-reorder-blocks -fno-optimize-sibling-calls
TINYFLAGS =
tiny/tinyept: tiny/tinyept.c
	$(CC) $(CFLAGS) $(SMALL) $(TINYFLAGS) -maout-heap=256 -maout-stack=2048 -o $@ tiny/tinyept.c

# eptdev.c: the next full ept, built up from tiny ept; EPTDEVFLAGS=-DTINY
# builds it as tiny ept
EPTDEVFLAGS =
# the full one nests its line reader, so it needs more stack than tiny ept
EPTDEVSTACK = $(if $(findstring -DTINY,$(EPTDEVFLAGS)),2048,4096)
eptdev: eptdev.c
	$(CC) $(CFLAGS) $(SMALL) $(TINYFLAGS) $(EPTDEVFLAGS) -maout-heap=256 -maout-stack=$(EPTDEVSTACK) -o $@ eptdev.c

clean:
	rm -f ept tiny/tinyept eptdev
