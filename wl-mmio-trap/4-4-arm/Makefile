# Out of tree, against the device's ARM kernel 4.4, configured with the same
# .config as the firmware plus the options below, and with its
# Module.symvers, or insmod refuses the module on vermagic or CRCs.
#
#   make KDIR=/path/to/kernel-4.4 ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j
#
# Kernel configuration this module needs:
#
#   CONFIG_KALLSYMS=y   flush_tlb_kernel_range is resolved by name. With
#                       CONFIG_KALLSYMS_ALL the static fsr_info is resolved
#                       too; without it, pass fsr_info_fn7= (see arm_syms.c).
#   CONFIG_PCI=y        for the BAR discovery; without it pass win_phys=.
#   !CONFIG_ARM_LPAE    the fault codes and pte layout are the 2-level ones.
#   !CONFIG_THUMB2_KERNEL  the emulator decodes A32; the Northstar wl is A32.
#
# Unlike the MIPS build this does NOT rely on the absence of CONFIG_KPROBES:
# it hooks the data-abort dispatch table, not a breakpoint on a fault path.
#
# win_find.c and wl_ring.c are shared with the MIPS build one level up, built
# through the one-line *_shared.c wrappers here so there is one copy. tools/
# holds the native check of the ARM load/store emulator:  make -C tools check

obj-m := wl_mmio_trap.o
wl_mmio_trap-objs := wl_mmio_trap_main.o arm_mmio_emulate.o mmio_pte.o \
                     arm_syms.o win_find_shared.o wl_ring_shared.o \
                     dma_dd_shared.o

ccflags-y := -I$(src)/..

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

check:
	$(MAKE) -C tools check

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C tools clean

.PHONY: all check clean
