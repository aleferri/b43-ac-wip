# Build out-of-tree contro il sorgente kernel 2.6.30 della DSL-3580L (SoC
# BCM6362, MIPS big-endian, o32, SMP PREEMPT, toolchain gcc 4.4.2 buildroot).
#
#   make KDIR=/path/al/kernel-2.6.30 \
#        ARCH=mips CROSS_COMPILE=mips-linux- \
#        -j
#
# KDIR deve puntare al kernel GIA' configurato con gli stessi .config del
# firmware DSL (stesso CONFIG_*; serve CONFIG_KALLSYMS=y) e con i
# Module.symvers/headers del build originale, altrimenti vermagic/CRC non
# combaciano e insmod rifiuta il modulo. NB: a differenza della 3.4 NON serve
# CONFIG_KPROBES (il tracer usa detour inline, non kprobe).
#
# Se kallsyms_lookup_name NON e' esportato ai moduli su questo build (link error
# "Unknown symbol kallsyms_lookup_name"), ricompila con:
#   make KDIR=... EXTRA_CFLAGS=-DWLDIAG_NO_KALLSYMS ...
# e passa TUTTI gli indirizzi via parametro syms= (da /proc/kallsyms):
#   insmod wl_diag.ko arm=1 syms="phy_reg_read:80xxxxxx,si_corereg:80yyyyyy,..."

obj-m := wl_diag.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
