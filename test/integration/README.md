# b43 intero su AC

Stato: **b43 compila, linka, parte, completa attach e bring-up e si chiude
pulito, con il `src/` di questo repo dentro.** La catena di riconoscimento --
core, PHY, radio -- e' tutta chiusa, i valori vengono dai dump della board,
`b43_bcma_probe` ritorna 0, `hw->ops->start()` ritorna 0 dopo 21036 op su
ch36 BW20, e la remove non lascia niente in piedi neanche sotto
AddressSanitizer (vedi [Dove si ferma](#dove-si-ferma)).

## Come si lancia

Tutto da questa directory. Verificato su Ubuntu 24.04 con header `6.8.0-139`.

### Prerequisiti

- `gcc`, `binutils`, `make`, `patch`, `curl`, `python3`, `unzip`; `gdb` per il
  backtrace.
- **Header kernel in `/usr/src`**: `apt-get install linux-headers-generic`.
  Non devono combaciare con il kernel in esecuzione -- qui non si carica
  niente -- ma decidono quale b43 si scarica: `fetch-upstream.sh` prende il tag
  `vX.Y` dalla versione degli header (`6.8.0-139` -> `v6.8`). Il Makefile
  prende la prima `linux-headers-*` che trova; con piu' versioni installate si
  passa `KVER=6.8.0-139` a ogni `make`.
- Rete verso `raw.githubusercontent.com`, solo per `make fetch`.

### I passi

```sh
make fetch          # b43 vanilla dal tag degli header + patches/ del repo
make check          # ogni file di b43 e del port: deve dire "0 errori"
make b43-trace      # compila, linka, stampa il conto dei simboli
```

`make fetch` deve finire con `applicate 10, saltate 3`: le 10 patch che toccano
`b43/` e le 3 su bcma/ssb che qui non hanno niente da applicare. Se una patch
non applica lo script **esce con errore** e l'albero in `b43-upstream/` va
buttato (`rm -rf b43-upstream kinc`) prima di riprovare: `patch` applica un
file alla volta, e un albero mezzo toppato compila lo stesso.

Il conto atteso di `make b43-trace` e' `kernel 0` e tutto il resto a zero
tranne `libc`, che sono i simboli che `trace_out.c` e `kernel_shim.c` chiedono
a libc e si risolvono al link finale. Se `kernel` non e' zero il residuo sta
in `residual.txt`: o e' lavoro di stub, o e' un simbolo di libc che manca alla
lista `LIBC` del Makefile. E' successo due volte: con `__vfprintf_chk`, che
gcc genera al posto di `vfprintf` con FORTIFY, e con `setvbuf`, che serve a
`trace_out.c` per il flusso a righe.

### L'oracolo

Senza oracolo ogni lettura torna zero, la traccia sembra plausibile e non e'
una misura. Si passa un segmento a freddo del d6220 **bonificato dell'attach
dell'altro core** e poi **ripiegato**, esattamente come in `../unit/README.md`:

```sh
unzip -d /tmp/cold ../../router-data/d6220/cold-sweep.zip
python3 ../../reverse-tools/strip_other_core.py \
    /tmp/cold/segmenti/cold01-ch36-bw20.txt /tmp/cold01-pulito.txt
python3 ../../reverse-tools/trace_filter.py --retvals \
    /tmp/cold01-pulito.txt /tmp/m01
```

Il primo passo qui conta piu' che in `../unit`: la' l'harness impersona il
PHY e non tocca la shared memory in lettura, qui gira b43 intero e
`b43_validate_chipaccess` legge `UCODEREV` e `UCODEPATCH`. Senza il filtro
serve i valori di wl0.

Poi:

```sh
make run ORACLE=/tmp/m01 TRACE_OUT=/tmp/int.trace
```

`TRACE_OUT` scrive la traccia in un file (e' `B43_TRACE_OUT` per il binario);
senza, la traccia va su stdout **insieme all'output di make**, che va bene per
guardare e non per confrontare. Le righe `b43: ...` -- cioe' `b43info` e
`b43err`, quelle che dicono quale gate ha respinto la probe -- vanno su stderr.

Oggi l'esito e' `probe: 0` e `start: 0`, e il file contiene 29470 op: attach,
bring-up intero, TX power adjust, calibrazioni e lo stop. Si confrontano con gli strumenti di `../unit`, **con
il profilo `--bus`**:

```sh
python3 ../unit/compare.py /tmp/m01 /tmp/int.trace --auto-align --bus
python3 ../unit/cmp_skip.py /tmp/m01 /tmp/int.trace 528:36546 --board d6220 --bus
```

Il profilo serve perche' questa traccia e' presa al bus MMIO, e il bus non ha le
classi degli accessor: un `PHY.MOD` del vendor qui e' una `PHY.RD` seguita da
una `PHY.WR` dello stesso registro, un `TBL.WR` e' solo le parole sulla porta
dati, un `MAC.MHF` e' un'ombra software. `tracelib.unfold_bus()` svolge il
vendor (e l'harness di `../unit`, che ha le stesse classi) in quel vocabolario:
la `RD` senza valore, perche' il vendor non registra cosa un MOD ha riletto, e
la `WR` vincolata sui soli bit della mask. Sull'harness di `../unit` il profilo
e' neutro -- 99.93% contro 99.92% -- quindi quello che misura qui e' b43, non
la traduzione. Senza `--bus` il confronto si rompe alla prima maskset e il
numero non dice niente.

Con il profilo, il segmento di riferimento da' **84.22%** (26109/31001): 13
valori sbagliati, 3319 op del vendor mancanti, 1547 del port di troppo, 51
regioni. Lo switch di canale da solo -- la traccia tagliata sui marcatori
`B43_FN_MARKERS=1` contro la finestra `5007:13465` -- fa 98.72%, zero valori
sbagliati, come l'harness di `../unit`. La prima divergenza posizionale sta a
`@50`: dopo il core attach il vendor scrive il blocco di configurazione della
shared memory (`0x80`, `0x5c`, `0x16`, `0xc0`/`0xc2`, `0x18`, `0x1c`, `0x44`,
`0x46`, poi l'azzeramento da `0x78c`) prima di toccare il PHY, e b43 entra in
`op_init`.

Una correzione di nome che ha spostato 460 op: la fase che il port chiamava
`bss_up` e' il **`wl down`**. Lo dicono i tempi -- in ogni segmento a freddo
sta al 99%, un secondo prima del `mod GOING` del rmmod, e in ogni segmento
`up` dello sweep a caldo, dove non c'e' nessun rmmod, sta al 99% pure -- e lo
dice il suo contenuto: finisce con il banco AFE a 8 scritture
(`0x1721=0xffff`, `0x1725=0x1fff`, `0x1720=0x03ff`, ogni blocco spento) e con
il rilascio del PMU che chiude la richiesta del preambolo freddo. I nomi dei
due banchi AFE nel port erano invertiti, `AFE_ON` per quello che spegne e
`AFE_DOWN` per quello a 4 scritture con `0x5000` che sta all'1% di ogni
segmento e accende; ora sono `AFE_OFF` e `AFE_ON`, e il bit di stato e'
`STATE_AFE_OFF`, con la stessa maschera. La fase e' `b43_phy_ac_down()`, e b43
la raggiunge da `b43_phy_exit()` con `software_rfkill(blocked=true)`: prima
non la emetteva affatto, e al suo posto spegneva l'AFE con il banco corto.

Tre cose di misura hanno pesato piu' del driver, e valgono per chiunque legga
questa traccia:

- **il tracer del vendor registra ogni maskset radio come terna** -- il
  `RAD.MOD`, la `RAD.RD` interna e la `RAD.WR` interna, 435 su 435 sul
  segmento di riferimento. Sul bus la terna e' la coppia RD+WR con i valori
  veri, e `tracelib.unfold_bus_seq()` scarta il MOD. I `PHY.MOD` non hanno
  ombra (52 su 3435 seguiti da una lettura, casuale);
- **l'oracolo deve avere un posto in coda per la lettura di ogni `PHY.MOD`**:
  il vendor non ne registra il valore, ma il maskset del driver sul bus legge
  davvero, e senza segnaposto consuma il valore della lettura successiva. Il
  segnaposto rende l'ultimo valore noto del registro, letto o scritto: e' il
  primo `PHY.RD 0x19e` dello switch che prendeva `0x3d2` invece di `0x0`;
- **`cmp_skip.py --bus` allinea su (classe, registro) e giudica i valori
  dentro i blocchi allineati** con `ops_equal()`. Allineare sulle stringhe,
  come nel profilo di default, contava ogni MOD svolto come due valori
  sbagliati: erano 5167, sono 13.

I 13 che restano, presi uno per uno con `make run` (che ora scrive anche
`TRACE_OUT.fn`, la traccia con i marcatori, dalla stessa build): nove sono lo
stack sopra il driver che questa suite non impersona -- `DTIMPER`, `BTSFOFF`,
`TIMBPOS` e il `RD 0xcc` che legge 0x44 invece di 0x45 sono il blocco BSS del
core che `bss_info_changed` scriverebbe fra lo switch e il TX power adjust
(`#13596-#13603`); `MCTRL |= BEACPROMISC` a `#13671` e il suo clear a `#36053`
sono `b43_adjust_opmode` con l'interfaccia AP, aggiunta e tolta; le due
suspend dentro `rxiqcal_finalize` sono le ricariche del beacon, che in
`../unit` sono `AC_BEACON_RELOADS` e in b43 sono il core -- e il resto e'
rumore d'allineamento degli stessi blocchi mancanti. In cambio ne e' uscito
un difetto del tracer, `PMU.RC` che troncava la mask a 16 bit nello stub, e
la fase down qui sopra.

Tre cose hanno portato il numero dal 26% a qui, e sono il modo in cui le fasi
che `../unit` chiamava a mano hanno preso il nome di cio' che b43 fa:

- **`channel_setup_tail` era `adjust_txpower`.** b43 lo raggiunge da
  `b43_op_config()` con `b43_phy_txpower_check()` e il `txpower_adjust_work`;
  il body e' quello, e `recalc_txpower` risponde `NEED_ADJUST` dopo uno
  switch (`txpwr_adjust_due`). Lo stub deve dare a `hw->conf.power_level` il
  `max_power` del canale come fa mac80211, e far girare il work in linea come
  `queue_work_on`: senza, `b43_op_config` salta il check.
- **`channel_setup_tail2` + `set_channel_calibrations` sono il phyop
  `channel_calibrate`** (`patches/0015`): `b43_op_config()` lo chiama al
  posto del suo `mac_enable` finale, e il phyop riaccende il MAC fra lo sweep
  del gain control RX, che lo vuole sospeso (`rxgainctrl_regs` vieta
  `MAC_EN`), e le calibrazioni, che lo vogliono acceso. E' dove il vendor
  emette quell'enable, e non era un'op spostabile: spostarla fa fallire la
  precondizione.
- **`op_switch_channel` a chanspec uguale non emette.** b43 lo chiama due
  volte sulla salita, da `b43_phy_init()` e da `b43_op_config()`; la cattura
  ha un solo `PHY.MOD 0x0003 val=0x0100` di `channel_switch_prep`. La seconda
  consumava le code dell'oracolo e le calibrazioni leggevano valori d'altri:
  22532 op di troppo che sono diventate 3929. `op_init()` azzera `tuned`,
  cosi' l'up dopo un down risintonizza come il vendor a caldo.

E un difetto del port che solo questa suite poteva vedere: `STATE_MAC_EN` non
era alzato o abbassato da nessuna riga di `src/`, lo manteneva
`__wrap_b43_maccontrol_set` di `../unit`. Ora `b43_phy_ac_status()` lo deriva
da `dev->mac_suspended`, che e' del core e lo tengono `b43_mac_suspend/enable`.

Quel che resta sono le 3319 op del vendor che b43 non emette -- il watchdog
(`wd_*`, 19 tick con la schedula di `AC_PROBE_TICKS`/`AC_WATCHDOG_TICKS` che
`../unit` passa e qui non gira), il blocco BSS di `bss_info_changed`, le
quattro passate `conf_tx`, il blocco shm di `wlc_coreinit` a `@50` -- e le
1547 del port di troppo, che sono del core: l'azzeramento della shared memory in
`b43_upload_microcode`, la AMT, le chiavi. Sul tetto regolatorio: non e' una
manopola del driver. `b43_phy_ac_reg_ceiling()` legge il `max_power` che
cfg80211 ha applicato al canale, e `AC_MAX_POWER_MAP` e' lo stub di cfg80211
dell'harness di `../unit`. Il d6220 ha `ccode=` vuoto nel NVRAM, quindi `wl`
gira con la sua locale interna e 21/26 dBm non si ricavano da niente di bordo:
qui `subsystem_stub.c` li applica alla registrazione come una tabella con un
nome, `wl_default_locale_5g`, e sul segmento di riferimento non cambiano
un'op, perche' su ch36 il SROM da' gia' 56.

### Dove muore

La build normale e' `-O2` e b43 ci inlina mezzo attach dentro
`b43_one_core_attach`: il backtrace non dice niente. Per il backtrace:

```sh
make clean && make DEBUG=1 b43-trace
B43_READ_ORACLE=/tmp/m01 gdb -batch -ex run -ex bt ./b43-trace
```

`DEBUG=1` aggiunge `-fno-inline -g` e tiene `-O2`, che gli header kernel
pretendono. Dopo il debug si rifa' `make clean`, o gli oggetti a `-fno-inline`
restano in giro. E' il modo con cui si e' trovato ogni gate di questa pagina:
non tentativi.

Regola per quando ci si ferma su un gate: ogni valore che b43 legge e' un dato
della board, si prende dai dump in `router-data/`, non si inventa. Inventarlo
fa prendere a b43 il ramo sbagliato in silenzio, che e' lo stesso modo di
sbagliare dell'oracolo assente.

## Perche' esiste

In b43 l'ordine degli eventi non si legge. `main.c` ha 42 gate su `core_rev`,
`fw.rev`, `phy->type` e `phy->rev` con rami `else` liberi, e i quattro punti
d'ingresso del PHY sono chiamati da `phy_common.c:83` dentro `b43_phy_init()`,
che chiama `b43_chip_init()`, che chiama `b43_wireless_core_init()`.
Ricostruire quella sequenza leggendo produce errori. Eseguirla no.

`../unit` non puo' rispondere: la' l'harness impersona b43 e decide lui
l'ordine, quindi ogni gate e' una cosa che riproduciamo per congettura. E il
suo `100.00%` nel perimetro e' su cio' che il PHY deve fare, non su cio' che
il driver deve fare: le op che b43 emette da altrove -- `MAC.*`, gli `OBJ.*`
del core, `TPL.RAMW`, la address match table -- sono fuori perimetro perche'
l'harness non le puo' emettere, non perche' b43 non le faccia.

## Traguardo 1, fatto

`make fetch && make check`:

```
  main.c:        0 errori
  phy_common.c:  0 errori
  bus.c:         0 errori
```

Contro gli header kernel **veri** da `/usr/src`, non contro stub. Tre cose
sono state necessarie e nessuna e' ovvia:

- **la versione di b43 deve combaciare con gli header.** Da `master` vuole
  `linux/unaligned.h`, che esiste da 6.12: contro header 6.8 da un errore che
  non e' del codice. `fetch-upstream.sh` prende il tag dagli header installati.
- **`-O2` e' un requisito, non un'ottimizzazione.** Senza, gli header kernel
  danno `impossible constraint in 'asm'` su `asm_inline`.
- **`-DKBUILD_MODNAME`**, altrimenti `netlink.h` non compila.

Quindi non serve stubbare `linux/*.h` come fa `../unit`. Il primo dei tre
anelli di stub che questo README prevedeva in una versione precedente non
esiste.

## Traguardo 2: il bus finto, fatto a meta'

`bcma_stub.c` fornisce `struct bcma_host_ops`, e questo e' il punto d'aggancio
giusto: gli inline di `<linux/bcma/bcma.h>` dispacciano su `core->bus->ops`,
quindi b43 non va toccato. I dieci simboli `bcma_*` che il link chiede sono
controllo di clock, enable, GPIO e PLL -- **non** le read e write dei
registri, che passano da qui.

La ricostruzione delle op sta in `note_write()`/`note_read()`: b43 scrive
prima l'indirizzo nel registro di controllo e poi il dato in quello di dato,
quindi la riga esce sul secondo accesso con l'indirizzo del primo. E' la
stessa ricostruzione che il decoder fa sulle tracce del vendor.

`trace_out.c` emette, nel formato di `../unit/wrap.c`, cosi' il confronto lo
fanno gli stessi strumenti. E' compilato come codice **utente**: gli servono
stdio e getenv e non e' codice di driver.

## L'oracolo delle letture, fatto

`B43_READ_ORACLE=<cattura ripiegata>`, con lo stesso significato di
`AC_READ_ORACLE` in `../unit`: una cattura passata da
`trace_filter.py --retvals`, servita **in ordine** per (classe, indirizzo).

Una **coda** per chiave, non un valore: il vendor legge lo stesso registro
molte volte con valori diversi, ed e' la successione a contare. `PHY 0x000f`
nel segmento di riferimento e' letto 343 volte con 58 valori distinti;
appiattirla su un valore fisso e' cio' che fa terminare i poll a sproposito.
Provato: le prime quattro letture di `PHY 0x000f` danno
`0x00db 0x0101 0x00db 0x0101`, che sono i primi quattro valori della cattura.

Esaurita la coda si ritorna **l'ultimo valore visto**, non zero: e' il
comportamento meno sbagliato quando il port legge piu' del vendor. Senza
oracolo ogni lettura e' zero e lo si dice una volta su stderr, perche' una
traccia con letture inventate sembra buona e non e' una misura.

### L'attach di wl0 va tagliato, e la suite lo rifiuta

Una cattura a freddo dello sweep porta **due** attach in testa: `wl` al
caricamento fa l'attach di tutti i core, quindi prima di wl1 -- l'AC -- c'e'
quello di wl0, l'N-PHY a 2.4 GHz. Sono 46 op, e le toglie
`reverse-tools/strip_other_core.py`:

```sh
python3 reverse-tools/strip_other_core.py <segmento> /tmp/seg
python3 reverse-tools/trace_filter.py --retvals /tmp/seg /tmp/oracolo
```

Qui l'oracolo carica il file intero e non ha una finestra come
`AC_READ_ORACLE_FROM` in `../unit`, quindi senza il taglio quelle op stanno in
testa alle code per indirizzo e il driver sotto esame si legge lo stato
dell'altro core. Misurato sul segmento di riferimento: `UCODEREV` (0x0000) e
`UCODEPATCH` (0x0002), che `b43_validate_chipaccess()` legge su entrambi i
core, tornano `0x55aa` e `0xaa55` -- i pattern del self-test di wl0 -- invece
di `0x03a0` e `0x2715`. Quattro righe su 20866, e il resto della corsa sembra
buono: `start: 0` in entrambi i casi.

Per questo la suite **rifiuta** e non avvisa. Il testimone e' il confine che
definisce il taglio, cioe' due coppie `OTP.RDR`/`OTP.INIT` prima della prima
`PHY.RD 0x0739`; una cattura a caldo o l'oracolo del tick a regime non ne
hanno nessuna e passano.

## Gli shim del kernel, fatti

`kernel_shim.c`, codice utente come `trace_out.c`. Tre cose non sono no-op e
sono scritte nel file col perche':

- **i lock non serializzano**: la suite e' a un thread, e se non lo fosse la
  traccia non sarebbe piu' ordinata e il confronto posizionale non avrebbe
  senso;
- **i delay non aspettano**: va bene per una sequenza di op, ma vuol dire che
  un poll su un bit che l'hardware alza dopo N microsecondi gira a vuoto --
  e' l'oracolo a dover rispondere, non il tempo;
- **`request_firmware` riesce**, con un blob minimo valido. Deve: se
  fallisce, `b43_upload_microcode` esce, `b43_chip_init` ritorna errore e
  `b43_phy_init` non viene mai chiamata -- cioe' salta cio' che la suite deve
  misurare. Il blob passa la validazione di `b43_do_request_fw`
  (`main.c:2188`): `type` fra UCODE/PCM/IV, `ver == 1`, e per UCODE e PCM
  `be32(size)` uguale ai byte dopo l'header. L'ucode vero non serve.

  E `dev->fw.rev` **non si inietta**: `b43_upload_microcode` lo legge dalla
  shared memory, `B43_SHM_SH_UCODEREV` a `0x0000` e `UCODEPATCH` a `0x0002`,
  quindi lo serve l'oracolo. Nel segmento di riferimento del d6220 quelle
  celle valgono `0x03a0` e `0x2715`, che ricomposte danno l'`ucoderev
  0x3a02715` della revinfo -- e il gate duro `if (fwrev <= 0x128) goto error`
  passa con `0x3a0 = 928`, mentre `fw.rev >= 598` porta sul ramo nuovo. Sono
  gate che decidono meta' del bring-up e vengono dai dati, non da una
  costante nello shim.

## Il link, chiuso

`make link` non ha piu' simboli veri da risolvere:

```
  simboli non risolti: 20
    bcma_        0
    ssb_         0
    ieee80211_   0
    b43_         0
    kernel       0
    libc        20  (si risolvono su libc, non sono lavoro)
```

Da 120. Le voci, e cosa vuol dire ciascuna:

- **`bcma_` (9)**, in `subsystem_stub.c`: clock, enable, GPIO, PLL. Nella
  traccia del vendor **non compaiono** -- il tracer di wl aggancia le funzioni
  di accesso, non quelle di gestione del core -- quindi no-op e' la risposta
  giusta, non una scorciatoia. Le op passano solo dalla vtable di
  `bcma_stub.c`.
- **`ssb_` (9)**: b43 compila con `CONFIG_B43_BCMA` e il ramo ssb non viene
  percorso, ma il link li chiede comunque.
- **`ieee80211_` (12)**: uno solo deve restituire qualcosa di usabile.
  `ieee80211_alloc_hw_nm` alloca `sizeof(struct ieee80211_hw) +
  priv_data_len` in un colpo e mette `hw->priv` subito dopo la struttura,
  come fa il kernel: b43 ci scrive la sua `b43_wl` e la rilegge con
  `hw_to_b43_wl()`, e con una struttura inventata il puntatore cadrebbe
  altrove e il difetto si vedrebbe mille op piu' tardi.
- **`b43_` (23)**: DMA, PIO, LED, rfkill, e le vtable degli altri PHY. `init`
  di DMA e PIO deve **riuscire**, o `b43_wireless_core_init` esce prima di
  `b43_security_init`: non emettono op ma il loro esito e' un gate. Le vtable
  `b43_phyops_{g,n,lp,ht}` sono a **NULL** di proposito: `b43_phy_allocate()`
  dispaccia su `phy->type` e su questo hardware prende il ramo AC, quindi un
  puntatore nullo fa vedere subito un dispatch sbagliato invece di produrre
  una traccia plausibile del PHY sbagliato.

`subsystem_stub.c` e' compilato coi flag del **kernel**, non come utente, e
serve: il compilatore ha corretto sei firme contro le dichiarazioni vere
(`bcma_core_enable` ritorna `int`, `ieee80211_queue_work` ritorna `void`, e
altre). Con firme inventate il link sarebbe passato comunque -- il C risolve
per nome -- e l'errore si sarebbe visto a runtime.

Il conto di `libc` era sbagliato due volte: prima escludeva i simboli che
`trace_out.c` chiede, poi quelli di `kernel_shim.c`. Un numero di "lavoro
rimasto" che include `calloc` non e' un numero.

## Traguardo 4: b43 parte, e la catena dei gate si risale uno per uno

`make b43-trace` produce l'eseguibile, `make run ORACLE=<cattura ripiegata>`
lo lancia. **b43 esegue il proprio percorso di attach**, e l'aggancio non
tocca b43: `b43_bcma_probe` e' statica, ma `module_init(b43_init)` con
`-DMODULE` crea l'alias `init_module`, quindi

```
init_module() -> b43_init() -> __bcma_driver_register(&b43_bcma_driver)
```

e il nostro `__bcma_driver_register` **cattura la probe** dalla struttura che
b43 gli passa. Da li' la chiama `main.c`. La sequenza la percorre b43.

### La catena, per come si risale

Ogni gate che b43 attraversa vuole un dato che lo stub deve servire, e ogni
volta che manca la probe esce **prima di emettere una sola op**. Il modo di
trovarli e' la build `make DEBUG=1 b43-trace` -- `-O2` con `-fno-inline -g`,
perche' `-O2` gli header kernel lo pretendono e i frame servono -- e un
backtrace: non tentativi.

Risolti:

1. `bcma_aread32(BCMA_IOST)`, a `b43_wireless_core_attach` (`main.c:5393`).
   E' una **vtable a parte** -- `aread32`/`awrite32`, non le `read32` --
   e da essa dipendono `have_2ghz_phy` e `have_5ghz_phy`, cioe' quale PHY b43
   alloca. Sul d6220 wl1 e' il core 5 GHz, quindi porta `B43_BCMA_IOST_5G_PHY`.
   I valori vengono da `b43.h:509`, non inventati.
2. `B43_MMIO_PHY_VER`, a `b43_phy_versioning` (`main.c:4484`). Decodifica in
   analog, type e rev secondo i campi di `phy_common.h:30`. Da `phy_type`
   dipende tutto: `b43_try_request_fw` prende `ucode42` per corerev 42 solo
   `if (phy->type == B43_PHYTYPE_AC)`. Senza, phy_type e' zero, nessun ucode,
   `-EOPNOTSUPP`. I valori vengono da `wl1_revinfo.txt`: phytype `0xb`,
   phyrev `0x1`.
3. `-DCONFIG_B43_PHY_AC=1` piu' il `phy_ac.c` di mainline, perche' il
   `case B43_PHYTYPE_AC` dello switch e' dentro un `#ifdef`.

4. **`b43_radio_versioning`** (`main.c:4586`) vuole `radio_manuf == 0x17F`
   (Broadcom) e per l'AC un `radio_id` che deve essere `0x2069`. Servito da
   `RADIO24_CONTROL`/`DATA` in `bcma_stub.c`, con id e revisione da
   `radiorev 0x42069` di `wl1_revinfo.txt`.

### E poi la domanda che questa catena ha fatto emergere

Il `phy_ac.c` di mainline e' **88 righe**: una vtable minima che non fa il
bring-up. Quindi far girare b43 *con l'AC vero* non e' una questione di
stub -- vuole il `src/` di questo repo al posto di quello, che e' esattamente
cio' che fanno le 13 patch in `patches/`. `fetch-upstream.sh` oggi prende b43
vanilla e non le applica: e' il passo che manca perche' la suite misuri il
nostro driver e non la vtable vuota di mainline.

Da non fare: aggiungere stub a caso finche' non crolla piu'. Ogni valore che
un gate legge e' un dato della board, e inventarlo fa prendere a b43 il ramo
sbagliato in silenzio -- lo stesso modo di sbagliare dell'oracolo assente.

## Traguardo 3: la finestra di confronto

Ricomincia da `#425` invece di `#528`, cosi' l'attach del core entra nel test.
Attenzione: la cattura contiene anche l'attach di **wl0**, l'N-PHY del secondo
core, che b43 su questo hardware non fa. Va escluso, e il confine e' `cpu0`
piu' i registri che `b43/phy_n.h` nomina `RFCTL_CMD`, `AFECTL_OVER`,
`AFECTL_OVER1`, `AFECTL_C1`, `AFECTL_C2`. Vedi
`router-data/dsl3580l/README.md`.

Poi il perimetro si riscrive: cio' che resta fuori e' cio' che b43 non fa, non
cio' che l'harness non puo' emettere. Il numero si muovera'.

## Cosa questa suite potra' rispondere e `../unit` no

- L'ordine reale dei quattro punti d'ingresso, senza leggere `phy_common.c`.
- Se `b43_clear_keys()` cancella la maschera di catene, che oggi e' un difetto
  **dedotto** dall'ordine di `b43_security_init()` e non osservato. C'e' gia'
  `patches/0013-b43-relocate-the-key-index-block-on-ucode42.patch`: questa
  suite e' il modo di verificarla.
- Se `b43_software_rfkill(dev, false)` alla fine di `b43_op_config()` ha una
  controparte nella cattura: sul segmento a caldo c'e' un blocco a firma RAD
  fra la fine di `set_channel` e l'inizio della cal che nessuno span del port
  copre.

## Traguardo 5: le patch del port su mainline

`make fetch` ora prende b43 vanilla **e applica `patches/`** con
`apply-port.sh`. Serviva: il `phy_ac.c` di mainline e' un segnaposto di 88
righe, e senza le patch la suite misurava quello.

Le patch PHY erano fuori sincrono e sono **rigenerate da `src/`**, non
mantenute a mano: si costruisce un albero mainline, ci si copia `src/` sopra,
e il diff *e'* la patch. Cosi' non puo' divergere dall'albero che la suite
`../unit` misura. Le tre vecchie (`0002` registri e accessori, `0005` radio
2069, `0006` init/tuning/RF/TX) sono sostituite da una sola,
`0006-b43-AC-PHY-bring-up-from-the-reverse-engineered-port`, 10 file e 15391
righe.

`apply-port.sh` salta le patch che toccano `bcma` e `ssb`: qui il bus e'
finto e non hanno niente da applicare. Con un'eccezione, `patches/0001`: i
suoi due header `linux/ssb/ssb.h` e `ssb_regs.h` servono, perche' aggiungono i
campi della SROM rev11 -- `pa5ga`, `maxp5ga`, `femctrl`, `subband5gver`,
`rxgains` -- che in mainline non esistono e senza cui `phy_ac.c` non compila.
Stanno toppati in `kinc/`, che va **prima** degli header kernel.

### Stato

Tutti e nove i file compilano a **zero errori**, e delle patch di `b43/` ne
applicano **10 su 10**, e tutta la serie applica anche con `git am` senza fuzz.

Due difetti veri trovati, che `../unit` non poteva trovare perche' compila
contro `stubs/b43.h` invece degli header kernel:

- **`patches/0001` non dichiarava `antenna_gain_qdb`**, che
  `src/phy_ac.c:1528` legge. Esisteva solo in `../unit/stubs/b43.h:174`,
  quindi il port compilava contro lo stub e nessuno lo aveva visto. Aggiunto
  a `struct ssb_sprom` accanto agli altri campi rev11.
- **`patches/0013` era malformata**, due volte. L'intestazione del primo hunk
  dichiarava 26 righe nuove e il corpo ne aveva 25, perche' mancava una riga
  di contesto in testa. `patch` moriva con "malformed patch" alla riga 93,
  cioe' venti righe dopo la causa. Corretto quello, il secondo hunk (`main.c`)
  dichiarava `-812,9 +812,11` su un corpo di 10 e 10 righe: `xmit.h` applicava,
  `main.c` no, e `apply-port.sh` lo diceva su stderr e usciva a zero. Ora
  esce con errore.

### Il link, chiuso anche col port dentro

```
  simboli non risolti: 22
    bcma_ 0   ssb_ 0   ieee80211_ 0   b43_ 0   kernel 0
    libc 22  (si risolvono su libc, non sono lavoro)
```

Dopo le patch il residuo era risalito a 46, per la ragione giusta: si
compilano `xmit.c` e il nostro `phy_ac.c`, cioe' codice che prima non c'era.
Tutti coperti.

**Quattro non sono no-op e non potevano esserlo**: `bcma_chipco_gpio_out`,
`gpio_outen`, `pll_read` e `regctl_maskset` **emettono op**, e compaiono nella
traccia del vendor con le classi che il decoder gli da' -- `GPIO.OUT`,
`GPIO.OE`, `PMU.PLL`, `PMU.RC`. Stubbarli a no-op cancellerebbe op che vanno
confrontate, e il punteggio salirebbe togliendo lavoro: il modo peggiore di
sbagliare. `gpio_outen` la traccia la chiama `GPIO.OE` -- lo stesso registro
con due nomi -- e la normalizzazione di `reverse-tools/tracelib.py` li allinea
gia'.

Neanche `int_sqrt` e' un no-op: la usa il solve della RX IQ, e ritornare zero
falserebbe i coefficienti in silenzio. E' implementata per davvero.

`b43_dma_tx_suspend`/`resume`, `b43_pio_*`, `b43_ac_beacon_reload` e le
utilita' di mac80211 (`ieee80211_hdrlen`, `rts_get`, `ctstoself_get`,
`generic_frame_duration`, `get_tkip_p1k_iv`, `rx_napi`,
`channel_to_freq_khz`) non emettono op sul percorso di bring-up.

### Un difetto degli shim che solo l'esecuzione trova

`kmalloc_caches` era dichiarato come array piatto di 64 puntatori. Nel kernel
e' **bidimensionale**, `[NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH+1]`, e
l'indicizzazione inlinata di `kzalloc` calcolava offset 1456 su un oggetto di
1024 byte: `b43_bus_dev_bcma_init` moriva su
`mov 0x10(%rdx,%rax,1),%rdi` con `rax=0x5b0`. Sovradimensionato a `[8][64]`.

Il link non lo poteva vedere -- il simbolo c'era -- e nemmeno la compilazione.
Solo l'esecuzione, e solo col registro faulting sotto gli occhi.

### Dove si ferma adesso

b43 **riconosce l'hardware**, e lo dice:

```
b43: Broadcom 4352 WLAN found (core revision 42)
b43: Found PHY: Analog 0, Type 11 (AC), Revision 1
b43: Found Radio: Manuf 0x17F, ID 0x2069, Revision 4, Version 0
```

Tutta la catena di riconoscimento e' chiusa: core, PHY, radio. I valori
vengono dai dump della board -- `wl1_revinfo.txt` da' `radiorev 0x42069`,
cioe' id `0x2069` e revisione 4, il 2069 rev4 -- non da costanti scelte qui.

E l'attach lo completa: `b43_bcma_probe` ritorna 0, dopo **68 op**. Per un po'
non si vedevano: la traccia usciva su stdio a buffer pieno e il segfault che
c'era allora se la portava via. Ora il flusso e' a righe.

### Tre errori miei che questa fase ha reso visibili

1. **Gli offset MMIO li avevo ridefiniti a mano** in `bcma_stub.c`, e
   `B43_MMIO_PHY_VER` era `0x030` invece di `0x3E0`. Risultato: b43 leggeva
   zero e diceva `FOUND UNSUPPORTED PHY (Analog 0, Type 0, Revision 0)`. Ora
   il file include `b43.h` dell'albero fetchato e non ridefinisce niente --
   `-I$(UP)` nel Makefile serve a questo.
2. **`_printk` era muto.** `b43err` e `b43info` passano da la', e sono il modo
   in cui b43 dice quale gate lo ha respinto. Con uno `_printk` vuoto la probe
   ritorna `-95` e l'unica via e' il debugger: ci ho perso due giri. Ora
   scrive su stderr, e gestisce `%pV` -- il formato ricorsivo del kernel, dove
   l'argomento e' una `struct va_format` col vero messaggio dentro; un
   `vfprintf` normale stampa il puntatore e il messaggio si perde.
3. **`kmalloc_caches` era un array piatto** dove il kernel lo ha
   bidimensionale, e l'indicizzazione inlinata di `kzalloc` usciva
   dall'oggetto.

Tutti e tre invisibili al link e alla compilazione. E' il genere di cosa per
cui questa suite esiste.

## I difetti che la suite ha trovato

### `switch_analog` non e' un hook che si chiama una volta -- **risolto**

`src/phy_ac.c` leggeva `dev->phy.chandef->chan->center_freq` dentro
`b43_phy_ac_frontend_gpio_setup`, e la' il puntatore e' nullo: la funzione gira
da `op_switch_analog`, che `b43_wireless_core_reset` (`main.c:1455`) chiama
dentro l'attach, centinaia di righe prima che un canale esista.

Il puntatore nullo era il sintomo. Il difetto e' che b43 chiama
`switch_analog(dev, true)` da **quattro** siti -- `main.c:5650` (attach),
`4956` (core init), `3402` (chip init), `phy_common.c:97` (`b43_phy_init`) --
e su un `ifconfig up` ne scattano tre, mentre il vendor emette il preambolo a
freddo una volta sola. Con una guardia sul nullo il port lo avrebbe emesso due
o tre volte, che e' un difetto peggiore e silenzioso.

La correzione e' `b43_phy_ac_cold_preamble_due()`: il preambolo e' dovuto al
primo ingresso che ha un canale, e un bit di `status_mask` tiene fuori i
successivi. Il canale e' il discriminante di fase perche' b43 ne ha uno solo
da `b43_phy_init()` in avanti -- e' quella funzione a puntare `phy->chandef`,
sulla riga prima di chiamare `switch_analog()`. Non c'e' nessuna guardia
difensiva al sito di lettura: l'invariante e' del chiamante.

`../unit` non lo poteva trovare, per due ragioni indipendenti: il suo
`stubs/b43.h:100` dichiara una `struct cfg80211_chan_def chandef` **per
valore** dentro la conf finta, quindi il puntatore c'e' sempre; e l'harness
impersona b43, quindi chiama `switch_analog` una volta e la molteplicita' non
esiste.

### `do_full_init` non distingue freddo e caldo su b43 -- **aperto**

`b43_phy_exit()` (`phy_common.c:130`) rimette `do_full_init = true`, ed e'
chiamata da `b43_chip_exit()` a ogni `ifconfig down`. Quindi sul path b43 quel
flag e' vero a **ogni** salita, e i commenti di `src/phy_ac.c` che lo citano
come discriminante "cold attach only" dicono una cosa che non e' vera. Non
toccato: dipende da una decisione sul caldo che il README di primo livello
mette fuori priorita'.

### La shared memory usciva a offset diviso quattro -- **risolto**

`bcma_stub.c` riportava come indirizzo OBJ il contenuto grezzo del registro di
controllo. Per `B43_SHM_SHARED` quello non e' l'offset in byte: b43 ci mette
l'indice a 32 bit (`offset >> 2`) e distingue le due meta' con la porta dati,
`B43_MMIO_SHM_DATA` per la parola a byte `4w` e `B43_MMIO_SHM_DATA_UNALIGNED`
per quella a `4w+2`. Il tracer del vendor prende invece l'offset in byte dagli
argomenti di `wlc_bmac_read_shm`. Risultato: **ogni** op OBJ della suite era a
un quarto dell'indirizzo giusto e non combaciava con niente -- il
write-through degli host flag usciva `OBJ.WR addr=0x0017` dove la cattura ha
`addr=0x005e`, stesso valore `0x0100`.

Nella stessa correzione, un accesso a 32 bit allineato -- uno solo sul bus --
si scompone nelle **due** op a 16 bit che il vendor emette, perche' `wl` la
shared memory la tocca solo a 16 bit. Quale meta' sta al byte piu' basso lo
decide la cattura, non una convenzione scelta qui: vedi la sezione seguente.

### `MACCONTROL` non lo puo' servire l'oracolo -- **risolto**

Le 137 `MAC.MCTRL` del segmento di riferimento non hanno una sola `RETVAL`: il
tracer del vendor registra l'argomento della maskset, non la rilettura. Quel
registro lo determinano per intero le scritture del driver, e b43 lo usa in
read-modify-write -- `b43_validate_chipaccess` (`main.c:3641`) pretende di
rileggere l'`IHR_ENABLED` che `b43_wireless_core_reset` ha appena messo. Nello
stub e' un latch, non una lettura dalla cattura.

## `b43_validate_chipaccess`: il vendor fa lo stesso test

Vale la pena isolarlo, perche' e' il primo posto in cui la suite ha misurato
qualcosa invece di limitarsi a passare un gate.

`b43_validate_chipaccess` (`main.c:3597`) e' un self-test: salva SHM_SHARED 0 e
4, scrive `0x55aaaa55` e `0xaa5555aa` all'offset 0 rileggendoli, prova il path
non allineato, ripristina. Sembra il tipo di cosa che l'oracolo non puo'
servire perche' il vendor non la fa. **La fa**, e op per op:

```
#507 OBJ.RD  addr=0x0000 -> 0x03a0     backup
#509 OBJ.RD  addr=0x0002 -> 0x2715
#511 OBJ.WR  addr=0x0000 =  0x55aa     write32(0, 0x55aaaa55)
#512 OBJ.WR  addr=0x0002 =  0xaa55
#513 OBJ.RD  addr=0x0000 -> 0x55aa     read32(0)
#515 OBJ.RD  addr=0x0002 -> 0xaa55
#517 OBJ.WR  addr=0x0000 =  0xaa55     write32(0, 0xaa5555aa)
#518 OBJ.WR  addr=0x0002 =  0x55aa
#519 OBJ.RD  addr=0x0000 -> 0xaa55     read32(0)
#521 OBJ.RD  addr=0x0002 -> 0x55aa
#523 OBJ.WR  addr=0x0000 =  0x03a0     ripristino
#524 OBJ.WR  addr=0x0002 =  0x2715
```

Le celle `0x0000`/`0x0002` sono `UCODEREV` e `UCODEPATCH`, e i valori del
ripristino sono gli stessi `0x03a0`/`0x2715` da cui viene l'`ucoderev` della
revinfo. Il test lo fa anche il core dell'altro PHY, a `#437-448`.

Da qui viene il verso della ricomposizione a 32 bit, che b43 da solo non
determina: la parola al byte piu' basso porta la meta' **alta**. Con quel
verso le due letture del vendor a `#513`/`#515` ricompongono `0x55aaaa55` e il
test di b43 passa contro i valori che l'hardware ha davvero restituito; con
l'altro verso fallisce. Non e' una scelta dell'harness, e' una misura.

La seconda meta' del self-test, quella non allineata, il vendor **non** la
esegue: usa `0x1122`/`0x3344`/`0x5566`/`0x7788` e nella cattura non c'e'
niente da servire. Per quella lo stub modella **quattro celle e solo quelle**,
`SHM_SHARED` byte `0x0000`-`0x0006`: una volta scritte ritornano cio' che gli
e' stato scritto, e finche' nessuno le scrive vengono dall'oracolo.

Non e' un'assunzione: a `#513` e `#515` il vendor rilegge esattamente cio' che
`#511` e `#512` avevano scritto due op prima, quindi che quelle celle siano RAM
e' misurato. La lista e' a mano e corta di proposito, perche' il write-through
**non** e' corretto in generale -- le celle che aggiorna il ucode, la finestra
statistiche `0x0300`-`0x0314` e i contatori `0x0768`-`0x0788`, il port le
azzera e le rilegge, e servirle dal modello farebbe passare il gate del
watchdog misurando niente. Quando servira' una quinta cella si decidera'
allora, col criterio che le separa (le ucode-owned il vendor le legge piu' di
quanto le scriva) estratto dal segmento.

Con il modello le 12 op del vendor combaciano tutte, in ordine e con gli
stessi valori, e i due `b43warn` non compaiono piu'.

Le altre **16** -- la `read32(4)` di backup, il blocco non allineato e la
`write32(4)` di ripristino -- sono in `SOLO_PORT` di `../unit/compare.py`, con
le due prove che le qualificano: il vendor non tocca `0x0004`/`0x0006` in
nessuna cattura e `src/` non scrive SHM sotto `0x000c`, e le costanti
`0x1122`/`0x3344`/`0xccdd` su `0x0000`/`0x0002` non compaiono come valore OBJ
da nessuna parte. Sono di `main.c` mainline, non di `src/`, e non sono
pagabili senza toccare b43: e' il terzo caso legittimo che quella lista ora
nomina. La meta' allineata **non** e' scartata e va confrontata.

Punto cieco da non dimenticare: `0x0000` e `0x0002` sono `UCODEREV` e
`UCODEPATCH`, e a scriverle e' il microcodice quando parte. Qui coincide,
perche' il ripristino del self-test rimette i valori che l'oracolo aveva
servito; con un ucode di revisione diversa da quella della cattura la
rilettura darebbe il valore di `wl` e niente lo segnalerebbe.


## Dove si ferma

L'attach passa per intero. I gate risolti, ognuno con i valori presi dai dump
della board e non scelti qui:

| gate | dato |
|---|---|
| `bcma_aread32(BCMA_IOST)` | `B43_BCMA_IOST_5G_PHY`, wl1 e' il core 5 GHz |
| `B43_MMIO_PHY_VER` (`0x3E0`) | phytype `0xb`, phyrev `0x1` |
| `RADIO24_CONTROL`/`DATA` | id `0x2069`, rev `4`, da `radiorev 0x42069` |
| `bus->host_pci->device` | `0x43b3`, il 4352 di `patches/0009` |
| `kzalloc_obj` | macro di un kernel > 6.8, in `compat.h` |
| SHM `0x0000`/`0x0002` in `b43_validate_chipaccess` | le due meta' del self-test, dalla cattura |
| `B43_MMIO_MACCTL` in rilettura | latch dello stub, la cattura non ha `RETVAL` |

### Il bring-up parte: 2185 op

`main.c` ora chiama anche `hw->ops->start()` dopo la probe, che e' da dove
mac80211 entra su `ifconfig up`. L'aggancio non tocca b43: `b43_op_start` e'
statica, e la vtable la cattura `ieee80211_alloc_hw_nm()` nello stub, come
`__bcma_driver_register` cattura la probe.

Due cose sono servite, e nessuna e' arbitraria:

- **`ieee80211_register_hw()` scegle il canale di default**, che nel kernel fa
  mac80211 e non b43: `b43_phy_init()` (`phy_common.c:93`) legge
  `hw->conf.chandef.chan->hw_value`. Lo stub lo prende dalla prima voce della
  banda che b43 ha registrato in `b43_setup_bands()` (`main.c:5267`), non da
  una costante, cosi' e' la banda del driver a deciderlo.
- **I work item girano in linea.** `b43_bcma_probe` (`main.c:5851`) chiede il
  firmware da un work, e con un `queue_work_on` no-op quel lavoro non gira
  mai: `dev->fw.ucode.data` resta nullo e `b43_upload_microcode()` muore a
  `main.c:2761` dereferenziandolo, dentro il bring-up e lontano dalla causa.
  Stesso ragionamento dei lock: la suite e' a un thread. La differenza dal
  kernel e' che la' il work parte dopo che la probe e' ritornata, e se una
  fase dipendesse da quell'ordine si vedrebbe come op fuori posto, non come un
  crash. `queue_work_on()` sta in `subsystem_stub.c` e non in
  `kernel_shim.c`: gli serve `struct work_struct`, e quel file e' codice
  utente.

Da 68 op a **6981**, e il percorso e' quello vero: `b43_op_start` ->
`b43_wireless_core_init` -> `b43_chip_init`.

### Il firmware, fatto

Tre cose sono servite, e ognuna nascondeva la successiva.

- **`request_firmware_nowait` deve riuscire.** `b43_do_request_fw`
  (`main.c:2364`) chiama la nowait e, se ritorna negativo, stampa "Unable to
  load firmware" e **ritorna l'errore**: il commento sul fall-through al
  percorso sincrono vale solo per un esito >= 0. Lo shim chiama la
  continuazione in linea -- `b43_fw_cb`, che mette il blob in `ctx->blob` e
  completa -- e ritorna 0.
- **Un blob per tipo, non uno condiviso.** b43 tiene quattro firmware vivi
  insieme (`fw.ucode`, `.pcm`, `.initvals`, `.initvals_band`) e li usa dopo,
  non alla richiesta: con un unico statico l'ultima richiesta sovrascriveva le
  altre e `b43_upload_microcode()` leggeva l'header di una lista di initval.
- **Il microcodice deve rispondere.** `b43_upload_microcode()` (`main.c:2790`)
  avvia il PSM e aspetta `B43_IRQ_MAC_SUSPENDED` su `B43_MMIO_GEN_IRQ_REASON`,
  venti giri e poi "Microcode not responding". L'oracolo non lo puo' servire:
  il tracer del vendor non aggancia le letture MMIO grezze e in nessuna
  cattura c'e' una `REG.RD 0x128`. Lo produce il microcodice, e il microcodice
  qui e' lo shim -- il blob e' un header valido senza codice dentro, il PSM non
  partirebbe mai. Sta in `bcma_stub.c` accanto al latch di `MACCONTROL`, e non
  e' un latch: b43 scrive la' per fare l'ACK degli interrupt.

E una cosa e' stata **disfatta**, che e' il risultato piu' utile di questa
fase. Il punto cieco annotato sul modello delle quattro celle SHM si e'
materializzato due passi dopo: `0x0000` e `0x0002` sono `UCODEREV` e
`UCODEPATCH`, b43 le azzera prima di avviare il PSM, e il write-through
serviva quello zero a `b43_upload_microcode()`, che rifiutava il firmware come
troppo vecchio (`fwrev <= 0x128`). Il modello ora copre solo `0x0004` e
`0x0006`, le due celle che il vendor non tocca in nessuna cattura. Il prezzo e'
il `b43warn` sul path non allineato, e si paga volentieri: quell'avviso non
tocca il flusso, `fwrev` decide il ramo dell'ucode.

### Il bring-up arriva in fondo, e la chiusura ha trovato due difetti

Per un periodo la suite si fermava a 6981 op su una precondizione del port
(`b43_phy_ac_channel_setup`, `forbid=0x0306` con `AFE_ON` alzato). Non lo fa
piu': `start: 0` dopo **21036 op**, e quella precondizione e' storia di
`src/`. Ma la patch 0006 era fuori sincrono da `src/` -- mancavano
`ppr_ac.c/.h` e `phy_ac.c` divergeva di 1500 righe -- e la suite misurava un
albero che non esisteva. `scripts/regen-patches.sh` ora copia tutto `src/`,
Makefile compreso, che e' il Makefile del kernel con le righe AC e non un
makefile fuori albero.

Con la patch giusta il binario arrivava a `start: 0` e moriva **dopo**, in due
modi diversi a seconda dei flag: `free(): invalid pointer` a `-O2`, SEGV su
`dev->phy.ac` in `b43_phy_ac_post_rfseq_misc_setup` con `DEBUG=1`. Un sintomo
che cambia con l'inlining e' corruzione di memoria, e ASAN ha dato le due
cause:

- **`__sw_hweight32` era una funzione C.** Gli header x86
  (`arch/x86/include/asm/arch_hweight.h`) la chiamano da un inline asm che
  dichiara solo `%rdi` in ingresso e `%rax` in uscita, perche' la versione del
  kernel (`arch/x86/lib/hweight.S`) preserva ogni altro registro. In userspace
  le `ALTERNATIVE` non vengono patchate, quindi la `call` gira sempre, e una
  funzione C sporca `%rdx` e `%rcx`: e' li' che il compilatore teneva
  `dev->phy.ac`. Ora e' asm in `kernel_shim.c` con lo stesso contratto, e
  `KFLAGS` ha `-mno-red-zone` come il kernel, perche' quella `call` scrive
  l'indirizzo di ritorno sotto `%rsp`. La compilazione e il link non lo
  potevano vedere, e nemmeno il sanitizer da solo: e' il caso in cui serve
  leggere il contratto dell'asm.
- **`b43_bcma_remove` in mainline legge `wldev->dev` dopo che
  `b43_one_core_detach()` ha liberato `wldev`.** Use-after-free vero, presente
  anche in master; nel kernel passa perche' SLUB lascia il puntatore nella
  memoria liberata. Il ramo `b43_ssb_remove` salva `dev` prima della detach, e
  `patches/0014` fa lo stesso sul ramo bcma.

Nel farlo e' emerso che `patches/0013` applicava solo con il fuzz di `patch`:
il contesto dell'hunk su `xmit.h` non aveva la riga `/* FIXME ... */` del
sorgente. Riemessa via git, con lo stesso messaggio e author.

### Lo stub e' programmabile

`B43_CHANNEL` e `B43_BW` scelgono la configurazione, perche' i 26 segmenti
dello sweep sono 26 configurazioni e una suite che sa fare solo la prima non
li misura. Sono gli stessi due parametri che `../unit` prende come
`AC_CHANNEL` e `AC_BW`.

```sh
make run ORACLE=/tmp/m01 B43_CHANNEL=52 B43_BW=20 TRACE_OUT=/tmp/t
```

Il canale non si inventa: si cerca fra quelli che b43 ha registrato in
`b43_setup_bands()` (`main.c:5267`), che e' la stessa restrizione di
mac80211, e se non c'e' lo si dice invece di lasciare un puntatore a un canale
che il driver non conosce. `subsystem_stub.c` compila coi flag del kernel e
non ha `getenv`, quindi la lettura passa da `b43_test_env_long()` in
`trace_out.c`, che e' codice utente.

Fuori da ch36 BW20 serve anche `AC_ANY_CHANNEL=1` **al build**, come in
`../unit`, perche' il guard delle configurazioni validate di
`op_switch_channel()`
altrimenti rifiuta e la probe torna `-95` prima di emettere il bring-up:

```sh
make clean && make AC_ANY_CHANNEL=1 b43-trace
make AC_ANY_CHANNEL=1 run ORACLE=/tmp/m05 B43_CHANNEL=52 TRACE_OUT=/tmp/t
```

Con quello ch52 arriva in fondo come ch36 -- `start: 0`, 20916 op contro
21036. **Ricompilare senza il flag prima di chiudere**, o il gate di riferimento gira su un binario che difende
meno: e' la stessa avvertenza di `../unit/README.md`.

Il guard **non** va aperto per far girare la suite, e il punteggio non e' il
suo criterio: lo dice il commento a `src/phy_ac.c:5223`, *it defeats a guard
that protects the PA*, e la lista sopra dichiara che una voce se la guadagna
con una cattura. Il numero da guardare non e' la percentuale ma i valori
sbagliati -- un registro che il port scrive con un valore che il vendor non
scrive mai:

| configurazione | valore sbagliato | op del port di troppo |
|---|---|---|
| ch36 BW20 | **0** | **0** |
| ch40 BW20 | 8 | 162 |
| ch44 BW20 | 30 | 156 |
| ch48 BW20 | 38 | 271 |
| ch52 BW20 | 48 | 1808 |
| ch36 BW40 | 130 | 377 |
| ch36 BW80 | 139 | 141 |

Una sola configurazione e' a zero su entrambe le colonne, ed e' la sola nella
lista.

Il bring-up arriva in fondo, quindi la finestra di confronto del Traguardo 3
e' definibile: 21036 op del port contro 28566 del vendor sul segmento di
riferimento. La suite non cita ancora una percentuale perche' il perimetro --
cosa b43 davvero non fa, contro cosa l'harness di `../unit` non poteva
emettere -- va ancora scritto per questa suite: e' il prossimo passo.
