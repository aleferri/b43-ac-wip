# Indice delle differenze — DSL-3580L (4352 wl6.30) vs d6220 (4352) vs agcombo (4360)

Mappa funzione-per-funzione delle differenze fra i tre testimoni, come roadmap
di verifica. **Nessuna modifica al codice**: solo classificazione.

## Triangolazione

| board | chip | wl | flow catturato |
|---|---|---|---|
| d6220 | 4352 | recente (riferimento del port) | attach-to-bss ch36 |
| DSL-3580L | 4352 | 6.30.102.7 (vecchia) | down-to-bss ch36 (bw20/bw40) |
| agcombo | 4360 | 7.14.43 | down/rescan-to-bss ch36 |

Due 4352 con wl diverse + un 4360. Regola di lettura:
- **[CHIP]** — i due 4352 concordano, il 4360 diverge.
- **[VERSIONE]** — i due 4352 divergono fra loro (stesso chip, wl diversa).
- **[MISURA]** — valori analogici (cal/gain) che variano per esemplare: diversi per natura.
- **[FLOW]** — differenza dovuta a down-vs-attach, non al driver.

## Limiti (leggere prima di fidarsi delle righe)

1. **Flow diversi.** DSL = down-to-bss, d6220 = attach. Init e PLL "da zero",
   frontend completo e ricarico tabelle non sono allineati: il down non
   riprogramma ciò che è già su. Le fasi di programmazione completa vanno
   verificate con un **attach del DSL**, non con questa cattura.
2. **Segmentazione fragile.** Le firme di fase (dalla mappa macro, flow
   d6220/agcombo) slittano sul DSL: 8 fasi su 16 non si localizzano o danno
   conteggi gonfi (artefatti, non differenze). Segnate sotto come
   "non confrontabile".
3. **Tre versioni wl.** Non c'è un asse "versione" pulito: d6220 e agcombo hanno
   wl diverse fra loro, quindi [VERSIONE] qui = "il DSL 6.30 diverge dagli altri
   due", non un contrasto a parità di resto.

## Reperti puntuali verificati (ancorati a dati certi)

| funzione / punto | differenza osservata | classe | stato |
|---|---|---|---|
| `op_init` frontend, radio `0x0033` | d6220+agcombo: 3 write `0x4060→0x4161→0x4181` (alto `0x4000`); DSL: 1 write `0x60b1` (alto `0x6000`) | [VERSIONE] | **DA VERIFICARE**: il maskset `~0xf000/0x4000` del port cancella il `0x6000`; capire se quel nibble è config o residuo |
| `bcma_pmu_pll_init` / `op_init` PLLCTL3 | 4352 = `0x00133333` (letto), 4360 = `0x100e` (scritto) | [CHIP] | **RISOLTA**: 0007 scrive solo sul 4360; op_init verifica sul 4352 e bail se diverso |
| PLLCTL2 | `0x0c31` su entrambi i chip | comune | ok |
| `bcma_pmu_resources_init` max_res_mask | `0x7ff` su 4352 (DSL) e 4360 (agcombo) | comune | **RISOLTA/confermata** su hardware |
| coeff RX-IQ per-core `0x06a0/06a1/08a0/08a1` | valori diversi (DSL 0x002a/005f/0036/0030) | [MISURA] | atteso, non confrontabile 1:1 |
| readback radio `0x0407` | `0x8302` su tutti | comune | ok (read-plan corretto) |
| readback radio `0x08ed` | stessi valori, ordine per-core diverso | per-core | atteso |
| readback radio `0x040c` | d6220 read-plan `0x0200`; DSL `0x0210` | [?] | **DA VERIFICARE** |
| readback radio `0x0416` | d6220 read-plan `0x0009`; DSL `0x0006`/`0x0186` | [?] | **DA VERIFICARE** |
| read-plan `r33` (0x0033) | imbocca background `0x0060`; hw reale `0x6061`/`0x6060` | test debole | **NOTA**: read-plan tarato su pre-stato inesistente; port corretto (maskset preserva) |

## Differenze [CHIP] strutturali (fasi che si segmentano in modo affidabile)

Confermano i condizionali `is4360` già presenti nel port: sono registri che il
**4360 tocca e i due 4352 no**.

| fase | registri 4360-only | classe | stato |
|---|---|---|---|
| `chan_tables` | banco PHY `0x0b10..0x0b13`, `0x0b27`, `0x0b3c`; `0x0aa0/0aa1` (coeff core-2); RAD `0x0402..0405`; TBL 0x07 off `0x016a/038a/03ed`; TBL 0x0c off `0x0068/006a` | [CHIP] | attesa (core-2 / 4360) |
| `rxcal_radio_setup` | RAD `0x040e/0417/0424/0425/055f/0561/056e` (read+mod+write) | [CHIP] | attesa |
| `rxcal_tone_setup` | banco PHY `0x0b2x/0x0b3x`; RAD `0x040e`(×8), `0x056e`(×8), `0x041a`(×3) | [CHIP] | attesa |
| `chan_tables` / `rxcal_*` | `0x0072` toccato 2/6/3 volte (d6220/dsl/agcombo) | [tutti diversi] | **DA VERIFICARE** |

## Fasi non confrontabili con le catture attuali

Segmentazione slittata (conteggi artefatti) o firma non trovata nel DSL
down-to-bss — servirebbe un **attach del DSL** per un confronto affidabile:

`phy_channel_setup`, `post_noise_shaping`, `afecal`, `idle_tssi_meas`,
`txpwrctrl_setup`, `rxgainctrl_regs`, `rxcal_gaincal`, `rxiq_est`.

Nota: `rxgain_init` (dsl≈1005 vs 44) e `adc_reset` (dsl≈10020 vs 70) mostrano
conteggi gonfi = slittamento, **non** differenze reali.

## Priorità di verifica

1. `0x0033` nibble `0x6000` (DSL): config o residuo? — decide se il maskset del port è corretto sul DSL.
2. readback radio `0x040c`/`0x0416`: valori diversi fra i due 4352 → possibile [VERSIONE] o read-plan da ritarare.
3. `0x0072` (tutti diversi): unico registro con conteggio diverso su tutti e tre nelle fasi affidabili.
4. Cattura **attach del DSL** per sbloccare le 8 fasi non confrontabili + init/PLL da freddo.

## Riproducibilità

`reverse-tools/collapse_trace.py` sulle tre tracce, poi
`reverse-tools/macro_order_map.py <harness.collapsed> <vendor.collapsed>`
(firme di fase = mappa macro). I conteggi sono
del segmento collassato; le righe [CHIP] sono multiset di chiavi strutturali
(valori esclusi). Le fasi marcate non confrontabili sono escluse dai verdetti.
