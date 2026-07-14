# Build out-of-tree contro il sorgente del kernel 3.4-rt del device.
#
#   make KDIR=/path/al/kernel-3.4-rt \
#        ARCH=mips CROSS_COMPILE=mips-linux-gnu- \
#        -j
#
# KDIR deve puntare a un kernel GIA' configurato con gli stessi .config del
# firmware (stesso CONFIG_*; in particolare CONFIG_KPROBES=y e
# CONFIG_KALLSYMS=y) e con i Module.symvers/headers del build originale,
# altrimenti vermagic/CRC non combaciano e insmod rifiuta il modulo.

obj-m := wl_diag.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
