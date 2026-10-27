# Host-side check of the ARM load/store decoder/emulator, the way
# ../../tools checks the MIPS one: it compiles the real
# ../arm_mmio_emulate.c with the parent project's shim/ standing in for the
# kernel headers, so there is no second copy to drift.
#
#   make check
#
# Nothing here is cross-compiled and nothing here says anything about
# behaviour under a real data abort or about the target gcc's encodings.

CC ?= cc
CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -O2
INCLUDES := -I ../../tools/shim -I ..

all: check

test_arm_emulate: test_arm_emulate.c ../arm_mmio_emulate.c ../arm_mmio_emulate.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_arm_emulate.c ../arm_mmio_emulate.c

check: test_arm_emulate
	./test_arm_emulate

clean:
	rm -f test_arm_emulate

.PHONY: all check clean
