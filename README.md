# b43 AC-PHY / radio 2069 — BCM4352-family bring-up

Porting `b43` per AC-PHY rev 1 / radio 2069 rev 4.
Target principale: D-Link DSL-3580L (`0x14e4:0x43b3`, BCM4352-family,
2×2, 5 GHz only). Secondo hardware: Netgear D6220 (stesso chip family) e
agcombo (BCM4360, 3×3 dual-band). Reverse dal binario `wl` OEM; le
code-path sono rifatte con dispatch chip-aware `{4352, 4360, default}`.

## Chip target

| Campo | Valore |
|---|---|
| PCI ID | `0x14e4:0x43b3` |
| chip / rev / corerev | `0x4352 / 0x3 / 0x2a` |
| radio | 2069 rev 4, PHY AC rev 1 |
| antenne | `aa5g=3` (2×2 attive), `aa2g=0` → 5 GHz only su DSL-3580L |
| SROM | rev 11, board `0x668`, `femctrl=6` |

## Obiettivo MVP

Probe + `ifconfig wlan1 up` + scan passivo + associate su AP 5 GHz ch 36
+ 6 Mbit OFDM 1×1. Out of scope MVP: HT/VHT, MIMO, 40/80 MHz, UNII-2/3.

## Stato corrente

Il **percorso deterministico** (init → radio enable → channel setup) è
completamente portato. DMA verificato funzionante su hardware. Probe e
init stabili, nessun crash. RX/associate non ancora confermati — retest
con gli ultimi fix (rxgain analogico, txpwrctrl riordinato, indice/tabella
7.x, RST2RX morto rimosso) in corso.

### Cosa funziona

- **SROM rev 11**: estrattore completo, validato dall'harness userspace
  (77 PASS / 0 FAIL su DSL, 74/0 su D6220, 75/0 su agcombo).
  Patch draft in `sprom-rev11/`, vedi il README lì.
- **op_init**: chip id, num_cores/coremask, mode_init, PMU regctl, GPIO,
  24 tabelle PHY init, init_regs.
- **op_software_rfkill**: radio_2069_init, pwron, rccal (3 passate),
  afe_lpf_stage, GPIO frontend, PA bias, RF front-end arm, PMU enable.
- **set_channel** completo: classifier/clip freeze → radio_2069_channel_setup
  → channel_setup (reset-time, afe_lpf, rfseq, rxcore_setstate) → farrow →
  chanspec_tail → coeff_bank → rx_evm_shaping → chan_tables (tbl 0x11/0x0b/
  0x15/0x44/0x45) → rxgain_init (SROM-driven) → afecal → adc_reset →
  idle_tssi → txpwrctrl_setup (est_pwr LUT da pa5ga SROM) → txpwr_by_index →
  rxgainctrl_regs → rx_enable (RST2RX kick) → MAC enable.
- **TX/RX core wiring** (patch 0009): txhdr/phy_ctl1, RX signal/band/freq,
  rate_memory, dummy_transmission.
- **DMA 64 KB alignment**, **PCI bridge ID**, **bcma PMU init** (PLL + resources).

### Cosa manca

| Blocco | Tipo | Impatto |
|---|---|---|
| **RX gain cal sweep** (WI-5, ~3000 ops) | CAL — serve algoritmo brcmsmac | Sospettato #1 per assenza RX: popola i gain table finali |
| **Poll-cal 0x0380** (WI-6, ~1100 ops) | CAL | TX power closed-loop |
| **Poll-cal 0x0270** (WI-7, ~350 ops) | CAL | Feed-forward gain table |
| **Radio-cal burst** (WI-8, ~70 ops) | quasi-DET | Piccolo, da confermare non-branching |
| **Helper WI-2/3/4** (~2200 ops) | DET — trascrivibili ora | Inner loop delle cal: gain-program, radio-gain bank, AFE override |
| **recalc_txpower / adjust_txpower** | stub vuoti | Watchdog periodico: CRS min-pwr recalc, gain cycling |
| **RXIQ calibration** | skeleton, gated off | Degrada EVM, non blocca RX base |
| PA bias in rfkill | hardcoded da cattura d6220 | TX power sbagliata su board diversi |
| idle-TSSI base index | seed catturato, non derivato | Errore non dominante |
| pdet LUT 960 byte | TODO | Solo TX closed-loop |

Dettagli e piano di lavoro per i WI: [`docs/porting-plan.md`](docs/porting-plan.md).

## Build e test

Prerequisiti: kernel locale con `B43_PHY_AC=y` (togliere `BROKEN`) +
patch `sprom-rev11/0001-*.patch` applicata (`git am`), firmware in
`/lib/firmware/b43/`, DSL-3580L, AP target 5 GHz ch 36, seriale TTY.

```
modprobe b43                          # smoke: dmesg deve dire 0x4352/0x43b3, sromrev=11
ifconfig wlan1 up                     # op_init + rfkill(unblocked) + set_channel
iw wlan1 scan freq 5180               # scan passivo UNII-1
```

Compilare con `B43_DEBUG=y` per log dettagliati (`b43dbg` su ogni fase).

## Struttura del repo

```
src/                     — sorgenti driver (fonte di verità, target: drivers/net/wireless/broadcom/b43/)
test/                    — harness userspace di verifica trace op-per-op
patches/                 — serie patch rigenerabile dai sorgenti
docs/                    — documentazione tecnica (INDEX.md dentro)
sprom-rev11/             — patch SROM rev 11 + harness userspace
reverse-output/          — trace annotate/collapsed, correlazione
reverse-tools/           — script Python (correlatore, estrattori, generatori) + tool C on-device
router-data/             — dump NVRAM/SROM/wl-diag per board (DSL, D6220, agcombo)
bring-up-logs/           — log runtime del driver portato (b43 open)
scripts/                 — helper (es. conversione patch per OpenWrt)
```

Per navigare la documentazione tecnica: [`docs/INDEX.md`](docs/INDEX.md).

## Post-MVP

- **Split 0006 per upstream**: la patch 0006 attuale (~7300 righe) va
  spezzata in ~9 commit da 300-500 righe prima della submission a
  `linux-wireless`. Schema in [`docs/driver-status.md`](docs/driver-status.md).
- **RX gain cal**: portare `rxgaincal`/`gainctrl` da brcmsmac — singolo
  pezzo che più probabilmente sblocca l'RX.
- **TX power reale**: txpwrctrl_setup già calcola la LUT da pa5ga SROM;
  manca il readback idle-TSSI → base index e il closed-loop runtime.
- **HT/VHT**: le 24 init tables coprono già OFDM; auditare late PHY writes.
- **Sub-band UNII-2/3**: channeltab copre 5170–5825 MHz, selezione pa5ga
  per sub-band già implementata (`pa5g_group()`).
- **Submission upstream sprom-rev11/**: pre-condizioni in `sprom-rev11/README.md`.
- **Co-load con wl0 N-PHY integrato**: i MAC sono differenziati, testabile
  solo a bring-up reale.
