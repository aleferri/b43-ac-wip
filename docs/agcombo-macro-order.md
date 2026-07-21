# Ordine macro di set_channel: agcombo (wl 7.14) vs D6220 (ordine del port)

## Domanda

L'agcombo (BCM4360, wl 7.14.43) e il D6220 (BCM4352, wl piu' recente, l'ordine
replicato dal port) producono le stesse fasi di `set_channel`? E se l'ordine
locale differisce, e' una scelta architetturale della versione di driver
(lecita, da normalizzare nel confronto) o un bug/differenza di chip?

## Metodo

Falsificabile, non a occhio. Per ogni fase di `b43_phy_ac_set_channel` si
estrae una **firma**: la prima write/maskset a costanti nel corpo della
funzione della fase. La firma si localizza in sequenza nella traccia di
riferimento (harness `set_channel`, che replica l'ordine D6220); l'intervallo
tra una firma e la successiva e' la fase. Gli **episodi agcombo** di ciascuna
fase si ottengono accoppiando le op del riferimento con quelle della cattura
agcombo tramite `reverse-tools/reorder_trace.py` (accoppiamento strutturale per
tipo+indirizzo, valori esclusi) e leggendo i numeri d'episodio `#N` dei partner
vendor. Riproducibile con `/tmp/macromap.py` (le firme sono elencate nello
script) sugli output collassati dei due lati.

## Risultato: l'ordine macro e' identico

Tutte le 17 fasi ancorabili con una firma compaiono nello **stesso ordine** in
entrambi i driver: gli episodi agcombo crescono monotoni lungo la sequenza del
riferimento, zero permutazioni.

| # | Fase | Episodi agcombo | Copertura accoppiata |
|---|------|-----------------|----------------------|
| 0 | channel_switch_prep | #58403..#58415 | 100% |
| 1 | radio_2069_channel_setup | #58416..#58538 | 100% |
| 2 | phy_channel_setup | #58539..#60370 | 93% |
| 3 | chan_tables | #60374..#63461 | 100% |
| 4 | rxgain_init | #63462..#63770 | 100% |
| 5 | post_noise_shaping | #63782..#63925 | 100% |
| 6 | afecal | #63926..#64091 | 99% |
| 7 | adc_reset | #64092..#64178 | 100% |
| 8 | idle_tssi_meas | #64179..#65372 | 100% |
| 9 | txpwrctrl_setup (1a chiamata) | #65373..#65865 | 100% |
| 10 | txpwrctrl_setup (2a chiamata) | #65866..#66343 | 100% |
| 11 | rxgainctrl_regs | #66344..#66472 | 100% |
| 12 | rxcal_radio_setup | #66473..#66573 | 100% |
| 13 | rxcal_tone_setup | #66574..#67063 | 100% |
| 14 | rxcal gaincal (core 0..) | #67066..#69433 | 100% |
| 15 | rxcal_cleanup | #69434..#79279 | 100% |
| 16 | blocco est rxiq finale | #79280..#82786 | 54% |

## La differenza macro NON e' una permutazione

Le fasi sono nello stesso ordine; le differenze tra 7.14 e driver nuovo sono
altrove, e la mappa le isola:

**Struttura di ripetizione.** Le scritture gain-cal su TBL 0xc (off
0x63/67/6b + banco 0x73/77/7b) nel 7.14 compaiono ~55 volte distribuite da
#64007 a #81087 -- la prima cade dentro lo span di afecal e proseguono
attraverso idle_tssi, txpwrctrl e tutto il blocco rxcal -- mentre il port le
emette una volta per step di gainctrl dentro la fase rxcal. Non e' un blocco
spostato: e' la stessa fase con cadenza diversa. Speculare per i readback
rxgain 0x09aX (una volta nel 7.14, quattro ripetizioni nel port -- le
"replicate" del reorder).

**Contenuto unilaterale** (i residui, non permutabile per definizione):
solo-7.14 il gain-cal core-2 (off 0x6b/0x7b) e le due est fini in coda;
solo-driver-nuovo i RAD 0x020e/0x036e.

## Le due zone < 100% non sono buchi del port

**phy_channel_setup (93%).** Le op non accoppiate esistono 1:1 su entrambi i
lati (blocco RF-seq); non si accoppiano per un micro-riordino 7.14 (arming
per-core 0x0160/0x0401 a 0x0001 invece del coremask). E' segnale di versione,
non un buco: 0 mismatch di valore dopo i fix chip.

**Blocco est rxiq finale (54%).** E' dove i due mondi divergono di piu': la
coda est/rxiq del 7.14 contiene lavoro che il port non replica ancora.

## Caveat

Vale per `set_channel`. La ridistribuzione init<->set_channel non e' misurabile
finche' l'op_init agcombo del harness resta troncato (91 op collassate). Le
firme sono euristiche (prima write a costante): se una fase venisse riscritta in
modo da cambiare la prima costante emessa, la sua ancora andrebbe aggiornata in
`/tmp/macromap.py`.
