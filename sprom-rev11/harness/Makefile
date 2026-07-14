# Makefile for the bcma_sprom_extract_r11 offline harness.
#
# Build:   make
# Run:     make check       (or ./test)
# Clean:   make clean

CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
LDFLAGS ?=

OBJS = extract_r11.o data_load.o synth_srom.o test.o

test: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

extract_r11.o: extract_r11.c kernel_shim.h ssb_sprom.h ssb_regs.h
data_load.o:   data_load.c   data_load.h   kernel_shim.h
synth_srom.o:  synth_srom.c  synth_srom.h  data_load.h ssb_regs.h kernel_shim.h
test.o:        test.c        data_load.h   ssb_sprom.h synth_srom.h

.PHONY: check check-bcm4360usb check-d6220 check-agcombo clean
check: test
	./test

# Second test vector: BCM4360 USB rev-11 reference NVRAM (no raw SROM
# available — runs the synth-based round-trip described in the patch
# series cover letter and in synth_srom.c).
check-bcm4360usb: test
	./test --synth vectors/bcm4360usb.nvram

# Third test vector: Netgear D6220 wl1 dumps. Same chip family as the
# DSL-3580L (BCM43b3, rev-11 SROM) but distinct boardrev, PA tables
# and asymmetric maxp5ga0. Hardware-real srdump+nvram_dump pair, not
# synthetic.
check-d6220: test
	./test ../../../router-data/d6220/wl1_srom_raw.txt \
	       ../../../router-data/d6220/wl1_nvram.txt

# Fourth test vector: agcombo (BCM4360 reference, 3x3, dual-band).
# srdump returns zero on this blob (OEM omits the raw path), so the
# vector runs in --synth mode against agcombo_nvram.txt only. Exercises
# fields the BCM43b3 boards leave dormant: 2.4 GHz PA chain, aa2g=7,
# chain 2 rxgains.
check-agcombo: test
	./test --synth ../../../router-data/agcombo/agcombo_nvram.txt

clean:
	rm -f $(OBJS) test
