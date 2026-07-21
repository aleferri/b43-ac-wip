# Piano di porting — percorso `down → bss-up`

Trace di riferimento: `wl-diag-d6220-4352-wl1-down-to-bss-up.txt` (d6220,
BCM4352, ACPHY/2069). La correttezza del port si verifica col **match per
sequenza** dell'harness (`test/compare.py` contro la cattura vendor, es.
`set_channel` nel range validato); non con metriche di copertura aggregate.

## Copertura

| | op | quota |
|---|---:|---:|
| correlati (EXACT/UNIQUE/MULTI/RMW-WB) | 5 603 | 41.2% |
| mancanti (NONE) | 7 995 | 58.8% |
| — di cui deterministici (trascrivibili) | ~3 400 | ~43% del mancante |
| — di cui calibrazione (serve algoritmo) | ~4 600 | ~57% del mancante |

I NONE PMU/GPIO (11 op) sono coperti da chiamate bcma non tracciate dal
correlatore.

## Work item

| WI | stato | tipo | ~op | che cos'è |
|----|-------|------|----:|-----------|
| **WI-0** | ✅ CHIUSO | — | — | Già implementato (prefregs + per-core init + rccal). Bug `RCCAL_P1_B` 0x04a→0x043 corretto. Pass 2 completata: setup+run+disarm ora eseguiti (prima mancavano, G era letto dal residuo pass 1). |
| **WI-1** | ✅ CHIUSO | DET | 103 | `afe_lpf_stage` wirata: 1ª call in rfkill (afe_728=0x0800), 2ª in channel_setup (0x0000). |
| **WI-2** | da fare | DET | 720 | Gain-program MOD per-core (0x0725/0x0925/0x073a…). Inner loop di WI-5. |
| **WI-3** | da fare | DET | 1152 | Radio gain MOD bank (RAD 0x016e/0x000e/0x036e/0x020e) per-core. |
| **WI-4** | da fare | DET | 332 | AFE MOD burst (0x0724/0x0736/0x0924…). |
| **WI-5** | da fare | **CAL** | ~3000 | RX gain/RSSI cal sweep: ~18 iter/canale, misura→aggiusta. **Sospettato #1 per assenza RX.** Porta da `wlc_phy_rxgaincal_acphy` / brcmsmac. |
| **WI-6** | da fare | **CAL** | ~1130 | Poll-cal su 0x0380 (idle-TSSI / PAPD). |
| **WI-7** | da fare | **CAL** | ~350 | Poll-cal su 0x0270 (RXIQ estimator → gain table, non IQ comp). |
| **WI-8** | da fare | quasi-DET | 69 | Radio-cal write burst (RAD 0x0021/0x0023/0x003d). Confermare non-branching. |

## Anatomia del sweep WI-5

Ogni iterazione (~572 seq, ripetuta ~18×/canale):

```
WI-2 (det)  20× PHY MOD 0x0725/0x0925     programma gain stage
WI-5 (cal)   6× PHY RD  0x0921/0x0920      legge stato core
WI-2 (det)  20× PHY MOD 0x0725…
WI-5 (cal)  13× RAD RD  0x0161…            legge radio
WI-5 (cal)  22× RAD wr/mod 0x0361/0x015f   dipende dalle letture
WI-2/3      9× RAD wr
WI-3 (det)  64× RAD MOD 0x016e/0x000e      bank per-core
WI-5 (cal) 130× PHY RD  0x07af/07b3…       misura gain/RSSI per-core
```

Conseguenza: **WI-2/3/4 vanno scritti prima** come helper puri. WI-5 li
chiama dentro il proprio loop; la logica del loop (quante iterazioni,
cosa scrive in funzione delle letture) **deve venire dall'algoritmo di
riferimento**, non dalla trace.

## Piano a fasi

**Fase A — deterministico** (recupera ~3 400 ops → NONE da ~8000 a ~4600):
WI-2, WI-3, WI-4, WI-8. Verificabile: correlatore deve dare EXACT sulle
nuove op.

**Fase B — calibrazioni** (~4 600 ops restanti):
WI-5 (priorità: sblocca RX), poi WI-6, WI-7. Verificabile solo su
hardware (struttura/registri coerenti con trace + funzionamento reale).
