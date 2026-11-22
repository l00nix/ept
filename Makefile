# Build ept for ELKS with the ia16-elf-gcc cross compiler.
# CC must find the ELKS libc; override it if your toolchain lives elsewhere.
# Both builds come from ept.c: "make" is the full ept, "make tinyept" the
# tiny one for floppy systems (ept.c built with -DTINY).

CC = ia16-elf-gcc
CFLAGS = -mcmodel=small -melks-libc -mtune=i8086 -Wall -Os
# -mregparmcall passes arguments in registers and links the matching libc and saves about 10%
# the others turn off optimizations that make these programs bigger, not faster where it matters:
# about 200 bytes off the full build (a 1K block on MINIX) and 48 off tiny ept
SMALL = -mregparmcall -fno-inline-small-functions -fno-caller-saves -fno-reorder-blocks -fno-optimize-sibling-calls
# ELKS libc from before in_resolv() took three arguments needs TINYFLAGS=-DOLD_RESOLV
TINYFLAGS =

# the full one nests its line reader, so it needs more stack than tiny ept
ept: ept.c
	$(CC) $(CFLAGS) $(SMALL) $(TINYFLAGS) -maout-heap=256 -maout-stack=4096 -o $@ ept.c

tinyept: ept.c
	$(CC) $(CFLAGS) $(SMALL) $(TINYFLAGS) -DTINY -maout-heap=256 -maout-stack=2048 -o $@ ept.c

clean:
	rm -f ept tinyept

.PHONY: clean
