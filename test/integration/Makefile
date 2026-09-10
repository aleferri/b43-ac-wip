# SPDX-License-Identifier: GPL-2.0
#
# b43 intero su AC, contro gli header kernel veri.
#
# Il punto: in b43 l'ordine degli eventi non si legge -- 42 gate su core_rev,
# fw.rev, phy->type e phy->rev con rami else liberi -- quindi lo si esegue.
# Qui si compilano i sorgenti di b43 senza modificarli e si stubba il livello
# sotto, cosi' l'ordine e' output e non esegesi.
#
# Gli header kernel sono quelli VERI, da /usr/src: non serve stubbare
# linux/*.h come fa ../unit. Su Ubuntu:
#     apt-get install linux-headers-generic
#
# La versione di b43 deve combaciare con quella degli header, o si prendono
# errori che non sono del codice: b43 da master vuole linux/unaligned.h, che
# esiste da 6.12. Vedi fetch-upstream.sh.
#
# -O2 non e' un'ottimizzazione, e' un requisito: senza, gli header kernel
# danno "impossible constraint in 'asm'" su asm_inline.

KVER    ?= $(shell ls /usr/src/ | grep -oE '^linux-headers-[0-9.]+-[0-9]+$$' | head -1 | sed 's/linux-headers-//')
K       := /usr/src/linux-headers-$(KVER)
KG      := /usr/src/linux-headers-$(KVER)-generic
UP      := b43-upstream

# kinc/ tiene i due header ssb toppati da patches/0001, che aggiunge i campi
# della SROM rev11 (pa5ga, maxp5ga, femctrl, subband5gver, rxgains). Vanno
# PRIMA degli header kernel, o phy_ac.c non compila: quei campi in mainline
# non esistono. La parte di patches/0001 su drivers/bcma/sprom.c non serve,
# qui il bus e' finto.
KINC := -I$(CURDIR)/kinc -I$(KG)/include -I$(K)/include -I$(K)/include/uapi \
        -I$(KG)/include/generated/uapi \
        -I$(K)/arch/x86/include -I$(K)/arch/x86/include/uapi \
        -I$(KG)/arch/x86/include/generated \
        -I$(KG)/arch/x86/include/generated/uapi

KFLAGS := -nostdinc -I$(CURDIR)/$(UP) $(KINC) -D__KERNEL__ -DMODULE -DKBUILD_MODNAME=\"b43\" \
          -include $(K)/include/linux/kconfig.h \
          -include $(K)/include/linux/compiler_types.h \
          -DCONFIG_B43_BCMA=1 -DCONFIG_B43_PHY_AC=1 \
          $(if $(AC_ANY_CHANNEL),-DCONFIG_B43_PHY_AC_ANY_CHANNEL=1) \
          -O2 -fno-strict-aliasing -fno-common -std=gnu11 -w \
          -Werror=implicit-function-declaration \
          -include $(CURDIR)/compat.h \
          -DB43_AC_FN_TRACE

# Build per il backtrace: -O2 resta, perche' lo chiedono gli header; -fno-inline
# -g tengono i frame, cosi' gdb dice in quale funzione di b43 la probe muore
# invece che in b43_one_core_attach dove tutto e' inlinato.
ifeq ($(DEBUG),1)
KFLAGS += -fno-inline -g
endif

# I file di b43 che si compilano come sono. Tutti e tre a zero errori.
UPSTREAM := main.c phy_common.c bus.c xmit.c \
            phy_ac.c tables_phy_ac.c radio_2069.c rxiqcal_phy_ac.c \
            helpers_phy_ac.c ppr_ac.c

# I nostri: il bus finto che emette le op. L'aggancio e' la vtable
# bcma_host_ops, non i simboli bcma_* che il link chiede.
LOCAL := bcma_stub.c subsystem_stub.c main.c

# trace_out.c e' codice UTENTE: gli servono stdio e getenv, e non e' codice di
# driver. Si compila coi flag normali, non con -nostdinc.
USER := trace_out.c kernel_shim.c
UFLAGS := -O2 -Wall -std=gnu11
ifeq ($(DEBUG),1)
UFLAGS += -g

# ASAN=1 (con DEBUG=1): il sanitizer sulle due meta', kernel e userspace. Serve
# a far dire *chi* corrompe la memoria; gli alias __hwasan_* stanno in
# kernel_shim.c, perche' gli header del kernel rimappano memcpy e memset
# quando vedono __SANITIZE_ADDRESS__.
ifeq ($(ASAN),1)
KFLAGS += -fsanitize=address
UFLAGS += -fsanitize=address
LDEXTRA := -fsanitize=address
endif
endif

# Cio' che trace_out.c chiede a libc: si risolve al link finale, quindi va
# tenuto fuori dal conto del lavoro rimasto o il numero mente.
LIBC := memmove|fprintf|fopen|fclose|fgets|fwrite|getenv|memset|memcpy|stdout|stderr|strcmp|strstr|strnlen|snprintf|sscanf|__isoc99_sscanf|calloc|realloc|free|abort|_GLOBAL_OFFSET_TABLE_|__stack_chk_fail|__printf_chk|__fprintf_chk|__vfprintf_chk|setvbuf|strtol

.PHONY: all check link run clean fetch
all: b43-trace

fetch:
	sh fetch-upstream.sh $(KVER)
	sh apply-port.sh

# Traguardo 1: i sorgenti di b43 compilano contro gli header veri.
check:
	@test -d $(K) || { echo "header kernel assenti: apt-get install linux-headers-generic"; exit 1; }
	@test -f $(UP)/main.c || { echo "sorgenti b43 assenti: make fetch"; exit 1; }
	@for f in $(UPSTREAM); do \
	    n=$$(cd $(UP) && gcc -c $$f -o /dev/null $(KFLAGS) 2>&1 | grep -c 'error:'); \
	    echo "  $$f: $$n errori"; \
	done

# Traguardo 2: quanto manca al link, per famiglia. Il numero e' la mappa del
# lavoro rimasto, e il README lo spiega voce per voce.
link:
	@for f in $(UPSTREAM); do (cd $(UP) && gcc -c $$f -o $${f%.c}.o $(KFLAGS)) || exit 1; done
	@for f in $(LOCAL); do gcc -c $$f -o $${f%.c}.o $(KFLAGS) -I.; done
	@for f in $(USER); do gcc -c $$f -o $${f%.c}.o $(UFLAGS) -I.; done
	@ld -r $(patsubst %.c,$(UP)/%.o,$(UPSTREAM)) \
	      $(patsubst %.c,%.o,$(LOCAL)) $(patsubst %.c,%.o,$(USER)) \
	      -o b43_partial.o
	@nm -u b43_partial.o | awk '{print $$2}' | sort -u > residual.txt
	@echo "  simboli non risolti: $$(wc -l < residual.txt)"
	@for p in bcma_ ssb_ ieee80211_ b43_; do \
	    printf "    %-12s %s\n" $$p "$$(grep -c "^$$p" residual.txt)"; \
	done
	@printf "    %-12s %s\n" kernel \
	    "$$(grep -vE '^(bcma_|ssb_|ieee80211_|b43_)' residual.txt | \
	        grep -vwE '$(LIBC)' | wc -l)"
	@printf "    %-12s %s  (si risolvono su libc, non sono lavoro)\n" libc \
	    "$$(grep -wcE '$(LIBC)' residual.txt)"

# Traguardo 4: l'eseguibile, e la traccia.
b43-trace: link
	@gcc -o b43-trace b43_partial.o $(LDEXTRA)
	@echo "  $$(ls -la b43-trace | awk '{print $$5}') byte"

# La traccia. Senza oracolo ogni lettura e' zero e NON e' una misura: si passa
# una cattura ripiegata da reverse-tools/trace_filter.py --retvals.
# La traccia va su stdout insieme all'output di make: per un file da dare a
# compare.py si usa TRACE_OUT, che diventa B43_TRACE_OUT per il binario.
run: b43-trace
	@test -n "$(ORACLE)" || { \
	    echo "uso: make run ORACLE=<cattura ripiegata> [TRACE_OUT=<file>]"; exit 1; }
	B43_READ_ORACLE=$(ORACLE) $(if $(TRACE_OUT),B43_TRACE_OUT=$(TRACE_OUT)) \
	    $(if $(B43_CHANNEL),B43_CHANNEL=$(B43_CHANNEL)) \
	    $(if $(B43_BW),B43_BW=$(B43_BW)) ./b43-trace

clean:
	rm -f $(UP)/*.o *.o b43_partial.o residual.txt b43-trace
