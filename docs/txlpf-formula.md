# Analisi reverse-engineering formula LPF (celle table 7) — RISOLTA

## Stato

La famiglia LPF analogica (TX-LPF, RX-LPF, DACBUF) è **risolta**: il cap è
derivato da rccal e la RMW preserva il pre-state della cella. Verificata sui
tre board (d6220, DSL, agcombo). Formule finali:

- **TX-LPF**: `cap = ((RCCAL_F - RCCAL_E) * 193) >> 8` (RCCAL_E=0x414,
  RCCAL_F=0x415). `lo = pre_lo | (cap<<9)`, `hi = pre_hi | (cap<<1)`.
- **RX-LPF**: `f17 = lpf_cap1` diretto; `f6 = (lpf_cap0 * k[stage]) >> 8`,
  `k = {221, 215, 215}` per le tre sezioni. Le wl recenti scalano; la wl 6.30
  del DSL usa `k = 256` (nessuno scaling) — differenza di versione. Il
  coefficiente 221 vs 222 / 215 vs 216 è ambiguo su due campioni: TODO validare
  con un terzo `lpf_cap0` (altro canale).
- **DACBUF**: `dacbuf_cap = (RCCAL_G & 0x03e0) >> 5` (RCCAL_G=0x416), dal
  readback **post-apply** (il primo readback è pre-apply e dà cap 0).

## Dati di verifica

### Letture live via `wl phytable` (silicio agcombo)

| Sessione | chan/BW | E | F | lpf_cap | Cella 0x142 |
|---|---|---|---|---|---|
| t0 | 36/20 | `0x0a56` | `0x0c1f` | `0x58` | `0x5cdb` |
| t1 | 44/20 | `0x0a55` | `0x0c1e` | `0x58` | `0x5edb` |
| t2 (ritorno ch36) | 36/20 | `0x0a56` | `0x0c1f` | `0x58` | `0x5cdb` |

La scrittura e' deterministica per (canale, temperatura): t2 riproduce t0
esattamente, non e' drift.

### Pre-state per gruppo di stage (invariante su d6220 e agcombo)

| Stage | pre_lo |
|---|---|
| 0, 1, 2, 8 | `0x00db` |
| 3, 4, 5 | `0x0123` |
| 6, 7 | `0x016b` |

`pre_hi` e' 0 nei bit rilevanti; il bit 0 di hi arriva dal bit alto del cap.

### Riscontro per-stage (agcombo, ch36/20, cap `0xae`)

| Cella | Osservato | `pre_lo \| (cap << 9)` |
|---|---|---|
| stage 0 lo (`0x142`) | `0x5cdb` | `0x5cdb` |
| stage 3 lo (`0x145`) | `0x5d23` | `0x5d23` |
| stage 6 lo (`0x148`) | `0x5d6b` | `0x5d6b` |
| stage 8 lo (`0x14a`) | `0x5cdb` | `0x5cdb` |
| stage 0 hi (`0x362`) | `0x015d` | `(cap << 1)` = `0x015d` |

### rccal: quando viene rieseguita

Sull'agcombo il driver stock rilegge E/F e ricalcola il cap a ogni set di
`chanspec`, quindi `wl radioreg 0x414/0x415` riflette l'**ultima** rccal, non
quella usata alla scrittura iniziale. Sul d6220 la rccal gira solo in
`radio_2069_init`, e il cap resta costante fra i channel switch.

## Ipotesi respinta: `f = 0xa3 + ((cap >> 1) & 0x0f) + delta_ch_bw`

Registrata perche' e' costata tempo e non va ritentata. Tornava su d6220 ch36/20
e su agcombo ch36/20, ma richiedeva un termine correttivo `delta` per ogni altra
configurazione (`+1` su ch44, `+2` su BW40 sul d6220) e sullo **stesso silicio**
agcombo dava delta diversi fra ch36 e ch44. Il modello rccal spiega gli stessi
punti senza termine correttivo: il cap e' una misura, e cio' che sembrava un
delta per canale era la rccal rieseguita.

## Cosa resta aperto

Solo il coefficiente RX-LPF: `221` contro `222` e `215` contro `216`, ambiguo su
due campioni. Serve un terzo `lpf_cap0` da un altro canale 5 GHz. Non riguarda
il TX-LPF, che e' chiuso.

## Nel codice

`b43_radio_2069_rccal` (radio_2069.c) legge `R2069_RCCAL_E/F` e calcola il cap,
popolando `lpf_cap0`/`lpf_cap1`; `set_analog_tx_lpf` li usa come `f9`/`f17`, e
il percorso RX-LPF scala `f6` con `rx_k[stage]`.
