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

# The AC-PHY files of ../../b43 are copied into build/b43 and compiled from
# there. A quoted #include looks in the including file's own directory first,
# and ../../b43 also holds the real b43.h, main.h and phy_common.h, which
# would win over the stubs without a word.
B43     := ../../b43
SCRATCH := build/b43
STUBS   := stubs
AC_HDRS := $(addprefix $(SCRATCH)/,phy_ac.h tables_phy_ac.h radio_2069.h \
            ppr_ac.h)

# We include our stubs FIRST (via -I) so kernel headers <linux/*.h> and
# the b43 project headers resolve to our minimal versions. The copied AC-PHY
# directory comes next for its own headers ("phy_ac.h" etc.).
CFLAGS  := -O0 -g -Wall -Wno-unused-parameter -Wno-unused-function \
           -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-pointer-sign -Wno-sign-compare \
           -DB43_AC_FN_TRACE -DALLOW_24=true -DUNIT_TEST=1 \
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
    b43_maccontrol_set b43_mac_bw_set \
    b43_mac_phy_clock_set b43_phy_ac_mhf_maskset \
    b43_phyop_switch_analog_generic \
    b43_current_band b43_phy_init \
    b43_actab_read_bulk b43_actab_write_bulk b43_actab_write_bulk_reopen \
    b43_actab_write_bulk_scoped b43_actab_zerofill \
    b43_actab_write_bulk_locked b43_actab_zerofill_locked \
    b43_actab_write_r11 \
    b43_actab_fill_r11

LDFLAGS := $(foreach s,$(WRAP_SYMS),-Wl,--wrap=$(s))

# --- Source lists --------------------------------------------------------
# The AC-PHY files of the driver, copied into $(SCRATCH).
SCRATCH_SRCS_FULL := \
    $(SCRATCH)/tables_phy_ac.c \
    $(SCRATCH)/phy_ac.c \
    $(SCRATCH)/radio_2069.c \
    $(SCRATCH)/helpers_phy_ac.c \
    $(SCRATCH)/ppr_ac.c

# rxiqcal_phy_ac.c is the harness's own RX-IQ estimator, which validates the
# solve against the captures; the driver does not use it.
SRCS := $(SCRATCH_SRCS_FULL) rxiqcal_phy_ac.c wrap.c main.c
OBJS := $(patsubst %.c,build/%.o,$(notdir $(SRCS)))

vpath %.c .

BIN := ac_trace

all: $(BIN)

build:
	mkdir -p build

$(SCRATCH)/%: $(B43)/%
	@mkdir -p $(SCRATCH)
	cp $< $@

# The copies are what the objects and gdb point at: not intermediates.
.PRECIOUS: $(SCRATCH)/%

build/%.o: $(SCRATCH)/%.c $(AC_HDRS) | build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

build/%.o: %.c $(AC_HDRS) | build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

-include $(OBJS:.o=.d)

trace: $(BIN)
	./$(BIN) rxiq_est_debug d6220 > trace.d6220.out
	@echo "wrote trace.d6220.out ($$(wc -l < trace.d6220.out) lines)"

clean:
	rm -rf build $(BIN) trace.*.out

.PHONY: all clean trace
