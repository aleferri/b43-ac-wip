# Indice della documentazione

## Stato e piano

| Doc | Cosa contiene |
|---|---|
| [`driver-status.md`](driver-status.md) | Stato attuale del bring-up: board/canale/BW supportati, mappa file→patch, procedura di rigenerazione, split upstream previsto della patch 0006, SALAME notevoli. |
| [`porting-plan.md`](porting-plan.md) | Piano di porting del percorso `down → bss-up`. Struttura del flow vendor osservato nella trace `d6220-wl1-down-to-bss-up`, ordine di implementazione. |
| [`retrace-todo.md`](retrace-todo.md) | Lista di intervalli di trace da ri-catturare o ri-ancorare per singola funzione. Utile per pianificare nuove capture wl-diag. |

## Analisi tecniche

| Doc | Cosa contiene |
|---|---|
| [`channel-generalization.md`](channel-generalization.md) | Analisi di quanto del driver è già chan/BW-invariant. Diff a 3 vie tra ch36/ch44/ch36-bw40. Quali blocchi vanno generalizzati, con piano operativo in 4 fasi. |
| [`rxiq-cal-analysis.md`](rxiq-cal-analysis.md) | Analisi del blocco di calibrazione RX-IQ: struttura 4-step del sweep vendor, mapping tra i registri toccati e le fasi (RX AFE reconfig, radio 2069 IQ-cal, poll blocks, cleanup). |
| [`txlpf-formula.md`](txlpf-formula.md) | Reverse engineering della formula per le celle table 7 TX-LPF. Formula candidata, pre-state per stage, mapping bit-per-bit tra canale/BW/chip e valori scritti. Punto aperto principale del porting. |
| [`nvram-reference.md`](nvram-reference.md) | Significato di ogni variabile NVRAM/SROM rev 11 e sua destinazione nel programming PHY/radio, con livelli di confidenza (verificato / standard / SALAME / TODO). Include la sintesi delle correlazioni confermate (pa5ga→est_pwr, rxgains→rxgain_init, maxp5ga→max index, tssifloor→0x0724). |
| [`agcombo-macro-order.md`](agcombo-macro-order.md) | Mappa dell'ordine macro di `set_channel`: 17 fasi ancorate a firme dal sorgente, localizzate nel riferimento e negli episodi agcombo. Conclude che l'ordine macro agcombo (wl 7.14) e D6220 è identico; le differenze sono cadenza di ripetizione e contenuto unilaterale, non permutazioni. |
| [`dsl3580l-diff-index.md`](dsl3580l-diff-index.md) | Indice funzione-per-funzione delle differenze DSL-3580L (4352 wl6.30) vs d6220 (4352) vs agcombo (4360), con triangolazione chip/versione. Reperti verificati (0x0033, PLLCTL3, res mask, readback radio), differenze [CHIP] che confermano i condizionali is4360, e fasi non confrontabili col flow down-to-bss (serve attach). |

## Come navigare

- Per capire **cosa funziona** oggi: `driver-status.md`.
- Per capire **cosa manca** per il bring-up HW: sezione TODO in `driver-status.md`, poi `txlpf-formula.md`.
- Per capire **come estendere ad altri canali** dopo il primo bring-up:
  `channel-generalization.md`.
- Per capire **la struttura del set_channel**: `rxiq-cal-analysis.md` (fase
  di calibrazione, che è la parte più grossa).
- Per capire **cosa significa una variabile NVRAM** e dove finisce nel
  programming: `nvram-reference.md`.
- Per pianificare **nuove capture wl-diag** dal vendor: `retrace-todo.md`
  e `porting-plan.md`.

## Note per contributori

- I `SALAME` nei doc e nel codice sono speculazioni documentate: la nota è
  necessaria proprio perché il fatto non è confermato dal decomp vendor.
- I `TODO(formula)` nel codice sono valori hardcoded dai capture vendor
  che vanno derivati da SPROM/canale/BW quando si passa a coprire più
  configurazioni.
- I README locali (`../reverse-tools/README.md`, `../router-data/*/README.md`,
  `../sprom-rev11/README.md`) documentano contenuti specifici di quelle
  directory e restano lì.
