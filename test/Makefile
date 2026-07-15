# SPDX-License-Identifier: GPL-2.0
#
# Userspace build of the b43 AC-PHY driver, with a linker-time
# --wrap shim (wrap.c) that replaces every low-level HW accessor with a
# tracer that emits wl-diag-style lines on stdout. No source file under
# repository root is modified.
#
# Targets:
#   make          build the trace binary
#   make trace    run it (D6220 rxiq_est_debug) and dump to trace.out
#   make clean

SCRATCH := ../src
STUBS   := stubs

# We include our stubs FIRST (via -I) so kernel headers <linux/*.h> and
# the b43 project headers resolve to our minimal versions. The scratch
# source directory comes next so its own headers ("b43.h" already
# resolved above; "phy_ac.h" etc. live there).
CFLAGS  := -O0 -g -Wall -Wno-unused-parameter -Wno-unused-function \
           -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-pointer-sign -Wno-sign-compare \
           -I$(STUBS) -I$(SCRATCH) \
           -include stubs/linux/types.h \
           -include stubs/linux/errno.h \
           -include stubs/linux/delay.h

# Every symbol below is replaced at link time: callers see __wrap_<sym>,
# and __real_<sym> resolves to the original implementation (only useful
# for actab_* which we compile from tables_phy_ac.c).
WRAP_SYMS := \
    b43_phy_read b43_phy_write b43_phy_mask b43_phy_set \
    b43_phy_maskset b43_phy_force_clock \
    b43_radio_read b43_radio_write b43_radio_mask b43_radio_set \
    b43_radio_maskset \
    b43_read16 b43_write16 b43_write16f \
    b43_mac_enable b43_mac_suspend b43_mac_suspend_enable \
    b43_maccontrol_set \
    b43_mac_phy_clock_set b43_phy_ac_mhf_maskset \
    b43_phyop_switch_analog_generic \
    b43_current_band b43_phy_init \
    b43_actab_read_bulk b43_actab_write_bulk b43_actab_write_bulk_reopen \
    b43_actab_write_bulk_scoped \
    b43_actab_write_r11

LDFLAGS := $(foreach s,$(WRAP_SYMS),-Wl,--wrap=$(s))

# --- Source lists --------------------------------------------------------
# Start small: rxiqcal_phy_ac + tables_phy_ac are self-contained and
# were verified in the previous work. Extend WITH_* lines when adding
# more of the driver.
SCRATCH_SRCS_MIN := \
    $(SCRATCH)/rxiqcal_phy_ac.c \
    $(SCRATCH)/tables_phy_ac.c

# Full driver: the scratch phy_ac.c + radio_2069.c on top of the minimal set.
# This is the set actually built (see SRCS below).
SCRATCH_SRCS_FULL := \
    $(SCRATCH_SRCS_MIN) \
    $(SCRATCH)/phy_ac.c \
    $(SCRATCH)/radio_2069.c

SRCS := $(SCRATCH_SRCS_FULL) wrap.c main.c
OBJS := $(patsubst %.c,build/%.o,$(notdir $(SRCS)))

vpath %.c $(SCRATCH):.

BIN := rxiq_trace

all: $(BIN)

build:
	mkdir -p build

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

trace: $(BIN)
	./$(BIN) rxiq_est_debug d6220 > trace.d6220.out
	@echo "wrote trace.d6220.out ($$(wc -l < trace.d6220.out) lines)"

clean:
	rm -rf build $(BIN) trace.*.out

.PHONY: all clean trace
