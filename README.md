# b43 AC-PHY / radio 2069 — BCM4352-family bring-up

Porting `b43` per AC-PHY rev 1 / radio 2069 rev 4, reverse dal binario `wl`
OEM. Le code-path sono rifatte con dispatch chip-aware
`{4352, 4360, default}`. Tre board testimoni, con ruoli distinti:

- **Netgear D6220** (BCM4352, 2×2) — cattura di riferimento: è contro la sua
  trace che il flow è validato op-per-op, ed è il target del primo bring-up.
- **D-Link DSL-3580L** (BCM4352, 2×2, 5 GHz only) — è la board su cui il
  driver portato gira davvero. Monta una `wl` più vecchia (6.30), quindi
  alcune fasi divergono dalla cattura di riferimento.
- **agcombo** (BCM4360, 3×3 dual-band) — terzo testimone, serve a separare
  le differenze di chip da quelle di versione del blob.

## Chip target

| Campo | Valore |
|---|---|
| PCI ID | `0x14e4:0x43b3` |
| chip / corerev | `0x4352 / 42 (0x2a)` |
| radio | 2069 rev 4, PHY AC rev 1 |
| SROM | rev 11, `boardtype=0x668`, `femctrl=6`, `subband5gver=0x4` |
| antenne (DSL-3580L) | `aa5g=3`, `aa2g=0` → 5 GHz only; `txchain=rxchain=3` |

## Obiettivo MVP

Probe + `ifconfig wlan1 up` + scan passivo + associate su AP 5 GHz ch 36
+ 6 Mbit OFDM 1×1. Out of scope MVP: HT/VHT, MIMO, 40/80 MHz, 2.4 GHz.

## Stato corrente

Due risultati sono riproducibili in questo repo (vedi
[Verifica](#verifica-riproducibile)):

- `set_channel` su D6220 ch36 combacia **op-per-op** con la cattura vendor:
  22268/22268 operazioni, zero divergenze.
- L'estrattore SROM rev 11 passa l'harness userspace su tutti i vettori:
  77/0 (DSL), 74/0 (D6220), 75/0 (agcombo).

Cosa il match copre e cosa no: le op di tabella sono tracciate come marker
`TBL.WR id/off/len` **seguito dalle write dei singoli word** sul data port
(`PHY.WR addr=0x000f`), quindi anche il payload dei bulk è confrontato
word-per-word. Resta fuori solo ciò che il flow non esegue: il load delle
tabelle init di `op_init`, che ha un flow separato e per cui non esiste
oracolo (vedi i bug aperti).

Su hardware il driver **non completa ancora `ifconfig up`**. L'unico run
in repo ([`bring-up-logs/`](bring-up-logs/)) è sul DSL-3580L: probe, load
firmware 784.2, `op_init` e il load delle tabelle passano; `set_channel` parte,
arriva alla scrittura della tabella txgain `id=0x20` e si ferma lì.
Diagnosi corrente: mancano dei delay prima di quella scrittura. Attenzione,
quel log precede la risoluzione della famiglia LPF — emette una diagnostica
`f_predicted/f_actual` che nei sorgenti non esiste più — quindi non
fotografa il codice attuale. Serve un run nuovo.

Copertura per-funzione del bring-up radio (`rfkill`), misurata con
`coverage_by_function.py`: d6220 e agcombo 100% con zero gap; DSL con le
divergenze note (`prefregs` 91.7%, `rccal` 84.2%, `afe_lpf_stage` 4.2%).

`op_init` ha il suo gate contro `router-data/agcombo/wl-diag-wl1-attach.txt`,
la sola cattura in repo che contenga il load tabelle: `set_pdet_on_reset`,
`pre_init_frontend` e `mode_init` al 100%, `tables_init` **3714/3714** su una
span continua (`#356..#4069`), zero op vendor non attribuite. `init_regs` va
misurata sulle catture `down→bss` (17/17 su d6220, 33/33 su agcombo) perché
la attach parte dopo la sua finestra.

Il match op-per-op vale per D6220/ch36/BW20. Cosa questo implica sugli
altri board, canali e bandwidth è in
[`docs/driver-status.md`](docs/driver-status.md); le divergenze note del
bring-up radio (tutte sul DSL, wl 6.30) sono in
[`docs/retrace-todo.md`](docs/retrace-todo.md).

## Cosa è portato

- **SROM rev 11**: estrattore + harness userspace in
  [`sprom-rev11/`](sprom-rev11/). La patch della serie è `patches/0001`;
  `sprom-rev11/0001-*.patch` è un draft più ampio per l'upstream, non un
  prerequisito di build (vedi il README lì).
- **`op_init`**: chip/PLL check, probe dei core, frontend pre-init, PMU
  regctl, GPIO, `mode_init`, il load tabelle (15 popolate + 8
  azzeramenti, una sola lista in ordine vendor),
  `init_regs`, config MHF, `mac_suspend`.
- **`op_software_rfkill`**: `radio_2069_init`, `pwron`, `rccal` (3 passate,
  cap LPF e DACBUF derivati dalle misure), `afe_lpf_stage`. GPIO frontend,
  PA bias e PMU enable finale sono *fuori* dallo scope di rfkill e non sono
  ancora implementati.
- **`set_channel`** (BW20, 5 GHz): freeze RX → `radio_2069_channel_setup`
  → `channel_setup` (reset-time, AFE/LPF, RF sequencer, `rxcore_setstate`,
  farrow, chanspec tail, coeff bank) → `chan_tables` → noise shaping
  (tbl 0x15/0x0b/0x44/0x45 + `rxgain_init` per-core) → BW select →
  `reset_cca` → `afecal` → `adc_reset` → idle-TSSI → 2× `txpwrctrl_setup`
  (LUT est_pwr da `pa5ga` SROM) → `rxgainctrl_regs` → setup radio/tone e
  sweep `gainctrl` per-core → cleanup.
- **Calibrazioni post-channel**, invocate da `op_switch_channel` dopo
  `mac_enable`: `post_cal_finalize` iter 2/3, RXIQ apply + stage 2, cal AFE
  RX, i due round `txpwr`/`rxgain` con misura RXIQ, il loop
  `gainctrl_final_apply`, teardown RXIQ.
- **Core b43**: TX/RX wiring (`patches/0008`), allineamento DMA a 64 KB
  (0004), channel set 5 GHz dedicato (0003), PCI bridge ID (0009), bcma PMU
  init PLL + resources (0007).

Mappa file sorgente → patch: [`docs/driver-status.md`](docs/driver-status.md).

## Cosa manca

| Blocco | Stato | Impatto |
|---|---|---|
| Calibrazione periodica (`recalc_txpower`, `adjust_txpower`) | stub vuoti | Nessun ricalcolo CRS min-power né il ciclo ~10 s su `0x0725/0x0925`: sessioni lunghe driftano |
| `ppr[24]` (power reduction per-rate) | hardcoded dalla cattura D6220 ch36 | Derivazione da `mcsbw*po` SROM assente: TX power sbagliata su altri canali/board |
| Base index idle-TSSI | seed catturato, il readback viene scartato | Errore non dominante, ma non è board-independent |
| GPIO frontend 2-fase, PA bias per-core, PMU regctl enable finale | non implementati | Sono le op che il vendor emette solo a steady state |
| BW40 / BW80 | `set_channel` ritorna `-EOPNOTSUPP` | — |
| 2.4 GHz | `op_switch_channel` ritorna `-EOPNOTSUPP` | Mappa radio 2G non validata |
| Canali ≠ 36 | 50 voci in channeltab (5170–5825 MHz), solo ch36 validato | Piano in [`docs/channel-generalization.md`](docs/channel-generalization.md) |
| `b43_phy_ac_rxiqcal()` (solver generico da brcmsmac) | gated da `REGMAP_FILLED == 0`, nessun chiamante nel driver | Il path RXIQ effettivo è quello trascritto dalla trace, già wirato |

### Bug aperti

- **Log `[TXLPFLOG]` a `b43info`**: sette call site, di cui due dentro
  `b43_actab_write_bulk`/`read_bulk`, cioè una riga di dmesg non
  condizionata per ogni bulk di tabella. Erano la strumentazione con cui è
  stata risolta la famiglia LPF; ora che le formule sono chiuse vanno via.

Nota per chi legge il descrittore: `est_pwr_lut_core*` e
`papd_comp_rfpwr_tbl_core*` condividono id e offset **per costruzione**. Il
vendor scrive est_pwr e poi papd_comp_rfpwr sulle stesse celle nella stessa
sequenza di init (agcombo attach `#1345` e `#2899`). Non è un bug e non va
"corretto": rimuovere una delle due divergerebbe dal vendor.

## Verifica riproducibile

Match op-per-op del flow `set_channel` contro la cattura vendor:

```sh
cd test && make
./ac_trace set_channel d6220 > trace.switch.d6220.out
python3 compare.py \
    ../router-data/d6220/wl-diag-wl1-attach-to-bss-ch36.txt \
    trace.switch.d6220.out --range 32887:55154 --auto-align
# vendor: 22268 ops / test: 22268 ops / MATCH
```

Sequenza del load tabelle di `op_init` contro la cattura vendor:

```sh
AC_FN_MARKERS=1 ./ac_trace op_init agcombo > gen.op_init.agcombo.txt
python3 ../reverse-tools/coverage_by_function.py \
    gen.op_init.agcombo.txt \
    ../router-data/agcombo/wl-diag-wl1-attach.txt
# tables_init 3714/3714 100.0%  #356..#4069 / 0 op nei gap
```

Estrattore SROM rev 11 sui tre board:

```sh
cd sprom-rev11/harness && make
make check            # DSL-3580L  → 77 PASS / 0 FAIL
make check-d6220      # D6220      → 74 PASS / 0 FAIL
make check-agcombo    # agcombo    → 75 PASS / 0 FAIL
```

Dettagli su range, allineamento, read plan e copertura per funzione:
[`test/README.md`](test/README.md).

## Build e test su hardware

Prerequisiti: kernel locale con la serie `patches/` applicata (`git am`,
include l'estrazione SROM rev 11 e togliere `BROKEN` da `B43_PHY_AC` nel
Kconfig), `CONFIG_B43_PHY_AC=y`, firmware in `/lib/firmware/b43/`, AP target
5 GHz ch 36, seriale o netconsole.

```
modprobe b43                          # smoke: dmesg deve dire 0x4352/0x43b3, sromrev=11
ifconfig wlan1 up                     # op_init + rfkill(unblocked) + set_channel
iw wlan1 scan freq 5180               # scan passivo UNII-1
```

Compilare con `B43_DEBUG=y` per i log `b43dbg` di ogni fase.

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

- **Split della patch 0006** prima della submission a `linux-wireless`:
  schema in [`docs/driver-status.md`](docs/driver-status.md).
- **TX power reale**: derivazione di `ppr[]` dallo SROM, base index
  idle-TSSI dal readback, closed-loop runtime.
- **HT/VHT**: le init tables coprono già OFDM; auditare le late PHY writes.
- **Generalizzazione canale/BW**: piano in
  [`docs/channel-generalization.md`](docs/channel-generalization.md).
- **Submission `sprom-rev11/`**: pre-condizioni in `sprom-rev11/README.md`.
- **Co-load con wl0 N-PHY integrato**: probe di entrambi i PHY già
  osservato nei log di bring-up (`b43-phy2` N, `b43-phy3` AC); il resto è
  testabile solo a bring-up completo.
