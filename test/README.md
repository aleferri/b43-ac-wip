# test/ — userspace trace-verification harness

Compila il codice del driver AC-PHY sotto `../src/` in
userspace e produce una trace nel formato di `wl-diag` da confrontare
contro le catture vendor sotto `router-data/`.
Non modifica nessun file dello scratch.

## Come funziona

- **Nessun `#ifdef` nei sorgenti scratch.** Ogni accessor HW low-level
  (`b43_phy_read/write/mask/maskset/…`, `b43_radio_*`, `b43_read16`,
  `b43_write16`, `b43_actab_*`, `b43_mac_*`, `bcma_*`) è intercettato al
  linker con `-Wl,--wrap=<sym>`. La lista completa è in `Makefile`
  (variabile `WRAP_SYMS`).
- **`wrap.c`** fornisce `__wrap_<sym>` per ogni simbolo: emette una
  riga wl-diag su stdout, aggiorna un mirror di memoria in-process per
  le write, e ritorna il valore corretto per le read (vedi sotto).
- **`main.c`** monta un `struct b43_wldev` fittizio con la config della
  board scelta (D6220 2x2, DSL-3580L 2x2, agcombo 3x3), registra i
  read plans, e chiama uno dei flow pubblici.
- **`stubs/`** contiene i minimi header kernel (`linux/{types,kernel,
  delay,slab,errno}.h`) e i minimi header b43 (`b43.h`, `phy_common.h`,
  `main.h`) necessari a compilare i .c dello scratch senza il tree del
  kernel.
- **`test_harness.h`** è l'API pubblica del framework (`b43_test_*`);
  è inclusa solo da `main.c` e `wrap.c`. Il codice scratch non ha
  visibilità né dipendenze verso il framework.
- **`compare.py`** normalizza una cattura wl-diag (rimuove timestamp,
  numero episodio, colonna cpu, unifica `PHY.OR`/`PHY.AND` a
  `PHY.MOD`) e diffa contro l'output del test. Applica anche il
  **perimetro**, cioè scarta le op vendor che appartengono a codice fuori
  dall'unità sotto test — shared memory del MAC, template RAM, OTP, SROM —
  perché l'harness compila solo `src/` e non ha nessuno che le emetta. È il
  default, non un'opzione: su ogni cattura in repo un confronto senza
  perimetro si ferma sulla prima op del core. `--senza-perimetro` lo
  disattiva per ispezionare una cattura, e il numero che ne esce non è una
  misura del port. Le liste e i motivi sono in `PERIMETER` dentro il file.

## Read plans (mappa associativa scriptata)

Ogni read wrappata risolve così:

1. Se l'indirizzo ha un plan registrato: ritorna `results[iter]` (o 0
   se `iter >= cap`) e incrementa `iter`.
2. Altrimenti: ritorna il valore del write-mirror (l'ultima cosa
   scritta a quell'indirizzo, o 0 se mai scritta).

API:

```c
#include "test_harness.h"

/* prima di lanciare il flow */
static const u16 poll_0x270[] = {
    /* est#1 */ 0x0001, 0x0001, 0x0001, 0x0000,
    /* est#2 */ 0x0001, 0x0001, 0x0001, 0x0000,
    /* ... */
};

b43_test_plans_reset();
b43_test_plan_phy_reads(0x0270, poll_0x270,
                        sizeof(poll_0x270)/sizeof(u16));
/* similmente: b43_test_plan_radio_reads(...), _mmio_reads(...) */
```

Al termine del flow, `b43_test_plans_report(stderr)` stampa quanti
elementi di ciascun plan sono stati consumati — utile per verificare
che il codice sia arrivato dove ci si aspetta e non abbia terminato in
anticipo per un poll che non è mai stato registrato.

Un helper Python (TODO) estrarrà i plans da una trace vendor contando
le RD consecutive per indirizzo tra due WR e generando una tabella C
da incollare in `main.c`.

## Build e run

```sh
make                # compila ac_trace
./ac_trace                         # default: rxiq_est_debug su D6220
./ac_trace rxiq_est_debug d6220 > trace.d6220.out
./ac_trace rxiq_est_debug agcombo > trace.agcombo.out
```

Flow disponibili (`argv[1]`): `rxiq_est_debug` (default), `rxiq_comp`,
`rxiqcal`, `op_init`, `rfkill`, `switch_channel`, `full`, `periodic`,
`crsmin`. Board (`argv[2]`): `d6220` (default), `agcombo`, `dsl`.

`periodic` esegue un tick del watchdog a regime (poll TSSI/statistiche SHM
+ una tornata di measure block). Le letture SHM e TSSI sono stato ucode e
vengono dall'oracolo; la stessa cattura fa da riferimento per il confronto:```sh
AC_READ_ORACLE=../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt \
    ./ac_trace periodic d6220 > gen.periodic.out
python3 compare.py ../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt gen.periodic.out
# MATCH, 496/496 op; su stderr l'oracolo deve chiudere con 0 code esaurite
# e 0 non consumati.
```

`switch_channel` è il flow più ampio: guida l'intera pipeline
`b43_phy_ac_op_switch_channel`. Su D6220 ch36 emette ~22k operazioni e
consuma per intero ogni read plan registrato in `main.c`:

```sh
./ac_trace switch_channel d6220 > trace.switch.d6220.out
# a fine run, su stderr, la plan-consumption deve mostrare iter=N/N per
# ogni indirizzo: nessun underrun (flow terminato in anticipo) né overrun.
```

Nota: il nome del flow da passare sulla riga di comando è `switch_channel`;
`b43_phy_ac_op_switch_channel` è il nome della op kernel che il flow
invoca, non la stringa da passare a `argv[1]`.

Il binario stampa la trace su stdout, i log del driver (`b43dbg`,
`b43err`) su stderr. Il mirror di memoria simula la chiusura del bit
START di 0x0270 al primo read post-scrittura, così il poll del
correlatore non va in timeout.

## Confrontare con la vendor trace

### Flow `full` con oracolo (la validazione canonica)

Confronta la trace di `full d6220` contro
`router-data/d6220/wl-diag-wl1-attach-to-bss-up-ch36-bw20.txt`, la sola cattura
ch36 completa e con i valori letti. `AC_READ_ORACLE` serve i valori veri, quindi
il confronto verifica anche cio' che il driver *calcola* e non solo la sequenza.

```sh
python3 ../reverse-tools/merge_retvals.py \
    ../router-data/d6220/wl-diag-wl1-attach-to-bss-up-ch36-bw20.txt /tmp/att.merged.txt
AC_READ_ORACLE=/tmp/att.merged.txt \
    ./ac_trace full d6220 > trace.full.d6220.out
python3 compare.py /tmp/att.merged.txt trace.full.d6220.out --range 50:30172
```

### Flow `switch_channel`: e' il bring-up **successivo**

Il nome inganna -- l'op del driver e' `op_switch_channel` e quello che esegue e'
un bring-up successivo, non l'impostazione di un canale su un PHY gia' su.
### Il contatore del MAC e' il default

`b43_mac_suspend`/`enable` in-tree sono **annidabili**: toccano MACCTL solo
sulle transizioni. Dalla versione con `compare_lcs.py` quello e' il default e
non serve piu' passare `AC_MAC_REFCOUNT=1`; `AC_MAC_REFCOUNT=0` torna al
comportamento precedente, una `MAC.MCTRL` per chiamata.

Il default era il contrario. La misura:

|  | MAC.MCTRL emesse | similarita' |
|---|---|---|
| `full`, senza contatore | 32 (vendor 119) | 49.50% |
| `full`, con contatore | 113 (vendor 119) | **99.30%** |
| `switch_channel`, senza | 124 (vendor 119) | 99.19% |
| `switch_channel`, con | 124 | 99.19% |

Su `switch_channel` il flag **non ha effetto**: quel percorso non annida. Quindi
l'avvertenza che stava nel codice -- "col contatore il MATCH di switch_channel
cade" -- non si osserva.

### Due metriche, e non sono confrontabili

| strumento | cosa misura | flow1 |
|---|---|---|
| `compare.py` | il port emette **esattamente** la stessa sequenza | 1017 disallineamenti |
| `compare_lcs.py` | quanto del vendor e' riprodotto **in ordine** | 99.96%, 12 regioni |

La differenza non e' cosmetica: dieci op di lunghezza diversa sfasano il
confronto posizionale da quel punto in avanti, e `compare.py` conta 1017
divergenze dove l'altro ne conta poche decine. Le percentuali nei documenti di
questo repo vengono da `compare_lcs.py`.

**Nota di provenienza**: `compare_lcs.py` e' la ricostruzione di uno script di
lavoro non versionato, e ora torna: `flow2` da' 21000/21007 identico
all'originale, `flow1` 25002/25013 contro 24998, dove le quattro op di
differenza sono le `MAC.MCTRL` aggiunte a `phy_ac.c` dopo che quei numeri erano
stati presi.

Le normalizzazioni sono tutte **ricavate dai dati**, non assunte:

- si escludono `SI.COREREG` e `PMU.PLL`, che l'harness non simula (i
  denominatori 25013 e 21007 vengono da li');
- la larghezza degli esadecimali e i campi `ret=`/`a5=`/`a6=`, che solo la
  cattura ha;
- il `cpuN`, perche' l'harness non simula lo scheduling;
- il rendering di `AND`/`OR`: il vendor scrive `val=0x4000 (set 0x4000)` dove
  l'harness scrive `mask=0x0`. Su 163 coppie allineate il valore coincide
  **sempre** e la maschera del port e' **sempre** zero, 93 volte con `(clr)` e
  70 con `(set)`. Le `PHY.MOD` vere hanno gia' la stessa forma da entrambe le
  parti.

### I due gate sull'attach, con il set di hook esteso

Le catture storiche in `router-data/d6220/` non hanno `OBJ.*`, `TPL.*`,
`MAC.BW`, `OTP.*` ne' `SROMCTL`: come oracoli sono cieche a ~1600 op, e appena
il
port comincia a emettere scritture in shared memory ogni una risulterebbe una
divergenza. Le due nuove:

```sh
# l'attach vero e proprio, tabelle complete comprese le per-core
python3 ../reverse-tools/merge_retvals.py \
  ../router-data/d6220/wl-diag-wl1-attach-ch36-bw20-tabelle-complete.txt \
  /tmp/att2.merged.txt
AC_READ_ORACLE=/tmp/att2.merged.txt ./ac_trace full d6220

# il preambolo del probe: GPIO, core enable, OTP, PLL, test SHM
python3 ../reverse-tools/merge_retvals.py \
  ../router-data/d6220/wl-diag-wl1-attach-ch36-bw20-con-preambolo.txt \
  /tmp/pre.merged.txt
AC_READ_ORACLE=/tmp/pre.merged.txt ./ac_trace op_init d6220
```

**Due gate e non una traccia cucita.** I due attach divergono 68 record dopo il
punto in cui si agganciano (`OBJ.WR 0x0790` a `0x0500` contro `0x0300`), quindi
innestare il preambolo di uno sull'altro darebbe una giunzione inventata -- e un
oracolo vale per l'ordine, quindi una divergenza misurata su una cucitura non
distingue un errore del port da un artefatto. Il preambolo sta *prima* del punto
di divergenza, quindi quella regione e' incontestata e si misura da sola.

Va quindi confrontato con `down-to-bss-ch36-bw20`, non con l'attach, e con
`AC_FIRST_INIT=0`:

```sh
python3 ../reverse-tools/merge_retvals.py \
    ../router-data/d6220/wl-diag-wl1-down-to-bss-ch36-bw20.txt /tmp/d2u.merged.txt
AC_FIRST_INIT=0 \
    AC_READ_ORACLE=/tmp/d2u.merged.txt AC_READ_ORACLE_FROM=653 \
    ./ac_trace switch_channel d6220 > trace.switch.d6220.out
python3 compare.py /tmp/d2u.merged.txt trace.switch.d6220.out \
    --range 653:26671 --auto-align
# prima divergenza @2177 (banco 0x0910, vedi docs/retrace-todo.md)
```

`AC_READ_ORACLE_FROM` e' necessario: le code dell'oracolo sono per indirizzo e
in ordine, quindi caricare la cattura dall'inizio per un flow che ne esegue una
fetta fa consumare i valori delle letture precedenti.

Senza `AC_FIRST_INIT=0`, o confrontato con la cattura di attach, il flow diverge
su tutto cio' che dipende dalla fase -- il cap del TX-LPF viene dall'rccal di
`op_init` e produce `0x50db` dove il driver stock scrive `0x52db`. Non e' un bug
del driver, e' il gate sbagliato.

Output atteso:

```
aligning test at offset 2 (auto: 'PHY.RD   addr=0x019e val=UNDEFINED')
vendor: 22268 ops
test:   22268 ops
MATCH
```

- `--range 32887:55154`: l'estremo basso è l'episodio della prima
  `PHY.RD 0x019e` del blocco di channel-programming; salta le ~489 op di
  preambolo attach del vendor (MAC/PMU/setup, ep 32398..32886). L'estremo
  alto è l'ultimo episodio della cattura.
- `--auto-align`: salta le 2 op di prologo dell'harness (il `MAC.MCTRL`
  di disable e la `PMU.RC`), agganciando `test[2]` a `vendor[489]`.
- **Il poll di `0x0270` e i read plan.** Il numero di letture di `0x0270` non
  e' un parametro del driver: il bit 0 e' il flag di start della misura RX-IQ,
  l'hardware lo azzera e il driver rilegge finche' resta alto. Il valore letto
  governa quindi quante op vengono emesse, e deve arrivare da una cattura. Con
  `AC_READ_ORACLE` arriva da li'; senza oracolo arriva da
  `readplan_0270.h`, generato dalle catture con i RETVAL:

  ```sh
  python3 ../reverse-tools/gen_readplan.py 0x0270 \
      d6220_first=../router-data/d6220/wl-diag-wl1-attach-to-bss-up-ch36-bw20.txt \
      d6220_next=../router-data/d6220/wl-diag-wl1-down-to-bss-ch36-bw20.txt \
      agcombo=../router-data/agcombo/agcombo-wl1-4360-rescan-to-bss-ch36.txt \
      dsl=../router-data/dsl3580l/wl-diag-wl1-down-to-bss-ch36-bw20.txt \
      > readplan_0270.h
  ```

  La scelta del plan e' su board e fase (`AC_FIRST_INIT`), gli stessi assi da
  cui dipendono i conteggi. Verificato: con il plan la struttura dei poll
  coincide con la cattura sia su d6220 (12 run, `[5,5,5,5,29,1,27,1,22,1,41,1]`
  sul down->up e `[5,5,5,5,44,1,14,1,42,1,43,1]` sull'attach) sia con
  l'oracolo attivo. Per agcombo esiste una sola cattura con i RETVAL (un
  rescan, fase successiva) e per il DSL solo dei down->up: la stessa serve
  entrambe le fasi, ed e' un'approssimazione dichiarata.

- **I poll non si collassano.** Il numero di letture di `0x0270` non è un
  parametro del driver: è l'osservabile di un'attesa, e il driver esce quando
  il bit 0 si azzera. Con un oracolo che serve i valori reali il conteggio
  segue la cattura da sé, quindi il confronto resta op-per-op e la lunghezza
  è confrontabile. Un conteggio che non torna è un difetto da guardare, non
  rumore da nascondere.

### Sotto-finestra: solo il blocco RXIQ

Per isolare un singolo blocco (es. la calibrazione RX-IQ) si estrae la
finestra corrispondente dalla `down→bss-up` annotata:

```sh
python3 compare.py \
    ../router-data/d6220/wl-diag-wl1-attach-to-bss-up-ch36-bw20.txt \
    trace.d6220.out \
    --range LO:HI --auto-align
```

Attenzione agli indici: gli esempi di finestra che girano nei commenti e nei doc
(`82499:83540` e simili) vengono da trace annotate che **non sono in questo
repo**. Vanno ricalcolati sulla cattura che si usa davvero.

- `--range LO:HI` estrae la finestra del blocco d'interesse dal file
  vendor.
- `--auto-align` cerca in `test` la prima op che matcha `vendor[0]` e
  usa quell'indice come inizio del confronto. Utile quando il flow di
  test fa un prologo (save-gain, save-tone, ...) che il vendor non
  emette. In alternativa `--align-on OP` pinna l'allineamento su un op
  specifico.

Formato ops (allineato al vendor):

| Kernel call                          | Trace emesso                              |
|--------------------------------------|-------------------------------------------|
| `b43_phy_read(reg)`                  | `PHY.RD  addr=X val=UNDEFINED`            |
| `b43_phy_write(reg, val)`            | `PHY.WR  addr=X val=Y`                    |
| `b43_phy_mask(reg, kmask)`           | `PHY.MOD addr=X val=<kmask> mask=0x0000`  |
| `b43_phy_set(reg, kset)`             | `PHY.MOD addr=X val=<kset>  mask=0x0000`  |
| `b43_phy_maskset(reg, kmask, kset)`  | `PHY.MOD addr=X val=<kset>  mask=<~kmask>`|

`compare.py` normalizza le varianti `PHY.OR`/`PHY.AND` del vendor allo
stesso formato `PHY.MOD` singolo.

### Cross-driver: agcombo (OEM 7.14) contro l'ordine D6220

La cattura agcombo viene da un driver OEM piu' vecchio (7.14.43) di
quello che ha prodotto l'ordine replicato dal port. La semantica del
riordino distingue due livelli: il MACRO ordine (fasi intere in punti
diversi del flow) e' una scelta architetturale lecita della versione e
viene normalizzato; il MICRO ordine dentro le fasi e' probabile bug o
differenza 4352/4360 e viene PRESERVATO, cosi' compare.py a valle lo
mostra come cluster di mismatch localizzato accanto ai mismatch di
valore:

```sh
./ac_trace switch_channel agcombo > trace.agcombo.out
python3 ../reverse-tools/collapse_trace.py \
    ../router-data/agcombo/agcombo-wl1-4360-down-to-bss-ch36.txt v.col
python3 ../reverse-tools/collapse_trace.py trace.agcombo.out h.col
python3 ../reverse-tools/reorder_trace.py h.col v.col \
    --out-vendor v.reord --out-ref h.match \
    --res-vendor v.only --res-ref h.only --replicate
python3 compare.py v.reord h.match
```

Numeri attesi (oggi): 7573 op accoppiate (87.5% del riferimento) di cui
7382 dallo scheletro monotono e 191 da blocchi macro spostati (168
replicate: blocchi che il 7.14 esegue una volta dove il driver nuovo li
ripete, es. i readback rxgain 0x09aX); 80 blocchi macro; 351 mismatch.
Le fasi da channel_switch_prep a rxcal compreso sono a 0 mismatch di
valore fino a idle_tssi escluso: il residuo di valore vive in
idle_tssi/rxcal (tone_mode 0x?34 dello sweep, radio core-2 0x0445,
bassi di 0x?45) e nella coda est (54% di copertura). Il resto dei
mismatch e' micro-ordine preservato: il 7.14 legge i registri mute
core-N prima di pulire iqMode su 0x0270 (il port dopo), e piazza i
toggle MAC.MCTRL prima dei readback rxgain invece che dopo -- da
vagliare uno a uno come bug o differenza 4352/4360. La fase
phy_channel_setup resta al 93% di copertura per il blocco RF-seq, che
esiste 1:1 su entrambi i lati ma con micro-ordine 7.14 (arming
per-core: 0x0160/0x0401 core-select a 0x0001 invece del coremask) che
il matcher non riesce ad accoppiare: e' segnale di versione, non un
buco del port. I residui non sono rumore: `v.only` e' il lavoro che
solo il 7.14 fa (gain-cal core-2 su tabella 0xc off 0x6b/0x7b, blocco
rxiq a fine cattura), `h.only` quello che solo il driver nuovo fa (RAD
0x020e/0x036e).

Limite noto: --replicate attinge dalla stessa cattura. Se il blocco non
c'e' affatto (es. le catture rescan non passano mai da init), serve una
seconda cattura come donatrice -- estensione futura.

## Interpretazione del diff

Con format e align a posto, un diff non-vuoto evidenzia una di tre
categorie di causa:

1. **Peek mancante nel codice scratch.** Il vendor emette una
   `PHY.RD` "gratuita" (valore scartato) prima di alcune MOD; se nel
   diff appare `vendor: PHY.RD ... / test: PHY.MOD ...` all'inizio di
   una sequenza, molto probabilmente il codice scratch dovrebbe fare
   una `(void)b43_phy_read()` prima di quella MOD. Verificato su
   `gate_setup` con 0x0400 (peek assente).
2. **Ordine diverso.** Op emesse in ordine diverso; il codice scratch
   ha scritto la sequenza in modo differente dal vendor.
3. **Valore diverso.** Address giusto ma val/mask diversi; bug di
   porting nel valore hard-coded o nell'uso del mask.

Nessuna delle tre è "colpa" del framework: sta segnalando divergenze
reali fra il codice scratch e la trace vendor, che è esattamente lo
scopo.

## Estendere il set di flow

Oggi `main.c` cabla sei flow:

- `rxiq_est_debug` — Phase 1 sweep only (rxiqcal_phy_ac.c).
- `rxiq_comp` — solo `b43_phy_ac_rx_iq_comp_update` sui tre core.
- `rxiqcal` — Phase 1+2+3, ma resta gated da
  `B43_PHY_AC_RXIQCAL_REGMAP_FILLED == 0` dentro rxiqcal_phy_ac.c e
  ritorna presto senza toccare l'HW. Per attivarlo servirebbe cambiare
  quel define nel sorgente scratch: fuori dallo scope di questo harness
  (che non modifica lo scratch).
- `op_init` — `b43_phyops_ac.init` in isolamento.
- `rfkill` — `b43_phy_ac_op_software_rfkill` (bring-up radio 2069).
- `switch_channel` — l'intera pipeline `b43_phy_ac_op_switch_channel`
  (channel prep, table-7 program, radio 2069 channel setup, RX-IQ cal,
  finalize). È il flow con la copertura più larga: su D6220 ch36 emette
  ~22k operazioni.

Il full driver è già in `SCRATCH_SRCS_FULL` (`rxiqcal_phy_ac.c`,
`tables_phy_ac.c`, `phy_ac.c`, `radio_2069.c`) e la `SRCS` di default
lo usa: `make` compila e linka senza toccare i sorgenti scratch.

Per aggiungere un flow nuovo:

1. Se serve un altro .c dello scratch non ancora compilato, aggiungilo a
   `SCRATCH_SRCS_FULL` nel `Makefile`.
2. Compila; eventuali errori "field X of struct Y not declared" si
   risolvono aggiungendo il campo a `stubs/b43.h`.
3. Errori "undefined reference to `<sym>`" al link: se `<sym>` è un HW
   accessor che vuoi tracciare, aggiungilo a `WRAP_SYMS` e scrivi un
   `__wrap_<sym>` in `wrap.c`. Se è un helper che non vuoi tracciare,
   forniscine uno stub no-op in `wrap.c` (senza `__wrap_`).
4. Aggiungi il case in `main.c` sotto `argv[1]`, con gli eventuali read
   plan/pre-seed del mirror che il flow richiede.

## Stato oggi

Con gli stub attuali (senza tocchi ai sorgenti scratch) tutti i .c in
`SCRATCH_SRCS_FULL` compilano e linkano: `make` produce `ac_trace`
pulito.

| File scratch          | Compile | Note                                    |
|-----------------------|---------|-----------------------------------------|
| `rxiqcal_phy_ac.c`    | ✓       | build+run+trace ok con rxiq_est_debug   |
| `tables_phy_ac.c`     | ✓       |                                         |
| `radio_2069.c`        | ✓       | compila senza toccare gli stub          |
| `phy_ac.c`            | ✓       | compila e linka; abilita il flow `switch_channel` |

Il flow `switch_channel` gira end-to-end: su D6220 ch36 emette 22276 righe
di trace, ritorna 0, e la plan-consumption mostra `iter=N/N` per ogni
read plan (nessun underrun/overrun). La op kernel invocata è
`b43_phy_ac_op_switch_channel`.

Copertura rispetto al vendor: `switch_channel` copre la porzione di
channel-switch della sequenza `down→bss-up`, non l'intera cattura. Il
vendor `d6220-trace2` include un preambolo (GPIO, PMU-PLL, init radio)
che questo flow non riproduce, quindi un `compare.py` senza `--range`
diffa liste di lunghezza diversa. Per confronti mirati usare `--range`
sulla finestra del blocco d'interesse, come nell'esempio RXIQ sopra.

## Cosa il framework NON simula

- **HW dinamico**: read ritornano solo l'ultimo write, non ci sono
  bit read-only che rispondono a stimoli (temperature sensor,
  rxpower detector, ecc.). Se un test vuole vedere quella logica,
  serve un modello nel mirror.
- **Timing**: `udelay`/`msleep` sono no-op. L'ordinamento è preservato
  (single-threaded), ma non le finestre reali.
- **Race con MAC**: le funzioni `b43_mac_*` sono no-op. La finestra di
  quiesce MAC non viene simulata perché nessuna trace del vendor la
  richiede per il confronto.

## Copertura per funzione (marcatori `B43_AC_FN`)

Ogni funzione del driver è marcata con `B43_AC_FN()` (in `phy_ac.h`): no-op nel
kernel, nell'harness emette `----FN:nome----` all'ingresso e `----/FN:nome----`
all'uscita (via l'attributo `cleanup` di GCC, così i nesting sono corretti). I
marcatori escono **solo** con `AC_FN_MARKERS=1`, altrimenti il trace resta
identico al vendor e `compare.py` non si rompe.

```sh
# trace annotato per l'analisi di copertura
AC_FN_MARKERS=1 ./ac_trace rfkill d6220 > gen.rfkill.d6220.txt

# copertura per-funzione + gap, contro la cattura GREZZA (non collassata)
python3 ../reverse-tools/coverage_by_function.py \
    gen.rfkill.d6220.txt \
    ../router-data/d6220/wl-diag-wl1-down-to-bss-up_delay_only.txt

# localizzazione delle funzioni nel trace vendor
python3 ../reverse-tools/localize_functions.py gen.rfkill.d6220.txt <trace>
```

Le funzioni che scrivono tabelle (`tables_init`, `tables_zero_cal`) non si
misurano per sequenza: l'harness le emette come `PHY.WR` sul data-port, il
vendor le intercala con `TBL.WR/RD` in ordine diverso. Vanno confrontate per
contenuto delle celle.
