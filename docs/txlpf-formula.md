# Analisi reverse-engineering formula TX-LPF (celle table 7)

## Contesto

Il blob Broadcom `wl.ko` scrive celle della tabella PHY 7 durante `set_analog_tx_lpf` (parte di `analog_on_reset` nel channel_setup). Il pattern è RMW su ogni cella (`TBL.RD → PHY.RD 0x000f → RMW → TBL.WR → PHY.WR 0x000f`).

Nel scratch del driver Linux, avevamo una formula candidata `f = lpf_cap` diretto (`f9<<9 | f17<<17`) che produceva bit spuri rispetto ai valori vendor. Il vero output del chip è governato da una formula più complessa.

## Formula candidata (dopo reverse engineering)

```
f = 0xa3 + ((lpf_cap >> 1) & 0x0f) + delta_channel_bw(chan, bw)

v_25bit = pre_state[stage] | (f << 9) | (f << 17)
lo = v_25bit & 0xffff
hi = (v_25bit >> 16) & 0x1ff
```

Cioè:
- `f` viene calcolato da `lpf_cap` con normalizzazione (`>> 1`) e mask (`& 0x0f`) + una costante `0xa3` + eventuale offset channel/BW
- `f` (8 bit) viene **duplicato** in due campi di 8 bit del valore 25-bit `v`: bit 9-16 e bit 17-24
- Il pre-state della cella (`bit 0-8` per lo, `bit 0` di hi tramite v bit 16) viene preservato

### Pre-state per gruppo di stage (osservato invariante d6220 e agcombo)

| Stage | pre_lo |
|---|---|
| 0, 1, 2, 8 | `0x00db` |
| 3, 4, 5    | `0x0123` |
| 6, 7       | `0x016b` |

`pre_hi` è sempre 0 nei bit rilevanti (bit 0-8 di hi). Il bit 0 di hi (= v bit 16) proviene da `f << 9` (bit 7 di f).

## Dati raccolti

### Vendor traces (wl-diag)

| Chip | Cattura | chan/BW | lpf_cap | f iniettato | delta da formula |
|---|---|---|---|---|---|
| d6220 | attach-to-bss-ch36 | 36/20 | 0xab | **0xa8** | **0** ✓ |
| d6220 | attach-to-bss-ch44 | 44/20 | 0xab | **0xa9** | **+1** |
| d6220 | attach-to-bss-ch36-bw40 | 36/40 | 0xab | **0xaa** | **+2** |
| d6220 | down-to-bss-up | 44/20 | 0xab | 0xa9 | +1 |
| agcombo | attach-to-bss (down-up ch36) | 36/20 | 0x58 | **0xaf** | **0** ✓ |
| agcombo | down-to-bss-ch36-bw40 | 36/40 | 0x58 | 0xaf | 0 |
| agcombo | down-to-bss-ch100-bw80 | 100/80 | 0x58 | 0xaf | 0 |

### Letture live via `wl phytable` (stesso silicio agcombo)

| Sessione | chan/BW | E | F | lpf_cap | Cella 0x142 | f estratto | delta |
|---|---|---|---|---|---|---|---|
| t0 | 36/20 | 0x0a56 | 0x0c1f | 0x58 | 0x5cdb | **0xae** | **-1** |
| t1 | 44/20 | 0x0a55 | 0x0c1e | 0x58 | 0x5edb | **0xaf** | **0** |
| t2 (ritorno ch36) | 36/20 | 0x0a56 | 0x0c1f | 0x58 | 0x5cdb | **0xae** | **-1** |

### Osservazioni per-stage (chip agcombo attuale, ch36/20)

| Cella | Valore osservato | Predetto (f=0xae, pre_lo dal gruppo) | Match |
|---|---|---|---|
| stage 0 lo (0x142) | 0x5cdb | 0x00db \| (0xae << 9) = 0x5cdb | ✓ |
| stage 3 lo (0x145) | 0x5d23 | 0x0123 \| (0xae << 9) = 0x5d23 | ✓ |
| stage 6 lo (0x148) | 0x5d6b | 0x016b \| (0xae << 9) = 0x5d6b | ✓ |
| stage 8 lo (0x14a) | 0x5cdb | 0x00db \| (0xae << 9) = 0x5cdb | ✓ |
| stage 0 hi (0x362) | 0x015d | (0xae << 1) | (0xae bit 7 in bit 0) = 0x015d | ✓ |

## Ipotesi consolidate

### 1. La formula base è `f = 0xa3 + ((cap >> 1) & 0x0f)`
Confermata su:
- d6220 ch36/20 (delta 0)
- agcombo vendor cattura ch36/20 (delta 0)

`0xa3` è probabilmente una **costante letterale nel blob**, base del campo iniettato.
`((cap >> 1) & 0x0f)` scarta LSB e prende i 4 bit successivi — coerente con il fatto che gli LSB di lpf_cap sono rumore di calibrazione (jitter RCcal).

### 2. Componente delta channel/BW/silicon
Su d6220 (radio_rev 0, chip 2069 base):
- `chan==44 → +1`
- `bw==40 → +2`
- Vedibile perché d6220 nel vendor esegue rccal UNA VOLTA a boot → lpf_cap invariante tra catture

Su agcombo (chip 2069ac):
- Nelle 3 catture vendor (ch36/20, ch36/40, ch100/80): sempre `delta = 0`
- Sul chip attuale (stesso silicio delle catture): `delta = -1` su ch36 e `delta = 0` su ch44

Il fatto che lo **stesso silicio** produca delta diversi in condizioni diverse (ch36: -1 vs 0) suggerisce una **componente termica** (compensazione temperatura silicio) più che una tabella NVRAM statica per canale/BW.

### 3. rccal ri-eseguita ad ogni channel switch (agcombo)
Su agcombo il chip legge E, F e ricalcola lpf_cap ad ogni `chanspec` set. Il valore letto ora via `wl radioreg 0x414/0x415` è quello dell'**ultima** rccal, non necessariamente quella usata per la scrittura iniziale al boot.

Su d6220 la rccal è eseguita solo a `radio_2069_init` (a boot), quindi lpf_cap rimane costante tra channel switch.

### 4. Deterministic per canale (a temperatura costante)
Test del `chanspec 36 → chanspec 44 → chanspec 36`:
- ch36 t0 → 0x5cdb (E=0x0a56, F=0x0c1f)
- ch44 t1 → 0x5edb (E=0x0a55, F=0x0c1e)
- ch36 t2 → **0x5cdb** (E=0x0a56, F=0x0c1f) — **identico a t0**

Cioè la scrittura è **deterministica per (canale, temperatura)** — non è drift random.

## Cosa ancora non sappiamo

1. **Formula esatta del delta channel/BW/temp**: la tabella lookup che produce `+1` su ch44, `+2` su BW40 per d6220 non è nota. Servirebbero più letture cross-canale su d6220 vero per confermare il pattern.

2. **Sensore temperatura**: nel 2057 (usato in alcuni N-phy) esiste `R2057_TEMPSENSE_CONFIG = 0x00c`. Nel 2069 non abbiamo trovato ancora. Se esiste ed è letto durante channel_setup, il valore contribuirebbe al delta.

3. **La costante `0xa3`**: da confermare come "letterale" nel blob wl.ko. Verifica possibile con `strings wl.ko | grep -E "^0x[0-9a-f]{4}$"` o disassembly della funzione `wlc_phy_txbb_lpf_setup` (o simile).

4. **Se `delta` include il canale su agcombo**: sul chip attuale ch36 e ch44 producono delta diversi (-1 vs 0). Ma non sappiamo se è canale-dependent o rccal-dependent (agcombo ri-esegue rccal ad ogni switch, quindi le due variabili sono correlate).

## Implicazioni per il codice del scratch

Al momento il scratch usa `hardcode = valori vendor letterali` per il call site di `channel_setup` (`stages=0x1ff, f0/f3/f6=-1, f9=f17=lpf_cap`). Questo produce match strict `1673 op` con la trace vendor d6220 ch36 BW20.

Quando avremo accesso ai log `[TXLPFLOG]` da hardware reale (con la nuova versione che emette `f_predicted`, `f_actual`, `delta`), potremo:
1. Confermare la formula `f = 0xa3 + ((cap >> 1) & 0x0f) + delta_ch_bw`
2. Popolare la tabella `delta_ch_bw` per i chip di interesse (d6220, agcombo)
3. Sostituire l'hardcode con la formula parametrizzata

Nel frattempo l'hardcode è la scelta corretta — è testabile, deterministico e non pretende di conoscere una formula che ancora ci sfugge in parti.
