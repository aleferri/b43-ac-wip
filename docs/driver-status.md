# Stato del bring-up

## Cosa il driver accetta

`op_switch_channel()` accetta ogni canale e larghezza a 5 GHz che la channel
table conosce. Il port non ha un filtro per configurazione: niente va upstream
finche' la radio non e' a posto su tutti i canali, e il punteggio per canale
dello sweep e' la misura di quanto manca.

Chip `0x4352` e `0x4360`, altrimenti `-EOPNOTSUPP`. Board di
riferimento NetGear D6220 (radio 2069 rev 4), piu' DSL-3580L e agcombo nelle
catture.

## Cosa le suite misurano

Le tre condizioni di `test/unit/README.md`, piu' `test/integration`. Il numero
citabile e' il `grezzo` di `cmp_skip.py`.

Tutti i numeri qui sotto sono sullo sweep **ricatturato**, che traccia nove
classi di hook in piu' del precedente. Non sono confrontabili con quelli che
questo documento riportava prima: il denominatore e' cresciuto di op che
nessuno dei due lati emetteva, quindi il vecchio 99.90% e l'attuale 91.75%
misurano confronti diversi, non due stati del driver.

| misura | esito |
| --- | --- |
| freddo `cold01` ch36 bw20 | **94.64%** (28929/30567), 158 regioni |
| freddo, non-DFS a 20 MHz | 98.47% (ch40), 98.63% (ch44); ch149 e ch153 da rimisurare |
| freddo, i 27 segmenti con guardia radar | non misurabili: vedi sotto |
| caldo, i tre segmenti di riferimento | 84.92% (ch36); ch52 e ch104 non misurabili |
| tick periodico | **MATCH**, posizione per posizione |
| integrazione, b43 intero | `probe: 0`, `start: 0`, 29853 op; 84.21% sul segmento intero, **98.78% sul solo switch di canale con zero valori sbagliati** |

Il freddo e il caldo non sono intercambiabili e nessuno dei due copre l'altro:
ogni predicato che distingue il primo bring-up dai successivi e' invisibile
nello sweep a freddo. Vedi `test/unit/gates.sh`.

Le due famiglie di canali a freddo non sono "sotto e sopra i 5250 MHz": sono i
canali con la guardia radar e quelli senza. Lo sweep vecchio non poteva
distinguerle, perche' si fermava a ch140. Il driver legge il flag giusto,
`IEEE80211_CHAN_RADAR`, e dietro `b43_phy_ac_may_calibrate_tx()` salta le
calibrazioni post-switch finche' il check e' pendente; il poll del rivelatore
non sta piu' dietro quel predicato, perche' e' monitoraggio in servizio e
continua anche dopo che il check si e' chiuso.

I 27 segmenti con la guardia radar non producono un numero. La ricattura li ha
presi con il CAC che si chiude a meta' segmento, e l'harness ha `cac_pending`
come booleano per l'intera corsa: il vendor si ferma a `op_channel_calibrate`
per tutta l'attesa facendo girare il watchdog -- su `cold05` sono 10867 op in
62.9 s, inserite esattamente dove `cold01` ha l'entrata di quella funzione --
e poi riprende. Serve una fase d'attesa nel flow, non una leva. Il residuo per
canale sta in `retrace-todo.md`.

**Nessun segmento si ferma piu' su un'operazione mancante o di troppo**: la
prima divergenza posizionale di tutti e 25 e' un valore, e sono due famiglie --
il banco `0x0910` su nove segmenti, il blocco ppr per rate sugli altri. Questo
non vuol dire che non resti struttura: `compare.py` si ferma alla prima
divergenza e cio' che sta a valle non lo misura, mentre `cmp_skip.py` conta
ancora 29 mancanti sui segmenti migliori -- lo stesso fondo di `cold01`, che ne
ha 29 e zero di troppo -- e molte di piu' sui quattro sotto il 98%.

## Cosa resta aperto

- **I valori sbagliati crescono con la larghezza**: 0 su ch36 bw20, 8 su ch52
  bw20, 93 su ch36 bw80. E' la voce piu' grossa del residuo a freddo.
- **La spazzata dei contatori sui giri in ritardo**: 12-36 op di troppo su
  cinque segmenti, il residuo del latch della finestra statistiche che e'
  chiuso. Misure e due tentativi falliti in `retrace-todo.md`.
- **Quando cade il blocco CRS**: il valore delle soglie e del banco `0x0910`
  torna ovunque, il punto in cui il vendor le scrive no, su 12 segmenti a
  freddo tutti con la guardia radar. Vedi `crs-min-power.md`.
- **Il bit `0x80` di shm `0x00cc`**: non e' il canale, la banda, la larghezza,
  il CAC ne' il beaconing, e nessuna altra cella della cattura ha la sua
  partizione. Vedi `retrace-todo.md`.
- La generalizzazione a piu' canali validati richiede l'estrazione dei registri
  radio-chain per canale e delle chanspec table; piano in
  `channel-generalization.md`.

## Mappatura file sorgente → patch

I sorgenti in `src/` sono la **fonte di verità**; le patch in `patches/`
sono un artefatto rigenerato.

| File sorgente | Patch |
|---|---|
| — (ssb/bcma SPROM) | 0001 |
| `phy_ac.h`, `tables_phy_ac.{c,h}`, `Makefile` (tables add) | 0002 |
| — (registrazione canali 5 GHz, `main.c`) | 0003 |
| — (DMA 64 KB alignment) | 0004 |
| `radio_2069.{c,h}`, `Makefile` (radio add) | 0005 |
| `phy_ac.c`, `phy_ac.h` (update), `helpers_phy_ac.c`, `rxiqcal_phy_ac.{c,h}`, `Makefile` (rxiqcal+helpers add), Kconfig | 0006 |
| — (bcma PMU init) | 0007 |
| — (core TX/RX wiring, `xmit.c`/`main.c`) | 0008 |
| — (bcma PCI bridge ID) | 0009 |
| — (MAC in shared memory, AC) | 0010 |
| — (address match table, corerev >= 42) | 0011 |
| — (celle SHM, corerev >= 42) | 0012 |
| — (key index block su ucode42, `main.c`) | 0013 |
| — (use-after-free in `b43_bcma_remove`) | 0014 |
| — (`channel_calibrate` da `b43_op_config`, `phy_common.c`) | 0015 |
| — (`ledbh4..15` da NVRAM sopra la SROM del device: `ssb.h`, `bcm47xx_sprom.c`, `bcma/sprom.c`) | 0016 |
| — (LED su gpio 4-15, piu' LED per ruolo: `leds.{c,h}`, `main.c`) | 0017 |

Farrow e rxgain non hanno file dedicati: `b43_phy_ac_farrow_setup` e il
blocco rxgain vivono come sezioni di `phy_ac.c` (patch 0006).

Le patch "—" toccano file fuori da `drivers/net/wireless/broadcom/b43/` e
non hanno corrispondente in `src/`: vanno mantenute editando la patch.

## Rigenerazione patch 0006

```sh
scripts/regen-patches.sh          # KVER=6.8.0-139 per scegliere gli header
```

Lo script scarica b43 vanilla al tag degli header kernel installati con
`test/integration/fetch-upstream.sh` (lo stesso base della suite di
integrazione), applica 0003 e 0004, sovrascrive con tutto `src/` -- `.c`,
`.h` e `Makefile`, che e' il Makefile del kernel con le righe della PHY AC --
e riemette la 0006 con `git format-patch`. Un file che la 0006 corrente
aggiunge e `src/` non ha piu' viene tolto. Messaggio e author sono presi
dalla patch corrente via `git am`: per cambiarli si edita la patch e si
rilancia. Un nuovo sorgente in `src/` entra nella patch aggiungendo la sua
riga a `src/Makefile`, non alla patch.

## Split upstream previsto per 0006

La 0006 attuale è un monolite (~8300 righe aggiunte); prima della submission
a `linux-wireless` va spezzata in commit da ~300-500 righe ciascuno. Lo
schema previsto, con `switch_channel` stub al passo 1 e riempito via via:

| # | contenuto | dipende da |
|---|-----------|------------|
| a | ops scaffold: allocate/free/prepare_structs, mode_init, init_regs, phyop r/w, ops struct con stub `switch_channel`/`op_init`/`rfkill` | 0002 |
| b | TX power: pa5g_group, txpwrctrl_setup, txgain table, txpwr_by_index, idle_tssi_meas | a |
| c | RF sequencer + reset-time: rfseq tables/tbl_init, set_reg_on_reset, force_rf_sequence, reset_cca | a |
| d | Analog on reset: femctrl, tx_lpf, rx_lpf, dacbuf, pdet, analog_on_reset | c |
| e | Channel setup: classifier, clip_det, rxcore_setstate, rx_gate, channel_setup, chanspec_tail, coeff_bank, chan_tables, rx_evm_shaping, adc_reset, rx_enable — riempie `switch_channel` | b, c, d |
| f | op_init + op_software_rfkill: wira il PHY nel framework b43 | e |
| g | rxgain: sezione rxgain_init/rxgainctrl di phy_ac.c | e |
| h | farrow: b43_phy_ac_farrow_setup + tabelle (da phy_ac.c) | a |
| i | rxiqcal_phy_ac.c/h (skeleton, gated off) | a |

## Note sulle scelte di implementazione

La formula `(cur & 0x0800) | 0x05f4` per reg 0x0140 assume che solo il bit
11 si muova e che gli altri siano invarianti. L'invarianza e' misurata, non
supposta: sui 139 segmenti in repo (freddo e caldo di d6220 e agcombo, 25
canali a 20/40/80 MHz) il registro riceve solo `0x05f4/0x05f6/0x0df4/0x0df6`. Cosa
significhino i bit [10:4] resta ignoto, ma non serve saperlo per emettere il
valore giusto.

**SALAME**: sul chip vero, se il PLL non locca velocemente, il poll
100×10µs dà 1ms di budget prima di emettere `b43dbg` — nessuna evidenza
che sia troppo o poco. Il vendor non fa polling (peek singolo), quindi
la scelta del budget è ingegneristica arbitraria.

**Divergenze cosmetiche note**:

- Convenzione `phy_mask`/`phy_set` (mask=0x0000) vs `phy_maskset` — 6 op
  residue nel freeze RX, semantica identica.
- Bit [10:4] di 0x0140 non ancora reverse-ingegnerizzati.

## Famiglia LPF analogica — risolta

TX-LPF, RX-LPF e DACBUF non sono più hardcoded: il cap viene derivato da
rccal e la RMW preserva il pre-state della cella. Formule (dettaglio e
verifica in `txlpf-formula.md`):

- TX-LPF: `cap = ((RCCAL_F - RCCAL_E) * 193) >> 8`.
- RX-LPF: `f17 = lpf_cap1` diretto; `f6 = (lpf_cap0 * k[stage]) >> 8` con
  `k = {221, 215, 215}` per le tre sezioni (le wl recenti scalano; la wl 6.30
  del DSL no — differenza di versione).
- DACBUF: `dacbuf_cap = (RCCAL_G & 0x03e0) >> 5`, dal readback post-apply.

Verificate sui tre board (d6220/DSL/agcombo).

## Poll con budget

`b43_phy_ac_rxcal_afe_iter` attende il bit busy di `0x0380` con un budget
finito (1000×udelay(1)) e un `b43err` non fatale, come `force_rf_sequence`.
Un `while` senza uscita la' e' un hang del kernel se il bit non si libera:
nessun poll del driver e' senza limite.

## Copertura del bring-up (rfkill + op_init)

L'harness marca ogni funzione con `B43_AC_FN()` (attivo con `AC_FN_MARKERS=1`,
altrimenti il trace resta pulito per `compare.py`); `fn_map.py coverage`
misura la copertura per-sequenza contro la cattura grezza. Risultato: bring-up
radio coperto al 100% su d6220 e agcombo. Le divergenze note (tutte sul DSL
wl 6.30: prefregs −2 scritture, afe_lpf_stage, rccal ~84%) sono in
`retrace-todo.md`.
