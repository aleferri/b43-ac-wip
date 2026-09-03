# test/ — harness di verifica su trace

Compila il driver AC-PHY di `../src/` in userspace e produce una trace nel
formato di `wl-diag`, da confrontare contro le catture del vendor in
`../router-data/`. Non modifica nessun file di `src/`.

## Obiettivo

Che b43 emetta, una per una e nello stesso ordine, tutte le operazioni che il
driver stock emette su un attach a freddo: senza op mancanti, senza op di
troppo e senza valori sbagliati. Il punteggio dice quanto manca, il debito
residuo con il perche' di ogni pezzo sta in `../docs/retrace-todo.md`.

## La procedura, che e' una sola

**Non inventarne altre.** Ogni comando qui sotto e' quello con cui il repo
produce i numeri che cita; le varianti ad hoc danno risultati non
confrontabili.

### 1. Preparare le catture

```sh
unzip -d /tmp/cold ../router-data/d6220/cold-sweep.zip
```

I 26 segmenti stanno in `/tmp/cold/segmenti/coldNN-chC-bwB.txt`. Sono catture
grezze: le letture hanno `val=UNDEFINED` e il valore sta nella riga `RETVAL`
successiva. **Prima di usarle per una grep sui valori** vanno ripiegate:

```sh
python3 ../reverse-tools/merge_retvals.py \
    /tmp/cold/segmenti/cold01-ch36-bw20.txt /tmp/m01
```

Dimenticarlo e' l'errore piu' facile da fare: una `grep 'val=0x...'` su un file
non ripiegato non trova nessuna lettura e sembra che l'op non ci sia.

### 2. Compilare

```sh
make                     # ch36 BW20, la configurazione validata
make AC_ANY_CHANNEL=1    # per ogni altro canale o larghezza
```

Il guard di `set_channel()` rifiuta tutto cio' che non e' fra le configurazioni
validate, salvo il secondo build. Se il port emette poche migliaia di op invece
di ventimila e' quello, e `gates.sh` lo dice da se'.

**Ricompilare senza `AC_ANY_CHANNEL=1` prima di chiudere**, o il gate di
riferimento gira su un binario che difende meno.

### 3. I due gate, che sono la verifica canonica

```sh
./gates.sh                                             # cold01 ch36 bw20
./gates.sh /tmp/cold/segmenti/cold05-ch52-bw20.txt     # un altro segmento
./gates.sh /tmp/cold/segmenti/cold[0-9][0-9]-ch*.txt   # tutti e 26

AC_READ_ORACLE=../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt \
    ./ac_trace periodic d6220 > /tmp/p.out
python3 compare.py \
    ../router-data/d6220/wl-diag-wl1-steady-tick-ch36-bw20.txt /tmp/p.out
# deve stampare MATCH
```

`gates.sh` fa tutto da se': ripiega la cattura, ricava la finestra dalla prima
op PHY dell'attach, ricava la schedule dei tick con `probe_schedule.py`, lancia
il flow `full` con l'oracolo di lettura e chiama `cmp_skip.py` e `compare.py`.
Non serve rifarne i passi a mano, e farlo a mano sbaglia la finestra.

Il gate periodico va rilanciato a **ogni** modifica: e' l'unico confronto
posizione-per-posizione che sta a `MATCH`, quindi e' il rilevatore di
regressioni piu' sensibile che ci sia.

### 4. Leggere il punteggio

```
grezzo          : 26670/29292 = 91.05%   439 regioni
                  428 col valore sbagliato, 1673 op di wl mancanti,
                  93 op del port di troppo
nel perimetro   : ...
```

Il denominatore di **`grezzo`** e' l'unione dei due flussi: fa 100% solo se
coincidono. Le tre voci sono tre lavori diversi e non vanno sommate a occhio:

- **valore sbagliato** — registro giusto, numero no: c'e' una formula da
  trovare;
- **op di wl mancanti** — c'e' codice da scrivere;
- **op del port di troppo** — c'e' un gate da mettere, o una fase che sul
  vendor non gira.

**`nel perimetro`** toglie le op di codice fuori da `src/` e serve a navigare,
non a dare un punteggio. Il numero da citare e' `grezzo`.

### 5. Trovare la prossima divergenza

```sh
python3 compare.py /tmp/m01 /tmp/gate.full --range 528:36542 --auto-align
```

`compare.py` e' posizione-per-posizione e si ferma alla prima divergenza col
contesto: e' lo strumento per navigare. `gates.sh` lo lancia da se' e stampa il
primo `@N`. Un'op mancante sfasa tutto quello che segue, quindi `@N` dice dove
guardare, non quante cose sono rotte.

Per capire **chi** emette un'op nel port:

```sh
AC_FN_MARKERS=1 AC_CHANNEL=36 AC_BW=20 AC_FIRST_INIT=1 ./ac_trace full d6220
```

annota l'output con `----FN:nome----`.

Attenzione ai file temporanei: `gates.sh` scrive sempre in `/tmp/gate.merged` e
`/tmp/gate.full`. Analizzarli dopo aver lanciato il gate su **un altro**
segmento significa leggere i file del segmento sbagliato.

### 6. Prima di dire che una fase e' assente nel vendor

Serve un **testimone**: un registro o una tabella che nel port solo quella
funzione tocca. Si conta su tutti i segmenti, non su uno:

```sh
for s in /tmp/cold/segmenti/cold[0-9][0-9]-ch*.txt; do
    printf '%-24s %s\n' "$(basename $s)" "$(grep -c 'addr=0x0380' $s)"
done
```

Il metodo trova solo le fasi che hanno un testimone esclusivo: una fase che
condivide tutti i suoi registri con altre non si vede cosi', e va detto invece
di concludere che non c'e'.

## Le tre liste di eccezione, e perche' esistono

Stanno in `compare.py`, ognuna con la ragione voce per voce. Sono l'unico posto
dove si dichiara che un'op non conta, e ogni voce e' un pezzo di obiettivo
sospeso: vanno tenute corte e argomentate.

### `SOLO_PORT` — op del port che l'oracolo non puo' contenere

Oggi una voce: **`AMT.*`**, la address match table. Il port la scrive per via di
`patches/0011`, ricavata dalla cattura a freddo del DSL-3580L; le catture del
d6220 non la hanno perche' l'hook su `wlc_bmac_write_amt` e' stato aggiunto
dopo che sono state prese. **Non e' un'op di troppo: e' un'op giusta senza
oracolo**, e ci resta finche' non c'e' un retrace del d6220 con quell'hook.

I casi legittimi per questa lista sono due e vanno distinti: un'op che b43 deve
fare per la sua struttura dove wl ne fa una diversa, che e' permanente, e un'op
giusta la cui controparte esiste ma non e' stata catturata, che e' temporanea
per definizione e si chiude con una ricattura. Tutto il resto e' il port che fa
qualcosa di troppo, e si corregge nel driver, non nella lista.

### `SOLO_VENDOR` — op che nessun codice b43 puo' emettere

Oggi una voce: **`MAC.BW`**, l'hook su `wlc_bmac_bw_set`. Il suo equivalente
GPL in brcmsmac fa `pi->bw = bw` e nient'altro, piu' un reset e un init del
PHY; in b43 la larghezza sta in `phy.chandef`, che `b43_phy_init()` imposta
prima che il PHY arrivi la', e non c'e' nessun registro da scrivere. E' un
confine di funzione che b43 non ha.

Ogni voce qui dichiara un pezzo di obiettivo **irraggiungibile**, quindi serve
la prova che non ci sia niente da emettere, non l'impressione.

### `PERIMETER` — op di codice fuori da `src/`

Shared memory del MAC, template RAM, OTP, SROM. Il criterio e' l'appartenenza
dimostrata da `b43.h`, **non** la raggiungibilita' da `src/`: quest'ultima e'
degenere, perche' farebbe salire il punteggio quando si toglie codice.

Va **ristretta ogni volta che il port impara a scrivere una cella**: il
perimetro scarta dal solo lato vendor, quindi una cella che il port emette e il
perimetro scarta diventa un'inserzione senza controparte e rompe il confronto
posizionale. E' successo.

## Come funziona l'harness

- **Nessun `#ifdef` nei sorgenti di `src/`.** Ogni accessor hardware
  (`b43_phy_read/write/mask/maskset`, `b43_radio_*`, `b43_read16`,
  `b43_write16`, `b43_actab_*`, `b43_mac_*`, `bcma_*`) e' intercettato al linker
  con `-Wl,--wrap=<sym>`. La lista e' in `Makefile`, variabile `WRAP_SYMS`.
- **`wrap.c`** fornisce `__wrap_<sym>`: emette una riga wl-diag, aggiorna un
  mirror di memoria in-process per le write, e ritorna il valore per le read.
- **`main.c`** monta un `struct b43_wldev` fittizio col profilo di board
  (D6220 2x2, DSL-3580L 2x2, agcombo 3x3), registra i read plan e chiama uno
  dei flow.
- **`stubs/`** ha i minimi header kernel e b43 per compilare `src/` senza il
  tree del kernel.
- **`test_harness.h`** e' l'API del framework, inclusa solo da `main.c` e
  `wrap.c`. Il codice di `src/` non vede il framework.

### Flow

| flow | cosa fa |
|---|---|
| `full` | l'attach completo: e' quello da usare contro un segmento a freddo |
| `periodic` | un tick del watchdog, contro l'oracolo steady-tick |
| `switch_channel` | il cambio di canale a caldo, contro un segmento dello sweep a caldo |
| `up`, `down`, `op_init` | pezzi, per lavoro mirato |

Un segmento a freddo e' un ciclo `up` intero, e il flow da usarci e' `full`,
non `switch_channel`.

### Doppioni del core in `main.c`

Alcune op che il vendor emette stanno in codice del core (`main.c` del kernel),
che l'harness non compila. Dove servono al confronto sono rispecchiate in
`main.c` del test — `emit_core_shm_chipinit()`, `emit_core_hostflags()`,
`emit_core_shm_macaddr()`, `emit_core_amt()` — con i valori presi dalla patch
corrispondente, cosi' che se la patch cambia il doppione diventa sbagliato e il
confronto lo dice.

**L'ordine conta piu' del valore**: una sola inversione fa scartare l'op dal
confronto. Su cold01 l'ordine e' AMT `#443`, chip init `#649-#658`, MAC in
shared memory `#661-#663`, host flag `#686-#690`, chanspec `#691`.

## Read plans

Per le letture che il driver consuma, l'harness serve valori scriptati invece
del mirror. Due modi:

- `AC_READ_ORACLE=<cattura>` — i valori vengono dalla cattura stessa,
  nell'ordine in cui compaiono. E' il modo canonico ed e' quello che `gates.sh`
  usa; `AC_READ_ORACLE_FROM=<episodio>` sposta il punto di partenza.
- `b43_test_plan_phy_reads()` e simili in `main.c`, per casi mirati.

A fine run, su stderr, l'oracolo stampa una riga come:

```
oracle: 6830 hit, 0 indirizzi senza voce, 0 code esaurite;
        339 indirizzi noti, 102 non consumati del tutto
```

Le due che devono essere **zero** sono `indirizzi senza voce` (il port ha letto
un indirizzo che la cattura non ha) e `code esaurite` (il port ha letto lo
stesso indirizzo piu' volte di quante la cattura lo abbia). Entrambe
invalidano il confronto da quel punto in avanti.

`non consumati del tutto` **non** deve essere zero e non e' un difetto:
l'oracolo carica ogni indirizzo che la cattura legge, compresi quelli che
legge il core, e il port non li tocca. Sulla corsa canonica di cold01 sono 102.

I read plan espliciti, quelli registrati in `main.c`, devono invece mostrare
`iter=N/N`: la' un underrun vuol dire che il flow e' terminato in anticipo e un
overrun che ha girato piu' del previsto.

## Cosa l'harness NON simula

- **Hardware dinamico**: una read ritorna l'ultimo write o il valore
  dell'oracolo. Non ci sono bit read-only che rispondono a stimoli.
- **Timing**: `udelay`/`msleep` sono no-op. L'ordine e' preservato, le finestre
  reali no.
- **Race col MAC**: le `b43_mac_*` sono no-op.
- **Scheduling**: single-threaded, e la colonna `cpuN` della cattura e' quindi
  normalizzata via.

## Copertura per funzione

```sh
AC_FN_MARKERS=1 ./ac_trace full d6220 > /tmp/annotato.txt
python3 ../reverse-tools/coverage_by_function.py /tmp/annotato.txt \
    /tmp/cold/segmenti/cold01-ch36-bw20.txt
```

La copertura si misura contro la cattura **grezza**, non ripiegata: i marcatori
si allineano agli episodi.
