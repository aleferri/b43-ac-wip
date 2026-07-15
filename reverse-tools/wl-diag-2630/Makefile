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
# Su 2.6.30 kallsyms_lookup_name esiste ma NON e' esportata ai moduli (l'export
# arriva in 2.6.33), quindi il sorgente disabilita da se' quel ramo sotto 2.6.33:
# non serve alcun flag per compilare. Per risolvere i simboli, la via comoda e'
# passare l'indirizzo di kallsyms_lookup_name (una riga di /proc/kallsyms):
#   insmod wl_diag.ko arm=1 klookup=0x<addr_di_kallsyms_lookup_name>
# e il modulo trova gli altri da solo. In alternativa, la lista esplicita:
#   insmod wl_diag.ko arm=1 syms="phy_reg_read:80xxxxxx,si_corereg:80yyyyyy,..."
# Per forzare lo stesso comportamento su un kernel piu' recente:
#   make KDIR=... EXTRA_CFLAGS=-DWLDIAG_NO_KALLSYMS ...

obj-m := wl_diag.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
