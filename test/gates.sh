#!/bin/sh
# I due gate del port, sui segmenti dello sweep a freddo.
#
# Le catture usate prima -- attach-to-bss-up-ch36-bw20 e down-to-bss-ch36-bw20
# -- non tracciavano le classi OBJ, TPL e CAL: l'hook non esisteva quando sono
# state prese. Un'op assente da una cattura che non traccia la sua classe non
# dice niente sul driver, e dedurne qualcosa ha prodotto conclusioni sbagliate
# sul chanspec, sulle scritture probe-response e sul campione di rumore. Sono
# state rimosse; restano in git.
#
# Lo sweep a freddo le rimpiazza e copre di piu': un modulo ricaricato per
# canale, 18 canali a 20 MHz piu' 7 a 40 e 3 a 80, ognuno un attach isolato con
# tutte le classi. La verifica per canale che prima non era possibile lo e'.
#
# Uso: ./gates.sh [segmento...]      default: ch36 bw20, il canale validato

set -e
HERE=$(dirname "$0")
COLD=${COLD:-/tmp/cold}
SEGS=${*:-$COLD/cold03-ch36-bw20.txt}

if [ ! -d "$COLD" ]; then
	echo "manca $COLD: scompattare router-data/d6220/cold-sweep.zip e"
	echo "  python3 reverse-tools/split_by_mark.py <traccia> $COLD --prefix cold"
	exit 1
fi

fail=0
for seg in $SEGS; do
	ch=$(basename "$seg" | sed -n 's/.*ch\([0-9]*\)-bw.*/\1/p')
	bw=$(basename "$seg" | sed -n 's/.*-bw\([0-9]*\).*/\1/p')

	python3 "$HERE/../reverse-tools/check_class_coverage.py" --require "$seg"

	python3 "$HERE/../reverse-tools/merge_retvals.py" "$seg" /tmp/gate.merged \
		>/dev/null 2>&1

	# Il perimetro del flow parte dal chanspec, la prima op che il port emette
	# di quel ciclo; prima c'e' il test della shared memory del core b43, che
	# non e' codice del PHY. L'ancora fra le due fasi e' RAD.MOD 0x08ea.
	from=$(python3 - /tmp/gate.merged <<'PY'
import re, sys
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+OBJ\.WR\s+addr=0x0*a0\s', line)
    if m:
        print(m.group(1))
        break
PY
)
	anchor=$(python3 - /tmp/gate.merged <<'PY'
import re, sys
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+RAD\.MOD\s+addr=0x0*8ea\s+'
                 r'val=0x0*40\s+mask=0x0*40', line)
    if m:
        print(m.group(1))
        break
PY
)

	echo "=== ch$ch bw$bw (da $from, ancora $anchor) ==="

	# MAC.BW la scrive solo il primo segmento di ciascuna larghezza: gli
	# altri ereditano. Il segmento sa da se' se la contiene.
	if grep -q ' MAC\.BW' /tmp/gate.merged; then
		macw=0
	else
		macw=$(case $bw in 40) echo 2 ;; 80) echo 3 ;; *) echo 1 ;; esac)
	fi

	AC_CHANNEL=$ch AC_BW=$bw AC_MAC_WIDTH=$macw AC_FIRST_INIT=1 \
	AC_READ_ORACLE=/tmp/gate.merged AC_READ_ORACLE_FROM=$from \
		"$HERE/ac_trace" down d6220 2>/dev/null > /tmp/gate.down
	echo -n "  down: "
	python3 "$HERE/../reverse-tools/phase_diff.py" /tmp/gate.merged \
		/tmp/gate.down --from "$from" --until "$anchor" \
		> /tmp/gate.diff || fail=1
	grep -vE "^  anchored" /tmp/gate.diff | head -1

	AC_CHANNEL=$ch AC_BW=$bw AC_MAC_WIDTH=$macw AC_FIRST_INIT=1 \
	AC_READ_ORACLE=/tmp/gate.merged AC_READ_ORACLE_FROM=$from \
		"$HERE/ac_trace" up d6220 2>/dev/null > /tmp/gate.up
	echo -n "  up:   "
	python3 "$HERE/../reverse-tools/phase_diff.py" /tmp/gate.merged \
		/tmp/gate.up --from "$anchor" > /tmp/gate.diff || fail=1
	grep -vE "^  anchored" /tmp/gate.diff | head -1
done

exit $fail
