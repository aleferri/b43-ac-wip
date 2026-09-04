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

Il numero di riferimento viene dal gate a freddo, che si lancia con
`test/gates.sh`. La procedura esatta, con le trappole, sta in
[`test/README.md`](test/README.md) e **non va reinventata**: e' l'unico posto
dove si documenta come si produce un numero citabile.

Su `cold01-ch36-bw20`, il segmento di riferimento:

```
grezzo: 27759/29030 = 95.62%
        259 col valore sbagliato, 753 op di wl mancanti, 0 op del port di troppo
```

Il denominatore e' l'unione dei due flussi, quindi fa 100% solo se il port
emette esattamente le op del driver stock: ne' meno, ne' di piu', ne' con
valori diversi. Le tre voci sono tre lavori distinti — una formula da trovare,
del codice da scrivere, un gate da mettere — e stanno in
[`docs/retrace-todo.md`](docs/retrace-todo.md). Sul segmento di riferimento la
terza voce e' a zero; resta aperta sui canali sopra i 5250 MHz.

Sui 26 segmenti dello sweep a freddo il punteggio parte da ~95% sui canali
UNII-1 a 20 MHz e scende sui canali alti e larghi. L'intervallo pieno e' da
rimisurare: dopo la chiusura dei poll di up sono stati rifatti solo quattro
segmenti (ch36, ch40, ch44 e ch52 a 20 MHz), gli altri 22 portano ancora i
numeri precedenti. La differenza fra canali non e'
qualita' del port su quei canali: sopra i 5250 MHz il driver stock **salta**
alcune calibrazioni che trasmettono, il port ne esegue ancora una parte, e le
op di troppo pesano. Cinque fasi sono gia' gatate, il resto e' tracciato.

Il secondo gate e' il tick del watchdog periodico contro l'oracolo a regime, e
sta a **`MATCH`** — confronto posizione-per-posizione, nessuna eccezione. E'
il rilevatore di regressioni piu' sensibile del repo e va rilanciato a ogni
modifica.

L'estrattore SROM rev 11 passa l'harness userspace su tutti i vettori: 77/0
(DSL), 74/0 (D6220), 75/0 (agcombo).

Su hardware il driver **non completa ancora `ifconfig up`**. L'unico run in
repo ([`bring-up-logs/`](bring-up-logs/)) e' sul DSL-3580L: probe, load
firmware 784.2, `op_init` e il load delle tabelle passano; `switch_channel`
parte, arriva alla scrittura della tabella txgain `id=0x20` e si ferma li'.
Diagnosi corrente: mancano dei delay prima di quella scrittura. Attenzione,
quel log precede la risoluzione della famiglia LPF — emette una diagnostica
`f_predicted/f_actual` che nei sorgenti non esiste piu' — quindi non fotografa
il codice attuale. Serve un run nuovo.

Cosa il confronto copre e cosa no: le op di tabella sono tracciate come
marcatore `TBL.WR id/off/len` **seguito dalle write dei singoli word** sul data
port (`PHY.WR 0x000f`), quindi anche il payload dei bulk e' confrontato
word-per-word.

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
- **`switch_channel`** (BW20, 5 GHz): freeze RX → `radio_2069_channel_setup`
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
- **Watchdog periodico** (`b43_phy_ac_watchdog`, hook `pwork_15sec`): poll
  TSSI/statistiche SHM con latch-and-clear della finestra SHM: il latch
  legge fino a 0x0314 e la clear si ferma a 0x0312, asimmetria che ogni
  cattura mostra e che e' voluta. Piu'
  una tornata singola del measure block a MAC sospeso — il ciclo su
  `0x0725/0x0925` che il vendor esegue ogni ~5 s a regime. Op-per-op
  contro il tick vendor (vedi [Verifica](#verifica-riproducibile)).
- **Core b43**: TX/RX wiring (`patches/0008`), allineamento DMA a 64 KB
  (0004), channel set 5 GHz dedicato (0003), PCI bridge ID (0009), bcma PMU
  init PLL + resources (0007).

Mappa file sorgente → patch: [`docs/driver-status.md`](docs/driver-status.md).

## Cosa manca

| Blocco | Stato | Impatto |
|---|---|---|
| Ricalcolo TX power periodico (`recalc_txpower`) | portata | Catena CRS min-power implementata (ladder + ancoraggio per-BW + bump a freddo, dal blob D6220 7.14). Su hardware manca solo il campione d'interferenza per freq_range che seleziona l'indice, oggi fissato al valore della config validata (ch36 BW20). `adjust_txpower` resta stub: mai invocata nel path MVP |
| `ppr[24]` (power reduction per-rate) | hardcoded dalla cattura D6220 ch36 | Derivazione da `mcsbw*po` SROM assente: TX power sbagliata su altri canali/board |
| Base index idle-TSSI | seed catturato, il readback viene scartato | Errore non dominante, ma non è board-independent |
| GPIO frontend 2-fase, PA bias per-core, PMU regctl enable finale | non implementati | Sono le op che il vendor emette solo a steady state |
| BW40 / BW80 | `switch_channel` ritorna `-EOPNOTSUPP` | Il codice c'e' ed e' confrontato contro i segmenti a 40 e 80 MHz con `make AC_ANY_CHANNEL=1`; quello che manca e' la validazione che apra il guard |
| 2.4 GHz | `op_switch_channel` ritorna `-EOPNOTSUPP` | Mappa radio 2G non validata |
| Canali ≠ 36 | 50 voci in channeltab (5170–5825 MHz), solo ch36 in `b43_phy_ac_validated_configs[]` | Il confronto gira su tutti e 26 i segmenti con `AC_ANY_CHANNEL=1`, che scavalca il guard e lo dice con un `b43warn`. Piano in [`docs/channel-generalization.md`](docs/channel-generalization.md) |
| `b43_phy_ac_rxiqcal()` (solver generico da brcmsmac) | gated da `REGMAP_FILLED == 0`, nessun chiamante nel driver | Il path RXIQ effettivo è quello trascritto dalla trace, già wirato |

### Bug aperti

Nessuno al momento.

Nota per chi legge il descrittore: `est_pwr_lut_core*` e
`papd_comp_rfpwr_tbl_core*` condividono id e offset **per costruzione**. Il
vendor scrive est_pwr e poi papd_comp_rfpwr sulle stesse celle nella stessa
sequenza di init (agcombo attach `#1345` e `#2899`). Non è un bug e non va
"corretto": rimuovere una delle due divergerebbe dal vendor.

## Verifica riproducibile

La procedura sta in [`test/README.md`](test/README.md), in sei passi. Qui solo
i due gate, per averli a portata:

```sh
cd test
unzip -d /tmp/cold ../router-data/d6220/cold-sweep.zip
make
./gates.sh                                    # cold a freddo: grezzo + prima divergenza
./gates.sh /tmp/cold/segmenti/cold*.txt       # tutti e 26

AC_READ_ORACLE=../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt \
    ./ac_trace periodic d6220 > /tmp/p.out
python3 compare.py \
    ../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt /tmp/p.out
# MATCH
```

Le catture in `router-data/*/` sono lo sweep a freddo e quello a caldo negli
zip, piu' l'oracolo del tick a regime e i dump statici (NVRAM, SROM, tabelle
PHY, revinfo). Le catture singole di attach citate nelle versioni precedenti di
questo file non esistono piu': sono state sostituite dai segmenti degli sweep,
che coprono ogni canale e larghezza invece di uno.

## Build e test su hardware

Prerequisiti: kernel locale con la serie `patches/` applicata (`git am`,
include l'estrazione SROM rev 11 e togliere `BROKEN` da `B43_PHY_AC` nel
Kconfig), `CONFIG_B43_PHY_AC=y`, firmware in `/lib/firmware/b43/`, AP target
5 GHz ch 36, seriale o netconsole.

```
modprobe b43                          # smoke: dmesg deve dire 0x4352/0x43b3, sromrev=11
ifconfig wlan1 up                     # op_init + rfkill(unblocked) + switch_channel
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
