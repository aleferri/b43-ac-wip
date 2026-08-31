# Indice della documentazione

## Stato e piano

| Doc | Cosa contiene |
|---|---|
| [`crsminpwr-d6220.md`](crsminpwr-d6220.md) | Specifica delle soglie CRS min power derivata dal blob D6220 di riferimento e da 52 segmenti, con le tre LUT a `.rodata+0x40e84/94/a4`, i tre set di registri per larghezza, e la separazione fra reperti [BLOB], [MISURA] e [APERTO]. Include perche la correlazione rumore-banco non poteva funzionare. |
| [`driver-status.md`](driver-status.md) | Stato attuale del bring-up: board/canale/BW supportati, mappa file→patch, procedura di rigenerazione, split upstream previsto della patch 0006, SALAME notevoli. |
| [`porting-plan.md`](porting-plan.md) | Criterio di correttezza, gate attivi, cosa resta diviso per natura (trascrizione, derivazione, proprietà altrui), distinzione fra le fasi di bring-up. |
| [`retrace-todo.md`](retrace-todo.md) | Stato copertura del bring-up per funzione (via i marcatori `B43_AC_FN`) e divergenze note ancora aperte (DSL: prefregs −2 scritture, afe_lpf_stage, rccal ~84%; tables per-contenuto; accessor vendor da verificare col kallsyms). |

## Analisi tecniche

| Doc | Cosa contiene |
|---|---|
| [`txpwr-target-derivation.md`](txpwr-target-derivation.md) | Derivazione del target di potenza TX in `0x0646`: la catena a cinque stadi di `wlc_phy_txpower_recalc_target`, le tre scale di unita (quarti di dBm, mezzi dB dei nibble `po`, dBm interi del regolatorio), la formula parametrica su SROM esatta su 26/26 configurazioni dello sweep, e i nove candidati esclusi con argomento per i due residui a 40 MHz. Include cosa la CLM ha stabilito e cosa no, e quale cattura puo refutare quale ipotesi. |
| [`channel-generalization.md`](channel-generalization.md) | Analisi di quanto del driver è già chan/BW-invariant. Diff a 3 vie tra ch36/ch44/ch36-bw40. Quali blocchi vanno generalizzati, con piano operativo in 4 fasi. |
| [`rxiq-cal-analysis.md`](rxiq-cal-analysis.md) | Analisi del blocco di calibrazione RX-IQ: struttura 4-step del sweep vendor, mapping tra i registri toccati e le fasi (RX AFE reconfig, radio 2069 IQ-cal, poll blocks, cleanup). |
| [`txlpf-formula.md`](txlpf-formula.md) | Formula della famiglia LPF (TX-LPF, RX-LPF, DACBUF), **risolta**: cap derivato da rccal (E/F/G), pre-state preservato dalla RMW, verificata sui tre board. Include l'analisi storica. |
| [`bank-0910-analysis.md`](bank-0910-analysis.md) | Il banco `0x0910`: reperti strutturali sulle 12 catture, ipotesi escluse con la prova che le esclude, precedenti in b43/brcmsmac N-PHY (`CRSMINPOWER`) e ath9k (`minCCApwr`, mediana su finestra), e la relazione trovata: il banco e' un offset sulla scala della soglia CRS, `max(0, target - crs)`, con target per sito di chiamata e per chip. Include i controesempi che tengono i target come `TODO(formula)`. |
| [`nvram-reference.md`](nvram-reference.md) | Significato di ogni variabile NVRAM/SROM rev 11 e sua destinazione nel programming PHY/radio, con livelli di confidenza (verificato / standard / SALAME / TODO). Include la sintesi delle correlazioni confermate (pa5ga→est_pwr, rxgains→rxgain_init, maxp5ga→max index, tssifloor→0x0724). |
| [`agcombo-macro-order.md`](agcombo-macro-order.md) | Mappa dell'ordine macro di `switch_channel`: 17 fasi ancorate a firme dal sorgente, localizzate nel riferimento e negli episodi agcombo. Conclude che l'ordine macro agcombo (wl 7.14) e D6220 è identico; le differenze sono cadenza di ripetizione e contenuto unilaterale, non permutazioni. |
| [`dsl3580l-diff-index.md`](dsl3580l-diff-index.md) | Indice funzione-per-funzione delle differenze DSL-3580L (4352 wl6.30) vs d6220 (4352) vs agcombo (4360), con triangolazione chip/versione. Reperti verificati (0x0033, PLLCTL3, res mask, readback radio), differenze [CHIP] che confermano i condizionali is4360, e fasi non confrontabili col flow down-to-bss (serve attach). |

## Come navigare

- Per capire **cosa funziona** oggi: `driver-status.md`.
- Per capire **cosa manca** per il bring-up HW: `retrace-todo.md` (divergenze note) e la sezione stato in `driver-status.md`.
- Per **misurare la copertura per funzione**: harness con `AC_FN_MARKERS=1` +
  `reverse-tools/coverage_by_function.py` (vedi `reverse-tools/README.md`).
- Per capire **come estendere ad altri canali** dopo il primo bring-up:
  `channel-generalization.md`.
- Per capire **la struttura del switch_channel**: `rxiq-cal-analysis.md` (fase
  di calibrazione, che è la parte più grossa).
- Per capire **cosa significa una variabile NVRAM** e dove finisce nel
  programming: `nvram-reference.md`.
- Per pianificare **nuove capture wl-diag** dal vendor: `retrace-todo.md`
  e `porting-plan.md`.

## Note per contributori

- I `SALAME` nei doc e nel codice sono speculazioni documentate: la nota è
  necessaria proprio perché il fatto non è confermato.
- I `TODO(formula)` nel codice sono valori hardcoded dai capture vendor
  che vanno derivati da SPROM/canale/BW quando si passa a coprire più
  configurazioni.
- I README locali (`../reverse-tools/README.md`, `../router-data/*/README.md`,
  `../sprom-rev11/README.md`) documentano contenuti specifici di quelle
  directory e restano lì.
