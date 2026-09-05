# wl-diag — tracer inline-detour per il driver `wl`

Modulo kernel che aggancia gli accessor PHY/radio/PMU/MAC del driver Broadcom
`wl` senza kprobe (detour all'ingresso funzione), ed espone i record su un
misc-device. Vedi la testata di `wl_diag.c` per i dettagli del meccanismo e i
limiti (MIPS32R1, memoria modulo RWX, read con valore UNDEFINED).

Cosa cattura ora: PHY rd/wr/mod, **PHY and/or distinti** (op 19/20), radio
rd/wr/mod, PMU cc/rc/pll, GPIO ChipCommon, tabelle acphy, e il **controllo verso
il MAC** (`wlc_bmac_mctrl`, `wlc_bmac_mhf`) e la **object memory del MAC**
(`wlc_bmac_read_objmem16`/`_write_objmem16`, op `OBJ.RD`/`OBJ.WR`): il uCode
vi deposita fra l'altro il campione di potenza di rumore che la
`crs_min_pwr` cal legge, e quella lettura non passa da un registro PHY.

**Due hook non sono applicabili col detour a 4 parole**, ed e' bene saperlo
prima di flashare invece di scoprirlo dal log:

| funzione | perche' |
|---|---|
| `wlc_bmac_mhf_get` | `beq` alla parola 1: nemmeno lo short-j sta in piedi. Risolta patchando i **siti di chiamata**, vedi sotto. |
| `wlc_bmac_read_shm` / `_write_shm` | wrapper di 16 e 20 byte con `jr` alla parola 2. Per questo si aggancia il bersaglio della tail call, `read/write_objmem16`, che ha prologo pulito. |

### Il registro di rientro dello stub

Lo stub, dopo aver rieseguito le parole spiazzate, carica l'indirizzo di rientro
in un registro e ci salta. Quel registro **non puo'** essere uno che le parole
rieseguite scrivono, e il caso reale che lo ha imposto e' il prologo di un thunk
con tail-call: `lui $t9` / `addiu $t9`. Usando $t9 anche per il rientro si torna
a funzione+8 con $t9 uguale all'indirizzo di rientro invece del bersaglio,
l'`addiu` ci somma il lo16 e il `jr $t9` salta in mezzo a un'altra funzione.

Il sintomo e' inconfondibile e vale la pena riconoscerlo al volo:

    Unhandled kernel unaligned access
    epc : c2df18e4 wlc_bmac_core_phypll_ctl+0xa8 [wl]
    ra  : c2df3de4 wlc_bmac_mhf+0x154 [wl]
    $24 : 00000000 c2df18e4          <- t9 == epc

`$t9 == epc` dice che il salto era `jr $t9` con $t9 gia' sbagliato, e `ra`
identifica il chiamante: da lui si trova quale funzione agganciata sta in mezzo
(qui `wlc_bmac_mhf` chiama `wlc_bmac_write_shm` e rientra a +0x154).

Lo stub sceglie: $t9 se le parole rieseguite non lo scrivono, altrimenti $t8. Se
le scrivono entrambe l'hook viene scartato in pianificazione, perche' un rientro
calcolato male e' peggio di un hook in meno.

### Prologhi non agganciabili, e le due vie

Alcune funzioni hanno un branch nella finestra del detour, quindi ne' il detour
a 4 parole ne' lo short-j: `wlc_bmac_mhf_get` ha un `beq` alla parola 1, ed e'
per questo che `MAC.MHF.RD` e' 0 su 215 `MAC.MHF` in tutte le catture.

**Patch dei siti di chiamata** (preferita). Il modulo e' `-mabicalls`: zero
`jal` in `.text`, le chiamate sono `lui`/`addiu` + `jalr`, o `jr $t9` per le
tail call. Si riscrive la coppia perche' carichi lo stub; la funzione resta
intatta e funziona su entrambi i kernel. Tre condizioni verificate a runtime:
indirizzo esatto, salto sullo stesso registro, `addiu` non condiviso.
`reverse-tools/callsites_pic.py` fa la stessa analisi offline su un blob.

| funzione | 6.30 | 7.14 |
|---|---|---|
| `wlc_bmac_mhf_get` | 2 siti, 2 ok | 3 siti, 3 ok |
| object memory | `read_objmem`, 4 siti, 2 ok | `read_objmem16`, 4 siti, 2 ok |

**Percorso a `break`** (solo 3.4). Una parola, die notifier su `DIE_BREAK`:
`do_bp` lo chiama per `BRK_KPROBE_BP` fuori da `CONFIG_KPROBES`. Su 2.6.30 non
c'e' e si compila via -- `do_bp` va diretto a `do_trap_or_bp` e
`set_except_vector` non e' esportata.

**I nomi cambiano fra versioni** (`read_objmem` / `read_objmem16`, offset di
struct `0x84` / `0x88`) e agganciare il nome sbagliato non da' errore: l'hook
viene saltato e la cattura esce senza quella classe di op.

### Run unica su piu' canali

`reverse-tools/capture_plan.sh` fa lo sweep, e
`reverse-tools/split_trace.py` taglia la traccia decodificata in un file
per chanspec:

Le fasi sono **separate**, perche' una run intera produce troppi byte per
raccoglierli in un colpo. Una fase per volta, ognuna col suo file:

```sh
sh capture_plan.sh 20a          # sul device, con arm=1
sh capture_plan.sh 20b
sh capture_plan.sh 40
sh capture_plan.sh 80
python3 reverse-tools/split_trace.py --on gaps trace-20a.txt split/
```

A 40 e 80 MHz il **comando** vuole il canale basso del blocco (`5g36/40`, non
`5g38/40`), mentre il **chanspec** che ne risulta porta il canale **centrale**
(`ch=38 bw=40`). Sono due livelli diversi, entrambi veri.

Due cose viste alle prime catture, che rendono i record `CHANSPEC` inservibili
come confine:

- sono **in ritardo di un ciclo**: nella fase 20b compaiono `ch=40`, `ch=36`,
  `ch=60`, che sono della 20a. `wlc_phy_chanspec_set` viene invocata col
  chanspec corrente, non col nuovo. Per marcare i confini servirebbe agganciare
  anche `wlc_phy_chanspec_set_acphy`.
- ne arriva **uno solo per fase**, o zero.

Percio' `split_trace.py --on gaps` taglia sui **salti temporali** e prende i nomi
dall'ordine della fase, riportando i `CHANSPEC` trovati per verifica. Soglia di
default 1.03 s: lo `sleep 1` fra i cicli lascia 1.06 s, mentre il campionamento
periodico del rumore lascia buchi di 1.00 s esatti fra due `OBJ.RD`, e per
durata sarebbero indistinguibili.

**La coda: 131072 record di default sulla variante 3.4**, cioe' 3.5 MB e ~25 s
di margine, allocati con `vmalloc` e non `kfifo_alloc` -- quest'ultima usa
`kmalloc`, che vuole pagine contigue, e su un router frammentato qualche MB non
si trova. Si alza con `fifo_recs=262144` se l'allocazione riesce.

Ma un buffer grande assorbe le **raffiche**, non un ritmo medio superiore al
drenaggio: le 232 gocce da 2-3 record della fase 80 MHz dicono che la coda era
piena piu' volte, cioe' il lettore era in media piu' lento dello scrittore. In
quel caso si guarda il lato lettura, o si filtra di piu'.

**Il listener non va fermato fra i canali.** La fifo tiene `FIFO_RECS` = 32768
record; a circa 2500 record/s sono ~13 secondi di margine. Fermandolo, la coda
si riempie e il modulo emette un record `OP_DROP` col conteggio: utile per
sapere quanto e' andato perso, ma perso resta.

Il ciclo e' `{chanspec; up; attesa; down}`, quindi ogni canale ottiene un
**down->up**, non un attach. Su 3.4 la traccia puo' contenere 1 attach iniziale
(dal re-probe) + N down->up; su 2.6.30 solo N down->up, perche' il rescan
scarica `wl` e l'attach non e' catturabile in quel modo.

I segmenti sono tutti nella stessa fase e quindi confrontabili fra loro, ma
vanno confrontati col gate `switch_channel` e `AC_FIRST_INIT=0`, non con `full`
che modella l'attach. La distinzione non e' cosmetica: cal `crs_min_pwr`, primo
blocco del banco `0x0910` e doppia programmazione dell'analogico si comportano
diversamente nelle due fasi.

L'attesa dopo `up` lascia completare le calibrazioni asincrone -- crsmin, PAPD,
fdiqi -- che sono quelle che leggono dalla shared memory e dal template RAM.


`wlc_phy_chanspec_set` e' agganciata e emette `CHANSPEC` a ogni cambio, con il
chanspec grezzo in `addr`. Il decoder lo espande:

    CHANSPEC ch=36 bw=20 band=5g raw=0xd024

Cosi' una sola cattura puo' coprire 36/20, 36/40, 36/80 ... 140/20, 140/40,
140/80 e si taglia a posteriori sui record `CHANSPEC`. Si aggancia la generica
e non la variante acphy perche' scatta per ogni PHY ed e' piccola (96B su 6.30,
324B su 7.14, prologo pulito su entrambe).

Il decode usa il formato 802.11ac standard (`chan` bit 0-7, `bw` 0x3800, `band`
0xc000). **Assunzione, non verificata su cattura** -- nessuna delle catture
attuali contiene record `CHANSPEC`. Il campo `raw` c'e' apposta: se il decode
non torna, il valore grezzo resta leggibile.

### Copertura dell'I/O

Per spazio di memoria, non per nome di funzione:

| spazio | stato |
|---|---|
| registri PHY, radio, tabelle PHY | coperto |
| object memory / SHM | coperto (`OBJ.*`) |
| MAC mctrl + MHF, PMU, GPIO, core reg | coperto |
| **template RAM** | coperto da questo giro (`TPL.*`) |
| **OTP** | coperto (`OTP.*`) |
| I/O inline via `R_REG`/`W_REG` | **non agganciabile**: sono macro, non funzioni |

Il template RAM e' dove il PHY carica le forme d'onda dei toni, ingresso di
RXIQ, PAPD e `do_dummy_tx` (`wlc_phy_loadsampletable_acphy`,
`wlc_phy_sample_data_acphy` con 17 siti su `templateptr_wreg`). Non era coperto
da nessuna classe di op, quindi quei dati mancavano del tutto dalle catture --
lo stesso tipo di buco dello SHM.

Su 6.30 gli accessor `templateptr`/`templatedata` non esistono: la' si aggancia
solo `write_template_ram`.

L'OTP si aggancia al **livello generico** (`otp_init`, `otp_read_word`,
`otp_read_region`), che ha gli stessi nomi su 6.30 e 7.14 e prologo pulito,
mentre `hndotp_*`/`ipxotp_*` cambiano fra versioni. Il contenuto e' l'immagine
SROM -- statica e gia' nota dai dump raw -- quindi serve per sapere **quando**
viene letta e **quali word**, cioe' dove i valori vengono consumati. Il valore
finisce in un puntatore, non nel ritorno, quindi nel record c'e' il numero di
word e non il dato.

### Lo sweep non contiene un attach, e non associa

Il conto delle **parole** di tabella lo dice senza ambiguita':

| id tabella | `attach-to-bss` | `down-to-bss` | segmento dello sweep |
|---|---|---|---|
| `0x0c` | 463 | 427 | 42 |
| `0x0e` | 320 | 320 | **0** |
| `0x40` | 384 | 384 | 128 |
| `0x42` | 256 | 256 | **0** |
| `0x60` | 384 | 384 | 128 |
| `0x62` | 256 | 256 | **0** |
| `0x82` | 256 | 256 | **0** |
| totale | 3776 | 3740 | 1638 |

Quattro tabelle mancano del tutto e `0x40`/`0x60` sono a un terzo: 128 parole
invece di 384, cioe' **un core invece di tre**. E `0x42`/`0x62`/`0x82` sono la
stessa famiglia a stride `0x20`, i LUT di gain per core.

La differenza non e' attach contro up: la `down-to-bss`, che **non** e' un
attach, ha tutte le tabelle a 3740 parole. Quello che manca allo sweep e'
l'associazione -- `capture_plan.sh` fa `up` e aspetta, senza portare
l'interfaccia a stabilire il BSS -- e la programmazione per-core sembra avvenire
la'. E' un'ipotesi: la prova sarebbe una cattura `to-bss` con gli hook attuali,
che serve comunque per rifare gli oracoli dei gate.

Quindi lo sweep e' valido per **canale e larghezza**, dove conta cio' che varia
fra i 32 segmenti, e non copre l'inizializzazione per-core.

### Solo builtin negli script sul device

Su questi busybox mancano `head`, `awk` e altri: uno script che li usa muore a
meta' senza dirlo chiaramente. `capture_plan.sh` usa solo builtin della shell
piu' `wl` e `sleep`. `gen_syms` gira sul PC per lo stesso motivo.

### Conservare la fifo sui canali DFS

Sui canali DFS il rivelatore radar interroga `PHY.RD 0x0253` e `0x0254` in
continuo: **192000 e 194000 letture** nelle quattro fasi, l'85% di tutte le
letture PHY, e col `retcap` attivo il doppio. Non c'entrano con la
configurazione del canale, quindi per lo sweep di massa si filtrano prima della
fifo:

```sh
insmod wl_diag.ko arm=1 skipphyrd="0x253,0x254"
```

Due restrizioni deliberate, entrambe da verifiche sui dati.

Il filtro vale **solo per `OP_PHY_R`**: gli spazi di indirizzamento sono
separati
per classe, e nelle stesse catture ci sono 32 `OBJ.WR` a `0x252` e 32 a `0x254`
che sono offset di object memory, non registri PHY. Un filtro sul solo indirizzo
li avrebbe buttati in silenzio.

E si filtrano **solo `0x253`/`0x254`**. La testa del blocco -- `0x251` e
`0x252`,
lette 1558 volte in tutto, una per blocco -- e' plausibilmente lo stato e i dati
dell'impulso, cioe' la parte utile: costa poco e si tiene.

I record filtrati hanno un contatore separato dai persi, cosi' gli `OP_DROP`
restano un indicatore di perdita vera. A fine corsa:

    wl_diag: scaricato (persi: 0, filtrati: 148392)

**Le catture DFS vanno fatte senza filtro**, due o tre, perche' quelle letture
sono l'unico materiale sul rivelatore. Non serve di piu': il classificatore
ETSI/FCC Linux lo ha gia' in `drivers/net/wireless/ath/dfs_pattern_detector.c`
(377 righe), che consuma `struct pulse_event {ts, freq, width, rssi, chirp}` --
e i campi che il driver Broadcom stampa (`min_pw`, `pri`, `fm_min`/`fm_max`,
`nconsecq_pulses`) mappano su quelli. Quindi dei 24 KB di
`wlc_phy_radar_detect_run` serve solo il **formato dei quattro registri**, non
la
classificazione.

Attenzione alle unita': `pri=44258` e' troppo grande per essere microsecondi,
dato che i PRI della normativa stanno fra 200 e 3000 us. Da stabilire dalla
cattura, non per ipotesi.

### Se non arriva nessun RETVAL

Sintomo: tutte le letture hanno `val=UNDEFINED` e nella traccia non c'e' un solo
record `RETVAL`, quindi si ha la sequenza delle letture ma non i valori.

Prima verifica, una riga:

```sh
dmesg | grep 'trampolino ritorno'
```

Manca -> nessun hook eleggibile risultava `retcap`, e il trampolino non e' stato
costruito. C'e' -> e' costruito ma non viene raggiunto, e il sospetto e' il pool
di `wl_diag_enter_ret`, che restituisce `orig_ra` in silenzio quando le entry
sono esaurite.

**E' successo per davvero**, causa: tre campi di stato (`use_bp`, `use_sites`,
`bp_stub`) inseriti nella `struct hook` **fra `shortj` e `retcap`**. La tabella
usa inizializzatori posizionali, quindi il `true` destinato a `retcap` finiva in
`use_bp` e `retcap` restava falso per ogni hook. I campi nuovi ora stanno in
coda, e gli inizializzatori usano la forma designata (`.retcap = true`) che e'
immune al riordino.

### Init a freddo: un ciclo per canale, ricaricando `wl`

Un init a freddo si ottiene ricaricando il modulo bersaglio, non manomettendo la
struct del PHY: azzerare dallo stub il byte "gia' calibrato" darebbe una cal
completa dentro un `phy_init` **a caldo**, che non riproduce le costanti di fase
dell'attach -- coppia di gain ADC in `init_regs`, campo a 6 bit di `0x02e4`,
preambolo analogico. I flag adiacenti (250 "phy_init fatto", 249 POR) resterebbero
come sono.

`reverse-tools/cold_capture.sh` (solo 3.4) fa quindi un ciclo per canale, e con
l'armamento dinamico il ciclo e' tutto sul solo `wl`:

    MARK ; rmmod wl ; insmod wl ; chanspec ; [ssid] ; up ; attesa ;
    [bss up ; attesa] ; down

```sh
insmod /tmp/wl_diag.ko arm=1
cat /proc/wl_diag | nc <HOST> 5555 &
IF=wl1 SSID=test-ap sh cold_capture.sh 20 36 40 44 48
```

Cosa si guadagna rispetto allo sweep a caldo di `capture_plan.sh`:

- il primo `up` dopo il ricarico e' un `phy_init` con `do_full_init` vero e una
  `cal_init` mai fatta, senza toccare nessun flag;
- **c'e' anche l'attach**, perche' gli hook sono in piedi dal `COMING`, cioe'
  prima di `mod->init` e quindi prima del probe PCI. Non serve il remove/rescan
  del device, che riusciva circa una volta su due;
- il taglio a posteriori e' deterministico: `split_trace.py --on mark` sui record MARK,
  non l'euristica dei salti temporali;
- la fifo e' meno sotto pressione: il lettore resta aperto per tutta la corsa e
  non c'e' un `insmod` di mezzo, quindi `skipphyrd` si puo' lasciare vuoto e i
  canali DFS si catturano col rivelatore radar incluso.

L'attach cade sul chanspec di default e solo il primo `up` sul canale chiesto:
e' il `phy_init` per-canale che si vuole, il preambolo di probe non dipende dal
canale.

Due controlli che lo script fa e che conviene conoscere:

- se dopo il ricarico `wl -i wl1 isup` risponde `1`, il primo `phy_init` e' gia'
  avvenuto per mano di qualcun altro (hotplug del vendor, `wlconf`, `nas`) e si
  ferma invece di produrre un down->up etichettato come cattura a freddo. I
  demoni da terminare si passano in `KILL`;
- l'identita' dell'interfaccia si verifica sul `deviceid` di `wl revinfo`, non
  sul nome: la numerazione delle istanze la assegna l'ordine di probe, e
  catturare l'altra radio darebbe una traccia plausibile e sbagliata.

### Impostare il BSS: e' quello che mancava

Senza SSID i segmenti di uno sweep scrivevano **1638** parole di tabella; con
l'SSID impostato prima dell'`up` sono **3740**, cioe' identiche a una cattura
`down-to-bss`, id per id su tutte 22. Tornano le quattro tabelle che mancavano
(`0x0e`, `0x42`, `0x62`, `0x82`) e `0x40`/`0x60` passano da 128 a 384 parole,
cioe' da un core a tre.

Non era l'attach: era l'associazione. Quindi ogni ciclo di uno sweep con SSID e'
equivalente a una `down-to-bss` su un canale diverso, ed e' usabile come
oracolo.

### Sei configurazioni che il driver rifiuta, ed e' corretto

In uno sweep completo ne passano 26 su 32. Le assenti sono esattamente quelle il
cui blocco include i canali **120, 124, 128**:

| chiesto | canali coperti |
|---|---|
| `ch120/20`, `ch124/20`, `ch128/20` | se stessi |
| `ch116/40` | 116, **120** |
| `ch124/40` | **124, 128** |
| `ch116/80` | 116, **120, 124, 128** |

E' la banda **TDWR** (5600-5650 MHz), riservata ai radar meteo di terminale e
vietata alla trasmissione in molte giurisdizioni. Il rifiuto e' la regola, non
un
difetto dello script: `ch132` e oltre passano, perche' sono sopra la banda.

### Accessor che il port non fa affatto

Audit sistematico: si prendono gli accessor **chiamati da codice acphy** (non
tutti quelli plausibili, che sono 143 e in gran parte irrilevanti) e si vede
quali non sono agganciati. Ne restano tre dopo aver scartato i wrapper verso
funzioni gia' coperte:

| funzione | chiamata da | dove va nel port |
|---|---|---|
| `wlc_bmac_bw_set` | `chanspec_set_acphy`, `phy_init` | `set_channel` e `op_init` |
| `si_get/set_sromctl` | `attach_acphy` | il punto di `op_init` che corrisponde ad attach |
| `wlc_bmac_macphyclk_set` | `init_htphy`, `init_nphy`, `bmac_init` | **da nessuna parte**: non e' nel percorso AC-PHY |

Il port non ha **zero** riferimenti a `bw_set`, `macphyclk` o `sromctl`, e usa
`CHAN_WIDTH` solo in due punti. Quindi la larghezza al livello MAC non e'
toccata
da nessuna parte -- con solo BW20 il default passa inosservato, con 40 e 80 no.

Il **dove** viene dal grafo delle chiamate e non serve una cattura per saperlo:
il chiamante e' una funzione acphy, e quella ha una controparte nel port. La
cattura serve per la **posizione dentro** la funzione, che il grafo non da'.

### Il rumore passa dall'object memory

    wlc_phy_noise_read_shmem -> wlapi_bmac_read_shm -> wlc_bmac_read_shm
                             -> wlc_bmac_read_objmem[16]

Quindi `OBJ.RD` cattura anche il campione di rumore della `crs_min_pwr` cal.
Nel 7.14 `noise_read_shmem` chiama `wlc_phy_crs_min_pwr_cal_acphy`, nel 6.30
no: e' il motivo per cui il DSL scrive sempre zero nel banco `0x0910`.

`MAC.MHF.RD` invece e' ridondante: `mhf_get` e' un getter puro e tutte le
scritture sono gia' catturate (215 record, indici 0..4), quindi lo stato si
ricostruisce offline.


**Da quali simboli, secondo la build.** Su 7.14.89 la coppia e'
`wlc_bmac_read/write_objmem16`, e `aux` porta il selettore dello spazio. Su
**7.14.43 quegli accessor non esistono affatto**: ci sono solo la coppia bulk
`copyfrom/copyto_objmem` -- dove il valore sta in un buffer, quindi fuori
portata per un hook all'ingresso -- e i thunk `wlc_bmac_read/write_shm`, che la
tabella elenca come varianti. Coprono il **solo spazio SHM**: `aux` resta 0 e
gli accessi a SCR e IHR non compaiono.

I due thunk sono brevi (16 e 20 B) e hanno `jr $t9` dentro la finestra a 4
parole, quindi vanno di **short-j** a 2 parole; per i siti di chiamata non si
puo' passare, perche' `read_shm` ne ha ~34 e `MAX_SITES` e' 8. Una conseguenza
del meccanismo, non ovvia: in `write_shm` l'`andi 0xffff` e' la parola 1, cioe'
il delay slot della `j`, e si esegue **prima** dello stub -- quindi il valore
arriva gia' troncato a 16 bit. In `read_radio_reg` lo stesso `andi` e' la parola
0, che lo short-j sostituisce, e per questo la' `addr` e' grezzo.

Quando in una build esistono entrambe le vie, ne viene armata **una sola**: un
op ha un hook, e vince il primo della tabella che risolve e risulta
agganciabile. L'ordine in tabella e' l'ordine di preferenza.

## Le due versioni del tracer, e cosa le separa

`wl-diag/` gira sul kernel 3.4 contro `wl` 7.14, `wl-diag-2630/` sul 2.6.30
contro `wl` 6.30. Le tabelle `hooks[]` sono ora allineate, e le differenze che
restano sono imposte dai simboli dei due blob, non da scelte:

| hook | 6.30 | 7.14 |
| --- | --- | --- |
| `wlc_bmac_read/write_objmem` | `LOCAL` | assente |
| `wlc_bmac_read/write_objmem16` | assente | `LOCAL` |
| `wlc_bmac_read/write_shm` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_copyfrom/copyto_objmem` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_template*_reg` | assente | `GLOBAL` |
| `wlc_bmac_write_template_ram` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_set_addrmatch` | `GLOBAL` | assente |
| `wlc_set_addrmatch` | assente | `GLOBAL` |
| `wlc_bmac_write_amt` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_set_rcmta` | `GLOBAL` | `GLOBAL` |
| `phy_reg_write_array` | `GLOBAL` | `GLOBAL` |
| `phy_reg_read/write_wide` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_write_ihr` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_set_shm` | `GLOBAL` | `GLOBAL` |

Verificato con `readelf -s` sui due oggetti di riferimento. Gli accessor a 16
bit sono `LOCAL` in entrambi i blob, cioe' static, e `kallsyms_lookup_name`
trova i locali di un modulo solo con `CONFIG_KALLSYMS_ALL`: se non si
risolvono, la shared memory si vede comunque dai thunk `read/write_shm`, che
sono globali, mentre le altre regioni di object memory si vedono solo dalla
coppia bulk. Il log di `wd_init` dice quali hook si sono installati, e va
guardato prima di dedurre qualcosa da un'assenza.

### Le firme si leggono dal prologo, non dal nome

Un hook si scrive solo sapendo quale argomento porta l'offset, quale il valore
e quale la lunghezza, e se il prologo e' agganciabile. `reverse-tools/mipsdis.py
<oggetto> --prologo <simbolo>` stampa le prime otto istruzioni e dice se c'e' un
branch nella finestra delle quattro parole, che e' cio' che decide fra detour
classico, short-j e patch dei siti.

Gli accessor a 16 bit di object memory sono `LOCAL` in entrambi i blob, ma sul
firmware DSL si risolvono comunque: `kallsyms_lookup_name` li trova, quindi quel
kernel ha `CONFIG_KALLSYMS_ALL`. Da cui una conseguenza che va gestita: i thunk
`wlc_bmac_read/write_shm` tail-callano quegli accessor, e agganciare entrambi
darebbe DUE record per ogni accesso alla shared memory -- uno dal thunk, con
`aux = 0` per costruzione, e uno dall'ingresso dell'accessor, col selettore
vero. Il campo `ripiego_di` di `struct hook` serve a questo: `pianifica()`
scarta il thunk quando l'accessor di sotto si e' agganciato, e lo dice nel log.
I thunk restano per un firmware dove l'accessor non si risolve.

Sui cinque accessor aggiunti per ultimi, quattro firme su sei non erano quello
che il nome suggeriva:

| funzione | firma letta dal prologo |
| --- | --- |
| `phy_reg_write_array(pi, array, n)` | `a1` e' un PUNTATORE, `a2` il conteggio (`blez a2` esce). Nessun indirizzo. Dentro chiama `phy_reg_and` & co., quindi e' un marcatore e le singole scritture arrivano dagli hook a 16 bit |
| `phy_reg_write_wide(pi, val)` | nessun indirizzo: registro fisso, valore in `a1` |
| `phy_reg_read_wide(pi)` | nessun argomento utile, valore nel `RETVAL` |
| `wlc_bmac_write_ihr(hw, off, val)` | `off=a1`, `val=a2`; `a3` non esiste |
| `wlc_bmac_set_shm(hw, off, val, len)` | `off=a1`, `val=a2`, `len=a3` |
| `wlc_bmac_set_addrmatch(hw, idx, addr)` | `idx=a1`, `a2` e' un puntatore. Branch alla parola 2 -> **short-j** |

Registrare un puntatore in un campo `addr=` non e' un dettaglio: chi legge la
traccia lo prende per un indirizzo di registro.

Un candidato che non si aggancia non fa danno: `pianifica()` se ne accorge dal
prologo e lo salta con un `pr_warn`, quindi il costo di provarne uno e' un
avviso nel log.

I codici op sono gli stessi numeri nei due tracer e `decode-wl-diag.py` non
distingue le versioni: aggiungendone uno va aggiunto in coda a entrambi gli
enum, con lo stesso valore.

### Caldo/freddo: armamento dinamico su entrambi i kernel

Entrambi i tracer emettono i `MARK` -- l'etichetta di ciclo da
`echo "ch36 bw20" > /proc/wl_diag`, piu' `mod COMING` e `mod GOING` sulle
notifiche di modulo -- quindi una traccia si taglia in segmenti con
`split_trace.py --on mark`.

E entrambi riarmano: `COMING` rifa' il piano e applica le patch, `GOING`
ripristina, senza eccezioni. Serve per la cattura a freddo, dove ogni ciclo e'
un `rmmod` piu' un `insmod` del bersaglio, e il piano va rifatto perche' al
ricaricamento gli indirizzi cambiano.

L'ordine delle notifiche in 2.6.30 lo permette, ed e' lo stesso del 3.4 --
verificato su `kernel/module.c` dei due:

| | 2.6.30 | 3.4 |
| --- | --- | --- |
| `init_module()` | `load_module()`, poi `COMING`, poi `mod->init` | idem |
| testo scrivibile a `COMING` | sempre: `set_section_ro_nx` non esiste | si', ma stretto: gira subito DOPO `COMING` |
| `delete_module()` | `mod->exit()`, poi `GOING`, poi `free_module()` | idem |

Quindi a `COMING` le rilocazioni sono applicate e il driver non e' ancora
partito, e a `GOING` il testo e' ancora mappato: l'armamento cade in una
finestra buona da entrambe le parti, e su 2.6.30 non c'e' nemmeno la finestra
RO da rispettare.

Nessuno dei due tiene un riferimento sul bersaglio: `rmmod wl` deve poter
riuscire. La sicurezza viene dal disarmo al `GOING`.

## Parametri

| param | default | effetto |
|-------|---------|---------|
| `fifo_recs` | `131072` | record nella coda, 28 B ciascuno: 131072 sono 3.5 MB e ~25 s di margine. Solo variante 3.4 |
| `skipphyrd` | vuoto | letture di **registro PHY** da non registrare, es. `"0x253,0x254"` |
| `arm`   | `0` | `0` = dry-run (logga solo il piano hook); `1` = applica le patch |
| `target` | `wl` | nome del modulo da agganciare: gli hook si armano al suo `MODULE_STATE_COMING` e si disarmano al `GOING`. Serve anche a scartare i simboli che `kallsyms` risolve in un altro modulo |
| `delay` | `0` | `1` = aggancia anche `osl_delay` (rumoroso, usec inaffidabile) |

## Build

Fuori albero, contro il kernel 3.4 del device (stesso `.config`, stessi
`Module.symvers`, altrimenti vermagic/CRC non combaciano e `insmod` rifiuta):

```sh
make KDIR=/path/al/kernel-3.4-rt ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j
```

Copia `wl_diag.ko` sul device e `decode-wl-diag.py` sull'host di raccolta.

## Workflow di cattura (target 5 GHz, wl1 = `0x14e4:0x43b3`)

`wl_diag` si arma **da se'** sulla notifica `MODULE_STATE_COMING` del bersaglio
e si disarma su `MODULE_STATE_GOING`, quindi si carica una volta e si lascia
stare: `rmmod wl` e `insmod wl` in ciclo non lo riguardano, e non serve
ricaricarlo per far ri-risolvere gli indirizzi.

Verificato su `kernel/module.c` del tag `v3.4`, perche' tutto il meccanismo
dipende dall'ordine:

| momento | dove | cosa implica |
|---|---|---|
| `COMING` | `init_module`, dopo `load_module`, **prima** di `do_one_initcall(mod->init)` | il probe del driver e l'attach cadono sotto gli hook: niente remove/rescan PCI |
| | anche **prima** di `set_section_ro_nx` | la patch iniziale non dipende dal testo dei moduli scrivibile |
| `kallsyms` | `list_add_rcu` + `add_kallsyms` sono dentro `load_module` | al `COMING` i simboli del bersaglio si risolvono gia'; nel 3.4 `module_kallsyms_lookup_name` non filtra sullo stato del modulo (il filtro `MODULE_STATE_UNFORMED` e' del 3.9+) |
| `GOING` | `delete_module`, **dopo** `mod->exit()` e prima di `free_module()` | il detach viene tracciato, e il ripristino dei prologhi avviene con il testo ancora mappato |

Il disarmo al `GOING` ripristina le parole e poi fa `synchronize_sched()`: il
ripristino impedisce a una chiamata **nuova** di entrare in uno stub, la
sincronizzazione aspetta quelle **gia' dentro**. E' per questo che non serve
tenere un riferimento sul bersaglio -- e non si potrebbe, dato che `rmmod wl` e'
il passo centrale di una cattura a freddo.

Sequenza minima:

```sh
insmod /tmp/wl_diag.ko arm=1          # una volta, con o senza wl caricato
cat /proc/wl_diag | nc <HOST> 5555 &  # il lettore resta aperto per tutta la corsa
sh cold_capture.sh 20 36 40 44        # rmmod/insmod di wl a ogni canale
```

`target=<nome>` cambia il modulo agganciato, se non si chiama `wl`.

**Su 2.6.30 non e' verificato.** L'ordine delle notifiche in quel
`kernel/module.c` non e' stato controllato, e li' e' lo spazio utente del vendor
(hotplug o script `rc`) a fare `rmmod wl` al rescan del device: con l'armamento
dinamico quel `rmmod` non e' piu' un problema in linea di principio -- il
disarmo e il successivo riarmo sono automatici -- ma resta da provare sul campo.

### I confini fra i cicli: record MARK

Scrivere sul buffer inietta un record con l'etichetta scritta:

```sh
echo "ch36 bw20" > /proc/wl_diag
```

Dodici caratteri, impacchettati big-endian nei tre campi `u32` del record.
`wl_diag` ne emette due da se', `mod COMING` e `mod GOING`, ai bordi di ogni
caricamento del bersaglio. Il record entra in coda come tutti gli altri, quindi
e' **ordinato con le op che lo circondano** e non con l'orologio di chi scrive.

`reverse-tools/split_trace.py --on mark` taglia sui MARK e prende i nomi dalle
etichette. E' la differenza con `--on gaps`, che per uno sweep a
caldo deve tagliare sui salti temporali con soglia 1.03 s perche' i record
`CHANSPEC` arrivano in ritardo di un ciclo e uno solo per fase.

### Checkout su Windows: CRLF

`.gitattributes` forza `eol=lf`. Se il clone e' anteriore, gli script arrivano
sul router con CRLF e non partono: `set -u` seguito da CR e' un'opzione
illegale, e il CR riporta il cursore a colonna 0 mangiando l'inizio dei
messaggi. Si riconosce da errori come `: not foundn.sh: line 27:` invece di
`capture_plan.sh: line 27: X: not found`, o da un
`syntax error: unexpected end of file (expecting "done")` su uno script
corretto. Rimedio:

```sh
tr -d '\r' < capture_plan.sh > /tmp/cp.sh && sh /tmp/cp.sh 20a
```

### 0. Host: listener pronto PRIMA

TCP, non UDP: il `nc` di busybox sul router non ha `-u`, e con TCP non si
perdono record ne' si spezzano a meta'.

Linux/macOS -- `ncat` (pacchetto `nmap-ncat`); il `nc` di netcat-openbsd va
bene anche lui in TCP:

```sh
ncat -l 5555 | python3 decode-wl-diag.py | tee trace.txt
```

Windows: `ncat` esiste anche per Windows ed e' la via piu' semplice. Senza di
quello, un `TcpListener` in PowerShell che scrive un `.bin` grezzo -- la pipe di
PowerShell passa **oggetti**, non byte, quindi non ci si puo' infilare python
direttamente:

```powershell
$l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, 5555)
$l.Start(); $c = $l.AcceptTcpClient(); $ns = $c.GetStream()
$fs = [System.IO.File]::Create("$PWD\trace.bin")
$buf = New-Object byte[] 4096
try {
    while (($n = $ns.Read($buf, 0, $buf.Length)) -gt 0) {
        $fs.Write($buf, 0, $n); $fs.Flush()
    }
} finally { $fs.Close(); $c.Close(); $l.Stop() }
```

poi:

```powershell
python decode-wl-diag.py < trace.bin > trace.txt
```

### 1. Dry-run: verifica il piano hook

```sh
insmod wl_diag.ko                 # arm=0
dmesg | grep wl_diag              # "piano hook '<nome>' @..." per ognuno
rmmod wl_diag
```

Controlla che i simboli attesi risultino agganciabili (nessun "non trovato" /
"branch non rilocabile" / "stub fuori regione j 256MB" sui simboli che ti
servono — in particolare i nuovi `phy_reg_and/or`, `wlc_bmac_*`).

### 2. Porta giu' il device (PCI down)

Trova la funzione PCI di wl1 e rimuovila (su BCM63xx il bus e' 1 — adatta il
path al tuo SoC):

```sh
grep -il 14e4 /sys/bus/pci/devices/*/vendor        # individua il nodo
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove  # detach: wl.remove() gira
```

### 3. Arma il tracer (device ancora giu')

```sh
insmod wl_diag.ko arm=1           # + delay=1 se serve la temporizzazione
dmesg | tail                      # "wl_diag: ARMATO (N hook) -> /dev/wl_diag"
```

### 4. Niente da creare: il buffer e' in /proc/wl_diag

Appare al caricamento del modulo. Prima era un misc device a minor dinamico e
bisognava leggere il minor da `/proc/misc` e fare `mknod` a ogni `insmod`, con
il
rootfs spesso read-only e il nodo da mettere in `/tmp`.

`proc_create` ha la stessa firma su 2.6.30 e 3.4 e prende `file_operations`,
quindi la stessa chiamata vale per entrambi i kernel.

**Chiudere il lettore prima di `rmmod`.** Il `fops` ha `.owner = THIS_MODULE`,
quindi con un `cat` aperto lo scarico del modulo fallisce con `-EBUSY` invece di
lasciare puntatori penzolanti -- ma va comunque terminato, non forzato.


### 5. Avvia la pipe verso l'host (PRIMA del rescan, per non perdere record)

```sh
cat /proc/wl_diag | nc <HOST> 5555 &
```

### 6. Rescan PCI: il re-probe esegue l'attach sotto gli hook

```sh
echo 1 > /sys/bus/pci/rescan      # wl ri-probe 0x43b3 -> wlc_attach/init/up
```

Per la variante "down-to-bss-up" invece del rescan, con il device gia' su.
Attenzione: `wl up` da solo fa **solo attach** (equivalente al primo scan), NON
porta su la bss. Servono in sequenza il set-ssid e poi `bss up`:

```sh
wl -i wl1 down
wl -i wl1 up          # solo attach (come il primo scan): niente bss ancora
wl -i wl1 ssid <SSID> # configura l'SSID (senza portarla su)
wl -i wl1 bss up      # QUESTO porta su la bss (beacon/join)
```

### 7. Chiudi e decodifica

```sh
rmmod wl_diag                     # ripristina i prologhi + synchronize_sched
```

Sull'host, `Ctrl-C` su `nc`: `trace.txt` contiene la trace decodificata. Se
compaiono righe `** DROP ** persi=N`, la FIFO kernel (32768 record) e' andata in
overrun: reader troppo lento o burst troppo denso.

## Note

- **Sacrificale.** `arm=1` scrive nella memoria del modulo (RWX) e fa
  `flush_icache_range`; assunzione MIPS32R1 da confermare sul device.
- **Read = UNDEFINED.** Gli hook catturano solo gli argomenti d'ingresso: il
  valore restituito dalle read non c'e' (mai `0x0000` inventato). Per l'and/or
  PHY questo non e' un problema: l'operando e' `a2`, catturato, e il decoder
  rende la maschera effettiva (`clr ~val` / `set val`).
- **Ordine.** Listener su, poi arma a device giu', poi pipe, poi rescan. Armare
  a device gia' su perde l'inizio dell'attach.
- **Consumatore a valle.** `decode-wl-diag.py` (stream 28 B).
