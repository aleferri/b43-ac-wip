#!/bin/sh
# Il gate del port sui segmenti `up` dello sweep a caldo.
#
# Perche' esiste, e perche' gates.sh non lo sostituisce.
#
# Lo sweep a freddo e' tutto primo bring-up: un modulo ricaricato per canale.
# Ogni predicato che distingue il primo bring-up dai successivi e' quindi
# invisibile la', e un termine mancante non si vede -- vale lo stesso su tutti e
# 26 i segmenti. E' esattamente quello che e' successo a
# b43_phy_ac_may_calibrate_tx(), che guardava solo center_freq <= 5250: a
# freddo i punteggi tornavano, e sui segmenti up sopra i 5250 il port stava al
# 35% invece che all'80%. Quarantacinque punti su una meta' del driver che
# nessuno misurava.
#
# I tre segmenti di default sono scelti per cogliere quel caso: uno sotto i
# 5250 MHz e due sopra. Se un predicato confonde le due condizioni, il primo
# resta fermo e gli altri due crollano.
#
# Differenze di preparazione rispetto a gates.sh, tutte necessarie:
#
#   flow `up`          un segmento a caldo non contiene il caricamento del
#                      modulo, e va spezzato: le undici letture di
#                      rx_gain_regs_program() cadono prima dell'inizio del
#                      segmento, quindi un flow che le esegue svuota le code
#                      dell'oracolo che servono alla fase sotto esame. Vedi il
#                      commento del flow in main.c.
#   AC_FIRST_INIT=0    un segmento a caldo non e' un primo bring-up, e
#                      init_regs del vendor prende la sua diramazione a due
#                      passate. Metterlo a 1 qui misurerebbe un'altra cosa.
#   oracolo da `from`  non c'e' insmod da cui partire: le OTP e il probe dei
#                      core sono fuori dal segmento.
#
# Uso: ./gates-caldo.sh [segmento...]
#      HOT=/tmp/hot/segmenti ./gates-caldo.sh
set -e
HERE=$(dirname "$0")
HOT=${HOT:-/tmp/hot/segmenti}
SEGS=${*:-"$HOT/01-up-ch36-bw20.txt $HOT/09-up-ch52-bw20.txt $HOT/19-up-ch104-bw20.txt"}

if [ ! -d "$HOT" ]; then
	echo "manca $HOT:"
	echo "  unzip -d /tmp/hot router-data/d6220/hot-sweep.zip"
	exit 1
fi

fail=0
for seg in $SEGS; do
	[ -f "$seg" ] || { echo "manca $seg"; fail=1; continue; }
	ch=$(basename "$seg" | sed -n 's/.*ch\([0-9]*\)-bw.*/\1/p')
	bw=$(basename "$seg" | sed -n 's/.*-bw\([0-9]*\).*/\1/p')

	python3 "$HERE/../reverse-tools/merge_retvals.py" "$seg" /tmp/hot.merged \
		>/dev/null 2>&1 || cp "$seg" /tmp/hot.merged

	# La finestra parte dalla prima delle dieci letture di save che aprono
	# switch_analog_once(), come a freddo.
	from=$(python3 - /tmp/hot.merged <<'PY'
import re, sys
for line in open(sys.argv[1], errors='replace'):
    if re.match(r"\s*[\d.]+\s+#\d+\s+cpu\d+\s+PHY\.RD\s+addr=0x0*739\s", line):
        print(re.search(r'#(\d+)', line).group(1))
        break
PY
)
	[ -n "$from" ] || { echo "$seg: nessuna op PHY"; fail=1; continue; }
	last=$(grep -oE '#[0-9]+' /tmp/hot.merged | tail -1 | tr -d '#')

	sched=$(python3 "$HERE/../reverse-tools/probe_schedule.py" \
		/tmp/hot.merged --sh 2>/dev/null || true)

	if grep -q ' MAC\.BW' /tmp/hot.merged; then
		macw=0
	else
		macw=$(case $bw in 40) echo 2 ;; 80) echo 3 ;; *) echo 1 ;; esac)
	fi

	echo "=== $(basename "$seg" .txt) (da $from${sched:+, $sched}) ==="

	# shellcheck disable=SC2086
	env AC_CHANNEL=$ch AC_BW=$bw AC_MAC_WIDTH=$macw AC_FIRST_INIT=0 \
	    AC_READ_ORACLE=/tmp/hot.merged AC_READ_ORACLE_FROM=$from $sched \
		"$HERE/ac_trace" up d6220 2>/dev/null > /tmp/hot.full

	nport=$(grep -c '^cpu' /tmp/hot.full || true)
	if [ "$nport" -lt 6000 ]; then
		echo "  ATTENZIONE: il port ha emesso solo $nport op: set_channel"
		echo "  ha rifiutato ch$ch/bw$bw. Serve: make clean && make AC_ANY_CHANNEL=1"
	fi

	echo "  --- cmp_skip ---"
	python3 "$HERE/cmp_skip.py" /tmp/hot.merged /tmp/hot.full \
		"$from:$last" --board d6220 \
		| grep -E 'grezzo|valore sbagliato|op di wl mancanti'
	echo "  --- compare ---"
	python3 "$HERE/compare.py" /tmp/hot.merged /tmp/hot.full \
		--range "$from:$last" --auto-align \
		> /tmp/hot.cmp || fail=1
	sed -n '/^  @/{p;q}' /tmp/hot.cmp
done

exit $fail
