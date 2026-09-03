#!/bin/sh
# Il gate del port, sui segmenti dello sweep a freddo.
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
# tutte le classi.
#
# Un segmento si confronta col flow `full` e intero, non spezzato in `down` e
# `up`. Lo split serve sullo sweep a caldo, dove le undici letture di
# rx_gain_regs_program() cadono prima dell'inizio del segmento e un flow che le
# esegue svuota le code dell'oracolo che servono alla fase sotto esame. A
# freddo quel problema non esiste: il modulo viene caricato dentro il segmento,
# quindi quelle letture sono nel segmento e l'oracolo le serve dove il vendor
# le ha fatte. Spezzare a freddo lascia invece fuori dal confronto la testa
# dell'attach (switch_analog e le GPIO) e la coda dopo l'ultimo probe group.
#
# La finestra parte dalla prima op PHY dell'attach, non dall'insmod: fra i due
# ci sono il probe dei core e il test della shared memory, che sono del core
# b43. L'oracolo delle letture parte invece dall'insmod, cosi' le code per
# indirizzo sono complete.
#
# Uso: ./gates.sh [segmento...]      default: ch36 bw20, il canale validato

set -e
HERE=$(dirname "$0")
COLD=${COLD:-/tmp/cold/segmenti}
SEGS=${*:-$COLD/cold01-ch36-bw20.txt}

if [ ! -d "$COLD" ]; then
	echo "manca $COLD:"
	echo "  unzip -d /tmp/cold router-data/d6220/cold-sweep.zip"
	exit 1
fi

fail=0
for seg in $SEGS; do
	ch=$(basename "$seg" | sed -n 's/.*ch\([0-9]*\)-bw.*/\1/p')
	bw=$(basename "$seg" | sed -n 's/.*-bw\([0-9]*\).*/\1/p')

	python3 "$HERE/../reverse-tools/check_class_coverage.py" --require "$seg"

	python3 "$HERE/../reverse-tools/merge_retvals.py" "$seg" /tmp/gate.merged \
		>/dev/null 2>&1

	# Due confini, dallo stesso passaggio sul segmento.
	#
	# from    prima op PHY dell'attach, cioe' la prima delle dieci letture
	#         di save che aprono switch_analog_once(). E' da qui che parte il
	#         confronto: fra l'insmod e questa op c'e' il probe dei core e il
	#         test della shared memory, che sono del core b43 e non del PHY.
	#         Non va agganciata al banco AFE (PHY.WR 0x173e): quello viene
	#         dopo le dieci letture, e partire da la' le taglia fuori facendo
	#         sembrare che il port emetta dieci op in piu'. E non va presa
	#         come "prima op PHY qualsiasi": prima ancora il core scrive dei
	#         registri PHY suoi (cold03 #452, PHY.WR 0xa6).
	# oracle  l'insmod. L'oracolo delle letture parte da qui, non da `from`,
	#         perche' le code per indirizzo devono coprire anche le OTP e il
	#         probe dei core.
	eval "$(python3 - /tmp/gate.merged <<'PY'
import re, sys

first_phy = insmod = None
for line in open(sys.argv[1], errors='replace'):
    if insmod is None and re.match(
            r"\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+MARK\s+'mod COMING'", line):
        insmod = re.search(r'#(\d+)', line).group(1)
    if first_phy is None and re.match(
            r"\s*[\d.]+\s+#\d+\s+cpu\d+\s+PHY\.RD\s+addr=0x0*739\s", line):
        first_phy = re.search(r'#(\d+)', line).group(1)
    if insmod and first_phy:
        break
print(f"from={first_phy or ''}")
print(f"oracle={insmod or first_phy or ''}")
PY
)"
	[ -n "$from" ] || { echo "$seg: nessuna op PHY"; fail=1; continue; }
	last=$(grep -oE '#[0-9]+' /tmp/gate.merged | tail -1 | tr -d '#')

	# Deadline della fase probe e tick del watchdog: sono clock, e il clock
	# sta nei timestamp del segmento. Vedi probe_schedule.py.
	sched=$(python3 "$HERE/../reverse-tools/probe_schedule.py" \
		/tmp/gate.merged --sh 2>/dev/null || true)

	# MAC.BW la scrive solo il primo segmento di ciascuna larghezza: gli
	# altri ereditano. Il segmento sa da se' se la contiene.
	if grep -q ' MAC\.BW' /tmp/gate.merged; then
		macw=0
	else
		macw=$(case $bw in 40) echo 2 ;; 80) echo 3 ;; *) echo 1 ;; esac)
	fi

	echo "=== ch$ch bw$bw (da $from${sched:+, $sched}) ==="

	# shellcheck disable=SC2086
	env AC_CHANNEL=$ch AC_BW=$bw AC_MAC_WIDTH=$macw AC_FIRST_INIT=1 \
	    AC_READ_ORACLE=/tmp/gate.merged AC_READ_ORACLE_FROM=$oracle $sched \
		"$HERE/ac_trace" full d6220 2>/dev/null > /tmp/gate.full
	# Le due misure, con gli strumenti di riferimento.
	#
	# La riga da guardare e' "grezzo": op di wl riprodotte in ordine su
	# TUTTE quelle di wl, perche' l'obiettivo e' che b43 le emetta tutte,
	# comprese quelle del core. "nel perimetro" toglie cio' che l'harness non
	# puo' emettere e serve solo a far vedere la prossima divergenza del PHY
	# senza fermarsi sulla prima op del core -- e' navigazione, non punteggio.
	# Il debito del core e' in docs/retrace-todo.md.
	#
	# compare.py e' il confronto posizione-per-posizione, che si ferma alla
	# prima divergenza e da' il contesto. Le due misure non sono
	# confrontabili: la prima tollera inserimenti, la seconda no.
	# Il guard di canale di set_channel() rifiuta tutto cio' che non e' ch36
	# BW20 salvo build con AC_ANY_CHANNEL=1, e il flow esce con poche migliaia
	# di op. Il punteggio che ne esce sembrerebbe una regressione del port
	# invece di un binario compilato per un altro scopo, quindi lo si dice.
	nport=$(grep -c '^cpu' /tmp/gate.full || true)
	if [ "$nport" -lt 6000 ]; then
		echo "  ATTENZIONE: il port ha emesso solo $nport op: set_channel"
		echo "  ha rifiutato ch$ch/bw$bw. Serve: make clean && make AC_ANY_CHANNEL=1"
	fi

	echo "  --- cmp_skip ---"
	python3 "$HERE/cmp_skip.py" /tmp/gate.merged /tmp/gate.full \
		"$from:$last" --board d6220 \
		| grep -E 'grezzo|nel perimetro|CON  ecce|fuori perimetro|op saltate|valore sbagliato|op di wl mancanti|solo vendor|solo port|invisibili'
	echo "  --- compare ---"
	python3 "$HERE/compare.py" /tmp/gate.merged /tmp/gate.full \
		--range "$from:$last" --auto-align \
		> /tmp/gate.cmp || fail=1
	sed -n '1,4p;/^  @/{p;q}' /tmp/gate.cmp
done

exit $fail
