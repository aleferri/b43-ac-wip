# Build out-of-tree against the device's kernel source (same as wl-diag).
#
#   make KDIR=/path/to/kernel-3.4-rt \
#        ARCH=mips CROSS_COMPILE=mips-linux-gnu- \
#        -j
#
# Same source builds for the DSL-3580L's 2.6.30 too: just point KDIR at that
# tree (the pr_warn shim in cc_dump.c is compiled in automatically below 2.6.35).
# There the radio is the PCIe device 0000:02:00.0, so base= is its BAR0
# (0xa0000000 per /proc/iomem), not the SoC's 0x18000000; the host is
# big-endian, so add bswap=1 if chipid reads back garbled.
#
# KDIR must point at a kernel configured identically to the firmware (same
# CONFIG_*) with the matching Module.symvers/headers of the original build,
# otherwise vermagic/CRC will not match and insmod refuses the module.
# Unlike wl-diag this module needs no kprobes/kallsyms; it only ioremaps.

obj-m := cc_dump.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
