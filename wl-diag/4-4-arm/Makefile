# Out of tree, against the device's ARM kernel 4.4, configured with the same
# .config as the firmware plus CONFIG_KPROBES and CONFIG_KRETPROBES, and with
# its Module.symvers, or insmod refuses the module on vermagic or CRCs.
#
#   make KDIR=/path/to/kernel-4.4 ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j

obj-m := wl_diag.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
