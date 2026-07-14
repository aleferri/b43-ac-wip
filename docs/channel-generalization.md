# Analisi generalizzazione per canale/BW

## Contesto

Il draft 1 di `set_channel` è validato op-per-op contro il trace vendor
`d6220/attach-to-bss-ch36` (22268/22268 op). Molti commenti nel codice segnano
alcuni blocchi come `SALAME: valido solo per D6220 ch36` (bulk LUT hardcoded,
sequenze radio chain, coefficienti RXIQ). La domanda: quanto di quello che
sembra channel-specific lo è davvero, e quanto è già invariant?

## Metodo

Diff a 3 vie tra i 3 trace vendor disponibili in `router-data/d6220/`:
- `attach-to-bss-ch36.txt`      — canale 36, BW20
- `attach-to-bss-ch44.txt`      — canale 44, BW20
- `attach-to-bss-ch36-bw40.txt` — canale 36, BW40

Allineamento sull'anchor MHF `addr=0x0004 val=0x0080 mask=0x0080` (prima op
in comune dopo l'op_init tail). Op successive comparate con `difflib.
SequenceMatcher` per identificare cluster di differenze.

## Risultato macro

| Confronto | Op identiche | Ratio |
|---|---:|---:|
| ch36 vs ch44 (canale diverso, stessa BW) | 21797 / 22757 | 95.8% |
| ch36 vs bw40 (stesso canale, BW diversa) | 21319 / 22757 | 93.7% |

Cioè il grosso del driver è già de-facto channel/BW-invariant. Le op
chan-dependent sono concentrate in 2-3 aree ben identificabili.

## Cosa è verificato invariante

Cross-trace identical (nessuna formula necessaria):

1. **MHF sequence** — 19 MHF vendor identici per (addr, val, mask) su ch36,
   ch44 e bw40. Pura config MAC.
2. **`lut_0040[128]`** — LUT power-vs-index primaria in coda a
   `rxiqcal_finalize`. Contrariamente al commento SALAME, i 128 valori sono
   identici sui 3 trace.
3. **`lut_0060[128]`** — LUT secondaria idem.
4. **`lut_0021[24]`** — u32 flag table idem.
5. **Prime 214 op consecutive** del set_channel body (radio init pre-tuning).
6. Tutta la sequenza calibrazione lunga (`set_channel_calibrations`,
   ~15'000 op: post_cal_finalize + rxiqcal loop + rxcal_afe + gainctrl_final).

Implicazione: i tre commenti `SALAME: valido solo per D6220 ch36` sui bulk
LUT possono essere rimossi (o riscritti "valido su tutte le combo
canale/BW 5 GHz esaminate: ch36/ch44/ch36-bw40").

## Cosa è chan-dependent

### A) Chanspec tables — TBL id=0x0007

~30 write in coda al chan_tuning, con **offset e valore entrambi
chan-derived**.

Esempi (ch36 → ch44):

| offset | ch36 val | ch44 val | delta |
|---|---|---|---|
| 0x0143 | 0x0151 | 0x0153 | +0x0002 |
| 0x0363 | 0x50db | 0x52db | +0x0200 |
| 0x0142 | 0x50db | 0x52db | +0x0200 |

Il delta 0x0200 su 8 canali (36→44, cioè 40 MHz) suggerisce **64 unit per
canale** — plausibilmente una codifica LO-frequency scalata. Formula
candidata:

```
val_high = base_high + (chan - 36) * 8
val_low  = base_low                   // fisso
```

Verifica: servono trace vendor su ch40, ch48, ch149 (o comunque su ≥ 3
canali) per confermare linearità. Le due trace disponibili (ch36, ch44)
bastano per parametrizzare in modo lineare ma non per validare la formula.

### B) Radio chain setup per-core

~40 RAD.WR sui registri della sezione `0x08XX` (core 1) e `0x06XX` (core 0
via stride 0x200), più radio filter (`0x011X`, `0x065X`, `0x019X`,
`0x01aX`, `0x1603`, `0x1607`, ecc.).

Registri 100% chan-dependent (osservazione dal diff ch36 vs ch44):

```
Core 1:  0x0846, 0x08e6, 0x08e7, 0x08d8, 0x0894, 0x0895,
         0x0896, 0x0897, 0x0899, 0x089a, 0x089b, 0x089c,
         0x08c7, 0x08c8, 0x08cc
Filter:  0x011b, 0x065c, 0x019a, 0x019b, 0x01a1, 0x01a2,
         0x1603, 0x1607
```

Valori non lineari in frequenza. Vendor blob li calcola runtime da una
routine tipo `wlc_phy_chanspec_radio_2069_setup`; da confermare col decomp.

Approccio pratico: tabella `{chan → (reg, val)[]}` estratta dai trace
disponibili. Con solo ch36 e ch44 la tabella copre 2 canali; altri
resterebbero untested finché non arrivano nuovi trace.

### C) Loop convergence variabile

Il loop iniziale su reg `0x0413` (probabile radio calibration loop
convergente) fa 1-3 iterazioni in più su ch36/bw40 rispetto a ch44. Cioè
il vendor implementa un test-of-convergence e loopa finché non stabilizza.

Impatto: ~5 op su 22268 (trascurabile). Prima del bring-up si può
ignorare; dopo il primo funzionamento si può indagare quale bit indica
convergenza e sostituire il loop hardcoded con un `do { ... } while
(!converged);`.

## Cosa è BW-dependent

Rispetto a ch36 vs ch44 (960 diff), ch36 vs bw40 ha 1438 diff, cioè +478
op imputabili al cambio BW20→BW40. Aree:

1. **Farrow / sample rate config**: BW40 usa una tabella farrow diversa
   (stub in `farrow_phy_ac.c`).
2. **Filter chain**: registri `0x04XX` (filter regs) hanno valori diversi.
3. **Chanspec register**: il bit BW nel chanspec (reg `0x0417`/`0x0416`)
   codifica BW20 vs BW40.
4. **Gain regs `0x1XXX`-`0x17XX`**: alcune bank shadow hanno valori
   bw-derived.

Prima del bring-up conviene rifiutare esplicitamente BW40 con
`-EOPNOTSUPP` in `set_channel`, aggiungendo BW40 solo dopo che BW20
funziona in modo stabile.

## Cosa è runtime-measured (non chan-dependent nel senso "serve tabella")

I coefficienti RXIQ in coda a `rxiqcal_finalize` variano tra canali ma
con delta piccoli (1-3 unit):

| reg | ch36 | ch44 | bw40 |
|---|---|---|---|
| RAD 0x0203 | 0x0079 | 0x0079 | 0x0078 |
| RAD 0x0205 | 0x0078 | 0x0078 | 0x0078 |
| PHY 0x06a0 | 0x03f2 | 0x03f0 | 0x03f3 |
| PHY 0x06a1 | 0x004c | 0x0052 | 0x004a |
| PHY 0x08a0 | 0x03db | 0x03d6 | 0x03dc |

Sono risultati della RXIQ probing (che il vendor esegue sul HW real-time).
Nel porting sono hardcoded dal capture. Non serve una formula: sul HW
reale il probing converge sui valori giusti per il canale corrente. Basta
implementare l'estrazione dei coefficienti probed (la fase di misura
esiste già in `rxiqcal_measure_block`, manca il read-back).

## Piano operativo

### Fase 1 — prima del bring-up HW (~2h)

Sicurezze:

1. Rimuovere i commenti `SALAME: valido solo per D6220 ch36` dai bulk LUT
   `lut_0040`, `lut_0060`, `lut_0021` (verificato: chan/BW-invariant).
2. Aggiungere `EOPNOTSUPP` esplicito su BW40 in `set_channel`.
3. Restringere la lista di canali supportati al set testabile e
   documentare che ch36 è validato op-per-op, ch44 dovrebbe funzionare
   (95%+ op identici), altri canali sono untested.

### Fase 2 — dopo primo bring-up funzionante su ch36 (~1 giorno)

Estrazione tabelle:

4. Scrivere script `reverse-tools/extract_chan_tuning.py` che diffa i
   trace ch36 e ch44 ed estrae la tabella chan-specific
   `{chan → (reg, val)[]}`.
5. Con solo ch36 e ch44 la tabella copre 2 canali. Se possibile,
   catturare altri trace vendor su ch40, ch48, ch149, ecc. per estendere.

Loop convergence (bassa priorità):

6. Investigare il loop `0x0413` per capire il condition-of-convergence e
   sostituire con `do/while` runtime.

### Fase 3 — BW40 (dopo che BW20 funziona bene)

7. Diffare ch36-bw40 vs ch36-bw20, estrarre op BW-specific.
8. Implementare farrow config BW40 (già stub).
9. Filter chain BW40.

### Fase 4 — RXIQ runtime probing (long-term)

10. Implementare read-back dei coefficienti RXIQ post-probe per rimuovere
    l'hardcode.

## Sommario numerico

| Categoria | # op ch36 | # op chan-dep | % invariant |
|---|---:|---:|---:|
| Init pre-chanspec | ~214 | 0 | 100% |
| Radio chain setup | ~500 | ~80 | 84% |
| Chanspec tables | ~200 | ~30 | 85% |
| Calibrazioni | ~15'000 | ~200 (runtime) | 98.7% |
| Bulk LUT rxiqcal | ~450 | 0 | 100% |
| Coda finale | ~72 | ~10 (RXIQ) | 86% |
| **Totale** | **22'268** | **~960** | **95.8%** |

## Conclusione

Generalizzazione fattibile in ~1 giorno di lavoro dopo il primo bring-up,
non è un rewrite. La maggior parte dei "SALAME: hardcoded per ch36" nel
codice sono falsi allarmi. Le due sole aree veramente chan-dep sono radio
chain setup (~40 reg per canale) e chanspec tables (~30 entry per
canale) — entrambe risolvibili con lookup table o formula estratta dai
trace vendor su più canali.
