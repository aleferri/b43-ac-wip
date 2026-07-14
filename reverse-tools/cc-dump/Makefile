# Build out-of-tree against the device's kernel-3.4-rt source (same as wl-diag).
#
#   make KDIR=/path/to/kernel-3.4-rt \
#        ARCH=mips CROSS_COMPILE=mips-linux-gnu- \
#        -j
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
