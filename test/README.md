# test/ — userspace trace-verification harness

Compila il codice del driver AC-PHY sotto `../src/` in
userspace e produce una trace nel formato di `wl-diag` da confrontare
contro le catture vendor sotto `reverse-output/` e `router-data/`.
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
  `PHY.MOD`) e diffa contro l'output del test.

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
make                # compila rxiq_trace
./rxiq_trace                         # default: rxiq_est_debug su D6220
./rxiq_trace rxiq_est_debug d6220 > trace.d6220.out
./rxiq_trace rxiq_est_debug agcombo > trace.agcombo.out
```

Il binario stampa la trace su stdout, i log del driver (`b43dbg`,
`b43err`) su stderr. Il mirror di memoria simula la chiusura del bit
START di 0x0270 al primo read post-scrittura, così il poll del
correlatore non va in timeout.

## Confrontare con la vendor trace

```sh
python3 compare.py \
    ../../../reverse-output/d6220-trace2-annotated.txt \
    trace.d6220.out \
    --range 82499:83540 --auto-align --squash-poll
```

- `--range LO:HI` estrae la finestra del blocco RXIQ dal file vendor.
- `--auto-align` cerca in `test` la prima op che matcha `vendor[0]` e
  usa quell'indice come inizio del confronto. Utile quando il flow di
  test fa un prologo (save-gain, save-tone, ...) che il vendor non
  emette. In alternativa `--align-on OP` pin allineamento su un op
  specifico.
- `--squash-poll` collassa i run di `PHY.RD 0x0270 UNDEFINED` in un
  singolo evento marker: nel vendor l'HW polla ~45x prima di
  completare, nel test il read plan scriptato lo simula in 4 iter.

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

Oggi `main.c` cabla due flow:

- `rxiq_est_debug` — Phase 1 sweep only (rxiqcal_phy_ac.c).
- `rxiqcal` — Phase 1+2+3, ma resta gated da
  `B43_PHY_AC_RXIQCAL_REGMAP_FILLED == 0` dentro rxiqcal_phy_ac.c e
  ritorna `-EOPNOTSUPP` senza toccare l'HW. Per attivarlo servirebbe
  cambiare quel define nel sorgente scratch: fuori dallo scope di
  questo harness (che non modifica lo scratch).

Per aggiungere un flow nuovo (es. `switch_channel`, che coprirebbe
l'intera pipeline `set_channel` + i side-effect):

1. In `Makefile`, aggiungi `phy_ac.c`, `radio_2069.c`,
   `farrow_phy_ac.c`, `rxgain_phy_ac.c` alla lista `SCRATCH_SRCS_MIN`
   (o passa alla lista `SCRATCH_SRCS_FULL` già preparata).
2. Compila; i primi errori saranno "field X of struct Y not declared" —
   aggiungi il campo a `stubs/b43.h`.
3. Errori "undefined reference to `<sym>`" al link: se `<sym>` è un
   HW accessor che vuoi tracciare, aggiungilo a `WRAP_SYMS` e
   scrivi un `__wrap_<sym>` in `wrap.c`. Se è invece un helper
   sconosciuto (es. `b43_nphy_tx_power_ctl_setup`), fornisci un
   no-op stub in `wrap.c` (senza `__wrap_`).
4. Aggiungi il case in `main.c` sotto `argv[1]`.

Per `todo_leftovers.c`: la compilazione tira dentro `bcma_chipco_*` e
`b43_r2069_rccal_*` che sono definiti altrove (`bcma_*` fuori b43,
`b43_r2069_rccal_*` in `radio_2069.c`). Da wrappare come no-op logger
in `wrap.c` — extension diretta ma non tentata in questo primo giro.

## Stato oggi

Compile status di ogni .c dello scratch con gli stub attuali (senza
tocchi ai sorgenti):

| File scratch          | Compile | Note                                    |
|-----------------------|---------|-----------------------------------------|
| `rxiqcal_phy_ac.c`    | ✓       | build+run+trace ok con rxiq_est_debug   |
| `tables_phy_ac.c`     | ✓       |                                         |
| `radio_2069.c`        | ✓       | compila senza toccare gli stub          |
| `farrow_phy_ac.c`     | ✓       | compila senza toccare gli stub          |
| `rxgain_phy_ac.c`     | 4 err   | manca `struct ssb_sprom_rxgains`, campo `rxgains_5gl` / `revision` in ssb_sprom |
| `phy_ac.c`            | 12 err  | manca enum `b43_txpwr_result`, campo `subband5gver`/`core_pwr_info` in ssb_sprom, `ESRCH`, `switch_analog` in `b43_phy_operations` |
| `todo_leftovers.c`    | non provato | dipende da `bcma_chipco_*` e chain `dev->dev->bdev->bus->drv_cc` — da wrappare come no-op logger |

I gap in `stubs/b43.h` sono meccanici: ogni "field X of struct Y" si
risolve aggiungendo il field allo stub, ogni `undeclared` un enum o un
#define aggiuntivi. Nessuna richiede modifica ai .c dello scratch. Un
primo giro di estensione dovrebbe portare `rxgain_phy_ac.c` e
`phy_ac.c` a compilare in ~30-60 minuti di lavoro.

Il flow `switch_channel` (che dà una trace confrontabile con l'intera
`down→bss-up` del vendor) diventa disponibile appena `phy_ac.c` linka:
`b43_phy_ac_op_switch_channel` è già puntata da `main.c` come TODO.

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
