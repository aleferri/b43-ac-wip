# Host-side checks of the units that are pure logic: the instruction
# encoders, the load/store decoder/emulator and the record ring. They
# compile the REAL files from the parent directory, with shim/ standing in
# for the handful of kernel headers those files include, so there is no
# second copy to drift.
#
#   make check
#
# Nothing here is cross-compiled and nothing here says anything about
# behaviour under a real exception; see each test's own header for what it
# does and does not cover.

CC ?= cc
CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -O2
INCLUDES := -I shim -I ..

all: check

test_opcodes: test_opcodes.c ../mips_opcode.c ../mips_opcode.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_opcodes.c ../mips_opcode.c

test_emulate: test_emulate.c ../mips_mmio_emulate.c ../mips_mmio_emulate.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_emulate.c ../mips_mmio_emulate.c

test_ring: test_ring.c ../wl_ring.c ../wl_ring.h
	$(CC) $(CFLAGS) $(INCLUDES) -pthread -o $@ test_ring.c ../wl_ring.c

check: test_opcodes test_emulate test_ring
	./test_opcodes
	@echo
	./test_emulate
	@echo
	./test_ring

clean:
	rm -f test_opcodes test_emulate test_ring

.PHONY: all check clean
