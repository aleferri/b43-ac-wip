# Build out-of-tree against the target device's kernel source, same
# convention as wl-diag and wl-cc-dump:
#
#   make KDIR=/path/to/kernel-3.4-rt \
#        ARCH=mips CROSS_COMPILE=mips-linux-gnu- \
#        -j
#
# KDIR must point at a kernel ALREADY configured with the firmware's own
# .config and Module.symvers/headers, or vermagic/CRC will not match and
# insmod will refuse the module.
#
# Kernel configuration this module needs:
#
#   CONFIG_KALLSYMS=y   fixup_exception, flush_icache_range's R4K back end
#                       and flush_tlb_kernel_range are resolved by name;
#                       none of the three is exported to modules. Data
#                       symbols are NOT needed, so KALLSYMS_ALL is not
#                       required (init_mm is reached through init_task,
#                       which is exported).
#   CONFIG_PCI=y        for the BAR discovery. Without it, pass win_base
#                       and win_len by hand.
#
# CONFIG_KPROBES is explicitly NOT needed and explicitly NOT relied on:
# on this architecture do_page_fault()'s notify_die(DIE_PAGE_FAULT) only
# exists when KPROBES is on, which is why the trap hangs off do_bp()'s
# DIE_BREAK instead. See DESIGN.md.
#
# Load order does not matter any more. The module finds the window by
# itself and waits for it: insmod before wl, after wl, or between two
# `ifconfig up` cycles all work.
#
# tools/ holds host-side (not cross-compiled) checks of the two pure-logic
# units. Run them before trusting anything built from them on hardware:
#   make -C tools check

obj-m := wl_mmio_trap.o
wl_mmio_trap-objs := wl_mmio_trap_main.o mips_mmio_emulate.o mmio_pte.o \
                     win_find.o win_redirect.o bp_hook.o mips_opcode.o \
                     kern_syms.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

check:
	$(MAKE) -C tools check

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C tools clean

.PHONY: all check clean
