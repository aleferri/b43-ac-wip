# b43 AC-PHY / radio 2069 — BCM4352-family bring-up

Porting `b43` per AC-PHY rev 1 / radio 2069 rev 4, reverse dal binario `wl`
OEM. Le code-path sono rifatte con dispatch chip-aware `{4352, 4360}`. 
Tre board testimoni, con ruoli distinti:

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

**Configurazione completa di ogni pezzo hardware, senza tralasciare niente, e
beacon in aria su tutte le bande e frequenze 5 GHz.**

Non e' un obiettivo ambizioso per scelta, e' l'unico possibile con questo
metodo. Lavorando per confronto di tracce non esiste uno step intermedio utile:
ritagliare un sottoinsieme -- "solo associate a ch36 a 6 Mbit" -- richiederebbe
di bisecare la sequenza e di aver capito comunque *tutto* per sapere cosa si
puo' togliere senza rompere il resto. Il costo di capire tutto lo si paga in
ogni caso, quindi il traguardo e' la configurazione integrale.

Ne segue il criterio per decidere se un blocco va implementato: **non "serve ad
associare" ma "il vendor lo emette"**. Le fasi da AP -- beacon, template della
probe response, SSID in shared memory -- sono dentro lo scopo, perche' il
beacon in aria e' parte del traguardo.

Fuori scopo, e sono gli step successivi: la trasmissione e ricezione effettiva
di dati, e i 2.4 GHz.

**Priorita': il primo setup a freddo.** Lo switch a caldo e' *don't care*
finche' il freddo non e' confermato funzionante -- non fuori scopo, solo non
prioritario, e appena il freddo funziona torna subito lo step "ora deve
funzionare anche a caldo". Da cui tre conseguenze pratiche, che sono regole di
lavoro e non di scopo:

- **non si toglie niente.** I rami `FIRST_BRINGUP` in `src/` e la leva
  `AC_FIRST_INIT` nell'harness restano: servono di nuovo allo step successivo, e
  sono l'unica traccia di cosa fa il caldo;
- **`op_switch_channel` non rifiuta** una seconda chiamata. Un rifiuto sarebbe
  una restrizione di scopo, e lo scopo non e' restretto;
- **dove il caldo divergerebbe dal freddo, si implementa il freddo e si annota
  il caldo.** Vale per i valori CCK dei blocchi per-rate, per il conteggio dei
  poll a spazzata sola e per il massimo della catena PPR, che a caldo danno
  numeri diversi: la divergenza va registrata, non risolta ora.

Le catture a caldo restano invece **strumento di analisi** anche mentre non sono
un target: sono l'unico dataset con due cicli per configurazione, quindi il solo
in cui `reverse-tools/decorrelate_channels.py` possa rilevare la categoria
`dinamico`. E' come si e' stabilito che le celle del gruppo B sono una funzione
di canale e larghezza e non una misura.

## Stato corrente

Il numero di riferimento viene dal gate a freddo, che si lancia con
`test/unit/gates.sh`. La procedura esatta, con le trappole, sta in
[`test/unit/README.md`](test/unit/README.md) e **non va reinventata**: e' l'unico posto
dove si documenta come si produce un numero citabile.

Su `cold01-ch36-bw20`, il segmento di riferimento:

```
grezzo: 29818/29851 = 99.89%
        2 col valore sbagliato, 29 op di wl mancanti, 0 op del port di troppo
```

Su `cold01` il confronto posizionale non si ferma piu': `compare.py` non trova
nessuna divergenza sul prefisso e le due tracce differiscono solo in coda, per
le tre op che mancano dentro il perimetro. Delle 29 mancanti, 26 sono
dichiarate di altri dal perimetro -- OTP e SROM, celle di shared memory del
core, template RAM, i LED sul chipcommon -- e le tre che restano sono la coda
di `wlc_bmac_led_hw_deinit()` al rmmod, che la regola del perimetro non prende
perche' li' la maschera GPIO e' zero.

Contro lo sweep precedente lo stesso albero dava `28553/28582 = 99.90%` con 29
op mancanti e nient'altro. Il numero non e' peggiorato perche' il driver sia
peggiorato: e' peggiorato perche' la cattura vede di piu'. La ricattura ha
aggiunto nove classi di hook -- `AMT`, `OBJ.BULKR`/`OBJ.BULKW`, `ADDRM.SET`,
`PHY.FGC`, `PHY.RDW`, `PHY.WARR`, `CS.SHM`, `OBJ.SET` -- e ognuna porta in
denominatore op che prima nessuno dei due lati emetteva. Il 99.90% misurava un
confronto a cui mancava lo stesso pezzo da entrambe le parti.

Il denominatore e' l'unione dei due flussi, quindi fa 100% solo se il port
emette esattamente le op del driver stock: ne' meno, ne' di piu', ne' con
valori diversi. Le tre voci sono tre lavori distinti — una formula da trovare,
del codice da scrivere, un gate da mettere — e stanno in
[`docs/retrace-todo.md`](docs/retrace-todo.md). Su `cold01` restano due valori
e le 29 mancanti dette sopra, quindi il debito che il segmento di riferimento
misura oggi e' quasi tutto fuori da `src/`.

Lo sweep a freddo e' ora di 43 segmenti, e la tabella per famiglie che stava
qui non e' piu' valida: si reggeva su una separazione a 5250 MHz che lo sweep
vecchio non poteva smentire, perche' si fermava a ch140 e li' ogni canale con
la guardia radar sta sopra la soglia e ogni canale senza sta sotto. Con
ch144-165 le due cose si separano, e la famiglia vera e' la guardia radar, non
la frequenza: il poll del rivelatore (`PHY.RD 0x0251`) compare su tutti e soli
i segmenti fino a ch140, e `timeline.py` lo legge da ogni segmento.

La differenza di dimensione fra le due famiglie -- ~16k op contro ~29k --
neanche e' quella che c'era scritto qui. La parte grossa, 12052 op su 14022,
e' il bss-up che non avviene: sui canali con la guardia radar il CAC non si
chiudeva entro la finestra di cattura, quindi l'AP non saliva mai e le quattro
tabelle del BSS (`0x0e`, `0x42`, `0x62`, `0x82`) mancano. Non e' un attach
diverso del driver stock, e' una cattura presa durante l'attesa. I 21 segmenti
DFS sono stati ripresi con il CAC completato e adesso ce le hanno.

Quei 21 adesso producono un numero. Dopo la coda del bring-up il driver non
decide piu' il flusso: reagisce a eventi -- il work da un secondo del watchdog,
il timer da 150 ms del rivelatore radar, le ricariche del template che sono del
core, il bss-up che chiude il check e porta le calibrazioni. Su hardware
arrivano dal kernel e da mac80211; nell'harness li replaya `timeline.py` dai
timestamp della cattura, e nessuna lista di tick attraversa il confine di
`src/`. La lettura di temperatura e il dump della regione sono contatori del driver
(dieci e trenta giri), il bss-up e' un evento dello stack e cade dove la
cattura lo mette, fra due giri. Dalla cattura viene anche la forma del primo
giro dopo il bring-up -- pieno o la sola spazzata -- che non dipende dal
driver ma da quando scade il tick: `AC_WD_ENTRY_TURN`. Il costo rispetto al
modello a tick, da 0.4 a 1.7 punti, sono le ricariche del template che wl fa
dentro il giro del watchdog e b43 no. Sugli up a caldo dei canali con la
guardia radar, dove `wl up` rifa' il CAC ogni volta e l'harness lo negava, si
passa dal 37% al 93-96%. Dettagli e misure in
[`docs/retrace-todo.md`](docs/retrace-todo.md).

Lo stato dei 43 segmenti, con `gates.sh` su ognuno:

```
minimo 99.16%   mediana 99.44%   massimo 99.90%
```

Somma sui 43: 3189 valori sbagliati, 1510 op mancanti, 430 op di troppo, e
nessun segmento sotto il 99.1%. Una parte di quei valori pero' non e' un
valore: su cold12, il segmento piu' basso, 127 dei 163 sono letture di
`0x0308`/`0x030c` che il port fa giuste -- 125 su 125 identiche a quelle del
vendor -- e che il confronto appaia sfasate di un latch dietro a una sola op
fuori posto.

Delle op di troppo che restano, quelle della famiglia bassa sono in parte il
debito delle costanti per-canale. Il resto, e tutte quelle di cold15, e' la
spazzata dei contatori sui giri in cui il timer del vendor era in ritardo: non
e' il driver a deciderlo, e il latch della stessa finestra e' gia' chiuso --
vedi "Il latch della finestra sui giri in ritardo" in
[`docs/retrace-todo.md`](docs/retrace-todo.md).

Una parte del flusso non e' decisa dal driver, e il conto va tenuto separato:
le ricariche del template beacon e della probe response sono blocchi che il
vendor ripete un numero di volte che decide lo stack sopra, senza correlazione
con la durata del segmento (33-39 s in tutti). Non sono pero' debito: il
conteggio lo legge `reverse-tools/beacon_reloads.py` dalla cattura e `gates.sh`
lo passa all'harness come `AC_BEACON_RELOADS`, ed e' un orologio come
`probe_ticks` e il poll di CAC. Sui segmenti dello sweep precedente il port ne
emetteva esattamente
quante il vendor, da 7 a 19 caricamenti di template per segmento.
Il criterio che separa un orologio dal debito vero e' il conteggio fra
segmenti: una fase come `prb_rsp_rate_po` sta a 3 passate su tutti e 26 i
segmenti a freddo e tutti e 52 quelli a caldo, e quella e' struttura.

Il secondo gate e' il tick del watchdog periodico contro l'oracolo a regime, e
sta a **`MATCH`** — confronto posizione-per-posizione, nessuna eccezione. E'
il rilevatore di regressioni piu' sensibile del repo e va rilanciato a ogni
modifica.

L'estrattore SROM rev 11 passa l'harness userspace su tutti i vettori: 77/0
(DSL), 74/0 (D6220), 75/0 (agcombo).

Su hardware il driver **non completa ancora `ifconfig up`**, e non c'e' nessun
run recente: l'ultimo era sul DSL-3580L e precedeva la risoluzione della
famiglia LPF, quindi non fotografava piu' il codice. Serve un run nuovo prima
di poter dire dove si ferma oggi.

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
  cap LPF e DACBUF derivati dalle misure), `afe_lpf_stage`. PA bias e PMU
  enable finale sono *fuori* dallo scope di rfkill e non sono ancora
  implementati.
- **`switch_channel`** (BW20, 5 GHz): freeze RX → `radio_2069_channel_setup`
  → `channel_setup` (reset-time, AFE/LPF, RF sequencer, `rxcore_setstate`,
  farrow, chanspec tail, coeff bank) → `chan_tables` → noise shaping
  (tbl 0x15/0x0b/0x44/0x45 + `rxgain_init` per-core) → BW select →
  `reset_cca` → `afecal` → `adc_reset` → idle-TSSI → 2× `txpwrctrl_setup`
  (LUT est_pwr da `pa5ga` SROM) → lettura di temperatura del bss-up
  (`b43_phy_ac_tempsense()`).
- **Calibrazioni post-channel**, invocate da `op_switch_channel` dopo
  `mac_enable`: `post_cal_finalize` iter 2/3, RXIQ apply + stage 2, cal AFE
  RX, i due round `txpwr`/`rxgain` con misura RXIQ, il loop
  `gainctrl_final_apply`, teardown RXIQ.
- **Watchdog periodico** (`b43_phy_ac_watchdog`, hook `pwork_15sec`): poll
  TSSI/statistiche SHM con latch-and-clear della finestra SHM: il latch
  legge fino a 0x0314 e la clear si ferma a 0x0312, asimmetria che ogni
  cattura mostra e che e' voluta. Piu'
  la lettura di temperatura a MAC sospeso ogni `temps_period` giri — il
  ciclo su `0x0725/0x0925`, 10 s sulle board col campo non programmato. Op-per-op
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
| PA bias per-core, PMU regctl enable finale | non implementati | Sono le op che il vendor emette solo a steady state |
| LED (`ledbh10` da NVRAM) | portato nel core, `patches/0016-0017` | Il blocco GPIO che il vendor intercala nel preambolo freddo, i toggle al bss-up/down e il rilascio al rmmod sono `wlc_bmac_hw_up`/`wlc_bmac_led`/`wlc_bmac_led_hw_deinit` sul chipcommon: LED, senza effetto sul PHY. b43 li fa in `leds.c` per la sua via (MMIO `GPIO_CONTROL` del MAC); la 0016 porta `ledbh4..15` da NVRAM in `ssb_sprom`, anche sopra la SROM letta dal device (bcma), la 0017 gli insegna quei pin e piu' LED per ruolo. Le op del vendor stanno nel `PERIMETER` di `compare.py`, le `GPIO_CONTROL` di b43 in `SOLO_PORT`. Gira in `test/integration`, non provata su hardware |
| BW40 / BW80 | portati | Confrontati contro i 12 segmenti a 40 MHz e i 6 a 80 dello sweep a freddo |
| 2.4 GHz | `op_switch_channel` ritorna `-EOPNOTSUPP` | Mappa radio 2G non validata |
| Canali ≠ 36 | portati | 50 voci in channeltab (5170–5825 MHz); lo sweep ne copre 25 a 20 MHz. Piano in [`docs/channel-generalization.md`](docs/channel-generalization.md) |

### Bug aperti

- **`do_full_init` non distingue freddo e caldo su b43.** `b43_phy_exit()`
  (`phy_common.c:130`) lo rimette a `true` a ogni `ifconfig down`, quindi i
  commenti di `src/phy_ac.c` che lo citano come discriminante "cold attach
  only" affermano una cosa falsa sul path vero. La correzione dipende da una
  decisione sul caldo, che qui e' fuori priorita'. Trovato dalla suite di
  integrazione.
- **Il terzo `PHY.MOD 0x02e4`.** Sui canali sopra i 5250 MHz il vendor scrive
  quel campo tre volte; il port ne emette due. La terza sta a offset +13668
  dalla prima su tutti i segmenti alti, nella coda del channel setup a MAC
  abilitato. Nessun sito del port la emette.

Nota per chi legge il descrittore: `est_pwr_lut_core*` e
`papd_comp_rfpwr_tbl_core*` condividono id e offset **per costruzione**. Il
vendor scrive est_pwr e poi papd_comp_rfpwr sulle stesse celle nella stessa
sequenza di init (agcombo attach `#1345` e `#2899`). Non è un bug e non va
"corretto": rimuovere una delle due divergerebbe dal vendor.

## Verifica riproducibile

La procedura sta in [`test/unit/README.md`](test/unit/README.md), in sei passi. Qui solo
i due gate, per averli a portata:

```sh
cd test/unit
unzip -d /tmp/cold ../../router-data/d6220/cold-sweep.zip
make
./gates.sh                                    # cold a freddo: grezzo + prima divergenza
./gates.sh /tmp/cold/cold[0-9][0-9]-ch*.txt   # tutti e 43

AC_READ_ORACLE=../../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt \
    ./ac_trace periodic d6220 > /tmp/p.out
python3 compare.py \
    ../../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt /tmp/p.out
# MATCH
```

La seconda suite, `test/integration/`, compila b43 **intero** con il port
dentro contro gli header kernel veri, e lo esegue: oggi `b43_bcma_probe`
ritorna 0 e l'attach si chiude in 68 op. Non e' un punteggio, e' un ordine di
eventi che non si legge -- ed e' cosi' che si sono trovati i due difetti di
`switch_analog` e di `do_full_init` che `test/unit` non poteva vedere. Come si
lancia sta in
[`test/integration/README.md`](test/integration/README.md).

Le catture in `router-data/*/` sono lo sweep a freddo e quello a caldo negli
zip, piu' l'oracolo del tick a regime e i dump statici (NVRAM, SROM, tabelle
PHY, revinfo). I segmenti degli sweep sono l'unica fonte di attach: coprono
ogni canale e larghezza invece di uno.

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
docs/                    — documentazione tecnica
sprom-rev11/             — patch SROM rev 11 + harness userspace
reverse-tools/           — script Python (correlatore, estrattori, generatori) + tool C on-device
router-data/             — dump NVRAM/SROM/wl-diag per board (DSL, D6220, agcombo)
scripts/                 — helper (es. conversione patch per OpenWrt)
```

I due documenti da cui partire sono [`docs/driver-status.md`](docs/driver-status.md)
per cosa funziona e [`docs/retrace-todo.md`](docs/retrace-todo.md) per cosa
manca; gli altri file di `docs/` sono le analisi che quelli citano.

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
- **Trasmissione e ricezione dati** e **2.4 GHz**: i due step che seguono il
  traguardo MVP, non varianti di esso.
