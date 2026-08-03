# TODO di validazione — divergenze note del bring-up

La localizzazione per-funzione ora non usa più i fingerprint indovinati dai
sorgenti: l'harness marca ogni funzione con `B43_AC_FN()` (attivo con
`AC_FN_MARKERS=1`), quindi `localize_functions.py <generated> <trace>` e
`coverage_by_function.py` hanno confini esatti. La copertura si misura contro
la cattura **grezza** (come `compare.py`), non contro il collassato.

## Stato copertura bring-up (rfkill + op_init), per sequenza

| funzione | d6220 | DSL | agcombo |
|---|---|---|---|
| radio_2069_init | 100% | 100% | 100% |
| r2069_prefregs_init | 100% | ~92% | 100% |
| radio_2069_pwron | 100% | 100% | 100% |
| radio_2069_rccal (+setup/run) | 100% | 84% | 84% |
| radio_2069_afe_lpf_stage | 100% | ~4% | 100% |
| op_init sub non-tabella (set_pdet, pre_init, mode_init) | 100% | 100% | 100% |

d6220 e agcombo: bring-up radio coperto integralmente, gap 0%. Le divergenze
sono tutte sul DSL (wl 6.30) e sono i punti aperti sotto.

## Punti aperti

- **prefregs_init sul DSL — due scritture in meno.** Il DSL salta `RAD 0x065e`
  (e una seconda scrittura poco dopo); il resto della sequenza è identico, solo
  shiftato. Capire in quale ramo il port scrive `0x065e` incondizionatamente e
  se va reso condizionale a chip/versione.
- **afe_lpf_stage sul DSL.** Il port scrive valori tarati sul d6220
  (`PHY.MOD 0x0728 = 0x0800`, `RAD 0x0045 = 0x3080`) che il vendor DSL non
  emette (usa `0x0045 = 0x703f`, mai `0x0728 = 0x0800`). La wl 6.30 struttura la
  fase AFE-LPF diversamente. Da isolare dove il DSL imposta l'AFE dopo rccal.
- **rccal 84% su DSL e agcombo.** ~6 op su 38 divergono; probabile misura
  analogica per-board (E/F/G) o dettaglio di versione. Da confermare.
- **init_regs sul DSL — costanti di versione.** La coppia `(hi, lo)` dei
  registri di gain ADC è selezionata dalla **fase**, non dal chip: attach da
  freddo `0x097a/0x08fa`, down→up `0x03bf/0x0340`, e `adc_reset` riscrive poi
  `0x03ac/0x032c`. Vale su entrambi i chip (d6220 `#4479` e agcombo `#4071`
  per la prima, d6220 `#51731` e agcombo `#58369` per la seconda). Il DSL con
  wl 6.30 scrive invece `0x0395/0x0315` sul down→up e non emette `0x1645`:
  quella resta una differenza di versione, e su DSL `init_regs` misura 0/17.
  Il conteggio delle passate è invece per chip: due sul 4360, una su entrambi
  i testimoni 4352.
- **0x02e4 sul DSL — costante di versione.** Il campo a 6 bit di `0x02e4`
  (`mask=0x3f00`) viene scritto nella fase iniziale su tutte e tre le board, ma
  con valore diverso: `0x0f00` sull'attach da freddo di agcombo (`#27`) e d6220
  (`#81`), `0x0800` sul down→up del DSL (`#120986`). Sul down→up del d6220 non
  viene scritto affatto. Il port emette `0x0f00` gatato su `do_full_init`; la
  variante 6.30 non e' riprodotta. Da non confondere con lo `0x0800` che
  `switch_channel` scrive sul 4360 (`agcombo #5810`, `#60373`): quello cade dopo
  `init_regs` e nessun testimone 4352 lo emette, quindi il gate
  `chip_id == 0x4360` in `switch_channel` e' corretto.
- **PLLCTL2/PLLCTL3 sul DSL — funzionalita' assente nel driver stock.** Sul
  cold attach il d6220 (4352) scrive `PLLCTL2=0x0c31` e `PLLCTL3=0x100e`
  (`#15`, `#23`) e li rilegge piu' avanti (`#164`, `#172`), identico
  all'agcombo; sul down→up compaiono solo le read, ed e' da li' che nasceva la
  tesi "il 4352 non scrive", ora corretta nella 0007. Il DSL, pure 4352 con
  `chiprev 0x03` e `package 0x01` identici, lascia `PLLCTL3` al valore ROM
  `0x00133333`: il suo driver stock non ha il supporto vcofreq frazionario
  (verificato con `strings`), mentre i driver stock piu' recenti impostano
  vcofreq. Quella board funziona cosi', quindi scriverci i valori nuovi e' non
  testato, non un fix.

  Metodo riusabile: per le divergenze DSL qui sotto, confrontare la presenza
  dei simboli fra i due blob con `strings`/`readelf -s` prima di ipotizzare
  differenze di board — "funzione assente nella versione vecchia" e' un
  pattern gia' accertato una volta.
- **Max index TX su 0x0646/0x0846 -- gate applicato, resta il DSL.** Al primo
  bring-up tutte le board scrivono la costante `0x38`; su un channel setup
  successivo il valore e' `maxp5ga[grp] - 6`, cioe' `0x42` sul d6220
  (maxp5ga0=72) e `0x44` su agcombo (74). Il port ora gata sulla fase e combacia
  su entrambi i gate d6220. **Resta aperto**: il DSL emette `0x38` anche sul
  down→up, quindi o non fa la derivazione o la fa da altri campi.
- **La cattura `d6220/wl-diag-wl1-down-to-bss-up_delay_only.txt` sottoconta:
inaffidabile
  per i conteggi.** E' una finestra da `#50388` (36228 episodi, zero RETVAL) e
  mostra **una** passata in `init_regs` e **un** round in `adc_reset`, dove la
  `down-to-bss-ch36-bw20` -- completa da `#1`, 5152 RETVAL -- ne mostra due di
  ciascuno. Non e' troncata all'inizio: il prologo prima di `init_regs` e' lungo
  uguale (439 op contro 433). Ma e' anomala su **due conteggi indipendenti,
  entrambi verso il basso**, mentre usa i valori della fase successiva
  (`0x03bf`, non `0x097a`) -- cioe' si comporta da primo bring-up nei conteggi e
  da successivo nei valori. Due anomalie correlate indicano op mancanti, non un
  comportamento hardware diverso.

  Dove **non** sottoconta: il flow `rfkill` misura 100% con zero gap su entrambe
  le catture, funzione per funzione. Quindi il gate `rfkill` che gira contro la
  vecchia resta valido; sono i blocchi di `op_init` a essere sospetti.

  Regola operativa: per i conteggi usare la `down-to-bss-ch36-bw20`. La vecchia
  resta utile per gli indirizzi e come secondo testimone qualitativo.

- **0x0910-0x0913 -- riprodotto per ch36 BW20, meccanismo identificato.** Il
  banco porta un **offset per catena** su una soglia assoluta presa da un ladder
  discreto (`.rodata +0x040e84` nel blob D6220, tre righe da 15 entry u8,
  attribuito a `wlc_phy_crs_min_pwr_cal_acphy`). La relazione e'
  `CRS = ladder[indice] - offset`: il CRS porta la parte comune agli 8 registri,
  il banco la correzione per catena. Verificato 4/4 sul d6220, e risolve
  l'anomalia del `CRS = 49`, che nel ladder non c'e' mentre `49 + 3 = 52` si'.

  Il port implementa `off = max(0, target - crs)`, che ha la **causalita' al
  contrario** e riproduce i numeri solo perche' entrambi i termini sono fittati
  sulla configurazione validata. Resta aperto: la regola dell'indice, la
  trasformazione che genera l'offset per catena, e la verifica agcombo (le sue
  somme non sono nel ladder del d6220, ma e' un chip diverso). Derivazione,
  tabelle e controesempi in
  [`bank-0910-analysis.md`](bank-0910-analysis.md). Nota che il nome
  "noise floor" non e' fondato. Viene dal nome della
  funzione nel port e non compare in `brcmsmac`, in nessun header, ne' in
  nessuna fonte. Cio' che e' accertato del blocco e' solo strutturale: il driver
  stock **non lo legge mai** (write-only in entrambe le catture con retval),
  quattro registri per catena con stride `0x200` -- ma **mai** per la catena 0,
  che in 12 catture non compare: i banchi sono `catene - 1`. Lo stesso scalare
  in entrambi i byte, e un ordine asimmetrico -- pari hi-poi-lo, dispari
  lo-poi-hi -- che
  suggerisce due quantita' a 32 bit scritte a byte piuttosto che quattro
  registri indipendenti.

  Il nome ha condizionato l'indagine: si e' cercata una *misura* da cui derivare
  il valore, guardando le letture vicine. Ma essendo write-only e senza letture
  vicine che lo spieghino, il valore viene plausibilmente da **stato lato
  driver** -- un contatore o un indice -- non da un registro. Coerente con la
  crescita dentro la sessione e col fatto che l'agcombo (3 core) arrivi piu' in
  alto: banco core-2 con valori 0, 6, 9, 14, 15, 18, 25.

- **Valore del blocco: cresce nella sessione, non costante.**
  `b43_phy_ac_prog_bank_0910` scrive uno scalare fisso su tutte e otto le
  posizioni -- `0` sul 4352, `0x0f` sul 4360 -- e l'ordine delle op e' giusto
  (`0x910` hi/lo, `0x912` hi/lo, `0x911` lo/hi, `0x913` lo/hi). Il valore no:
  cresce a ogni invocazione dentro la stessa sessione.

      DSL down→up            0, 0, 0
      d6220 attach           0, 0x05
      d6220 down→up          0x03, 0x05
      agcombo attach         0, 0x06, 0x09
      agcombo down→bss       0x0f, 0x12

  Le costanti attuali riproducono la **prima** occorrenza di alcune catture
  (`0` per d6220 attach, `0x0f` per agcombo down→bss) e sbagliano le successive.
  Gli incrementi non sono uniformi (+5, +2, +6/+3, +3) e nessuna lettura nelle
  100 op precedenti determina il valore, quindi dipende da storia di sessione
  che il driver a quel punto non ha. Da riprendere se salta fuori una cattura
  che copra due invocazioni con le letture in mezzo.

  Nota anche che il nome della funzione descrive cio' che fa il port (azzerare),
  non cio' che fa il driver stock (programmare).

  Contesto della prima occorrenza (`down-to-bss-ch36-bw20`), per chi riprende:

      #3102        RD 0x0601 = 0x49, riscritto su 0x1601
      #3111-3133   tabella 7, due RMW: off 0x3cd e 0x3dd, 0x0c02 letto,
                   0x04c2 e 0x04e2 scritti
      #3135-3140   prefregs 0x0371-0x0376
      #3143-3146   gain 0x031c-0x031f = 0x00bf
      #3147-3154   CRS, otto registri a 0x31
      #3155-3162   noise floor, otto op a 0x03      <-- divergenza
      #3163        peek 0x03a9

  Ipotesi **respinta**: `0x0c02 >> 10 = 3` combacia con la prima occorrenza, ma
  in tutta la cattura non esiste una lettura di tabella 7 con `>>10 = 5`.
  Coincidenza su un valore piccolo.

  Ipotesi `b // 10` **respinta**: correlava su 10 punti di 13, con tre
  controesempi da entrambi i lati -- `ch36 #52556` e `DSL #155080` scrivono zero
  con `b` disponibile (55 e 48, scritti centinaia di episodi prima), `d2u #3155`
  scrive 3 con tutti i coefficienti a zero. La correlazione era temporale: banco
  e coefficienti partono da zero e crescono insieme.

  Altri candidati esclusi provando contro le catture: il base index idle-TSSI,
  ogni coppia di registri per-core `0x06xx`/`0x08xx` letta o scritta prima del
  banco, i campi SROM per-core (identici su tutte e tre le board), e
  `0x073d + core*0x200` che legge zero in tutte e quattro. Vedi §8 di
  `rxiq-cal-analysis.md`.

  Il valore resta non spiegato. Le costanti attuali (`0` sul 4352, `0xf` sul
  4360) riproducono la prima occorrenza di alcune catture e sbagliano le altre.

- **Coefficienti RXIQ cablati, solve mai invocato.** `rx_iq_comp_update` ha la
  matematica verificata bit-exact ma zero invocazioni nel flow: il port scrive
  `coeffs[2][2] = {{0x03f2,0x004c},{0x03db,0x0037}}` in `phy_ac.c`, che non
  combaciano con la cattura di attach (`0x03ef`, `0x004d`, `0x03d4`, `0x003b`).
  Vicini ma diversi: i coefficienti sono misurati e variano per sessione.

  **Il solve e' pronto e verificato.** Eseguito a mano sui valori che l'oracolo
  serve nella cattura di attach, sommando i due round di stima, da' `0x3ef
  0x04d`
  e `0x3d4 0x03b` -- bit-exact su entrambi i core contro il driver stock. Gli
  accumulatori sono gia' letti da `iqcal_meas_post_dds_apply_v2` subito prima di
  `rxiq_apply_coefficients`, dove stanno le costanti. Collegarlo e' a rischio
  zero sul gate e rende la fase funzionante su qualunque sessione. Vedi §9 di
  `rxiq-cal-analysis.md`.
- **set_pdet_on_reset e pre_init_frontend sul DSL.** `not found` e 2/13. Non
  ancora isolate.
- **tables_init sul DSL — nessun oracolo.** L'ordine del load tabelle è
  allineato op-per-op al vendor (3714/3714) ma **solo su agcombo**: le due
  catture agcombo sono le sole in repo che contengano il load, e nessuna
  cattura 4352 lo contiene (`TABLE_ID` non prende mai `0x0001`). Serve una
  cattura DSL con l'attach tabelle per validare l'ordine sul chip di target.
- **coefficiente RX-LPF (221/222, 215/216).** Fitta due campioni (d6220,
  agcombo); serve un terzo `lpf_cap0` (altro canale 5G) per disambiguare —
  vedi `txlpf-formula.md`.
- **accessor vendor.** Verificare col `/proc/kallsyms` del DSL riflashato che
  `hooks[]` copra tutti gli accessor: cercare `phy_reg_write_list`,
  `wlc_phy_write_regs*`, varianti `write_radio_reg_*`. Se esistono e non sono
  hookati, le catture sono cieche su quelle op.

  **Confine.** Si intercettano gli accessor di **I/O hardware**, non la logica
  interna del driver: la traccia deve restare un'osservazione del dispositivo.
  Strumentare funzioni interne farebbe cadere quella distinzione. Leggere
  `.rodata`/`.data` dai blob e' invece lettura di dati, ammissibile.

- **indice del ladder crs_min_pwr.** Manca la regola che scegli l'indice, e
  **non va cercata hookando la funzione di calibrazione** (vedi il confine
  sopra). Strade praticabili: verificare se la misura passa da un accessor di
  I/O non ancora coperto; interrogare il driver stock via iovar se
  `phy_force_crsmin` esiste; leggere il ladder dal blob agcombo per chiudere la
  verifica 4360. Se non basta, resta aperta. Vedi
  [`bank-0910-analysis.md`](bank-0910-analysis.md) §5.4-5.6.

## Prima mappatura su agcombo (4360)

Cattura `agcombo-wl1-4360-rescan-to-bss-ch36` (6453 RETVAL, la sola agcombo con
i valori letti), flow `full`, finestra da `#47` -- l'analogo del `#50` del
d6220, trovato agganciando le prime op del port. **77.40% di op in comune su
30322, prima divergenza a @45.**

Il numero **non e' confrontabile** col 99.94% del d6220: quello misura una
configurazione validata op-per-op, questo la prima esposizione a una board
nuova, dove di 1615 regioni solo la prima e' interpretabile perche' il resto e'
a valle di un disallineamento.

### Reperti

- **`PHY.WR 0x01ec = 0x2`, solo 4360.** Emessa dopo la `MOD 0x02e4` in ciascuna
  delle due entrate nell'analogico. Zero occorrenze nella cattura d6220, dove il
  resto del preambolo combacia op-per-op: quindi chip, non fase. Implementata
  con gate su `chip_id`. Cosa sia `0x01ec` non e' stabilito -- il port lo scrive
  altrove con `0x9c40` in un punto che combacia su entrambe le board.

- **Doppia programmazione dell'analogico, solo 4360.** Il driver stock esegue
  l'unita' AFE due volte durante l'attach: unita' a `@17, @37, @71, @91` contro
  le `@17, @36` del d6220. **Tutte e quattro scrivono il banco `AFE_ON`** -- non
  c'e' spegnimento in mezzo, sono due accensioni (`AFE_DOWN` scriverebbe
  `0x1728=0x80`, `0x1720=0x180`, `0x1721=0x5000`, che non compare mai).

  Fra le due coppie il PLL viene **riscritto con gli stessi valori**
  (`PMU.PLL 0xc31/0x100e` a `#123`/`#131`), quindi se e' la causa lo e' l'atto
  -- un re-lock -- e non un cambio di frequenza. Il d6220 non ha ne' la seconda
  scrittura ne' la rientrata. Rescan e attach sono la stessa fase, quindi la
  fase e' esclusa.

  Implementata con gate su chip. Le op di bus fra le due entrate
  (`SI.COREREG`, `PMU.PLL`, `MAC.MCTRL`) **non** sono riprodotte: appartengono a
  bcma e a b43 core. Restano 13 op di sfasamento fra le due coppie per questo.

  `TODO(verificare su una board nuova)`: l'evidenza e' un solo 4360. Serve un
  secondo esemplare per separare "proprieta' del chip" da "proprieta' di questa
  board", e per stabilire se la rientrata segua la riscrittura del PLL o se
  entrambe seguano un terzo fattore.

- **`rxcore_setstate` era chiamata senza condizione.** Serve a spegnere i core
  che il silicio ha e la board non collega, e il commento lo diceva -- ma la
  chiamata era incondizionata: si leggeva il conteggio del silicio da `0x000b`
  e non lo si usava. Su una board che collega tutte le catene emetteva **31 op**
  che il driver stock non emette. Ora e' gatata su
  `hweight8(coremask) != num_cores`, con `num_cores` da `RD 0x000b & 0x07` (3 su
  entrambi i chip) e `coremask` da `rxchain` SROM (7 agcombo, 3 d6220).
  Verificato: `WR 0x16d8` emessa 0 volte su agcombo e 2 su d6220, come nelle
  rispettive catture.

- **`MOD 0x02e4 = 0x800` era nel posto sbagliato.** Implementata e gatata bene
  sul 4360, ma emessa dopo `channel_setup` invece che dentro la sequenza: @6893
  nel port contro @6250 nel vendor. Spostata fra il relock del gate e la
  `WR 0x01ec = 0x9c40`. Ora il vicinato locale combacia op-per-op per 11 op
  consecutive, e lo scarto assoluto residuo (13) e' esattamente il buco delle op
  di bus fra le due entrate nell'analogico, che non sono riprodotte.

- **132 indirizzi su 294 mai letti dal port**, 8 code dell'oracolo esaurite. Fra
  questi `0x0bb1`/`0x0bb3` (terza catena) letti 33 volte dal vendor e zero dal
  port. Non ancora guardato: e' il reperto strutturale grosso che resta.

- **La curva di gain della tabella `0x20` e' davvero invariante.** 128 entry su
  128 identiche fra d6220 e agcombo. Il commento nel port lo dava per assunto ed
  e' la prima conferma indipendente: non tutti i valori cablati sono
  board-specific.

### Decorrelazione su 33 segmenti multicanale

Materiale: 4 fasi di `reverse-tools/capture_plan.sh` sul **DSL-3580L**
(driver 6.30.102.7), 33 segmenti down->up su canali 36-140 a 20/40/80 MHz.
Strumento: `reverse-tools/decorrelate_channels.py`.

**Attenzione alla provenienza**: il port e' modellato su d6220 / 7.14.89, queste
catture sono DSL / 6.30. La **struttura** delle dipendenze e' fisica del chip e
si trasferisce; i **valori** no. Quindi da qui si ricava dove il codice ha
bisogno di una funzione invece di una costante, e i numeri si prendono dal blob
del d6220.

### Il risultato

    1581 chiavi (op, addr, mask) distinte
       invariante      1311   83%
       stesso-insieme    49    3%
       solo-canale      13    0.8%
       solo-larghezza   78    5%
       centro-freq     130    8%
       dinamico          0

**Nessun valore dinamico.** Con due segmenti alla stessa etichetta
(ch40/bw20 compare in due fasi) e i troncati esclusi, non c'e' una sola chiave
che differisca fra due esecuzioni dello stesso (canale, larghezza). Tutto e'
funzione deterministica dei due, quindi tabellabile: il lavoro diventa trovare
le tabelle nel blob, non dedurre formule.

**`solo-canale` e' quasi vuoto: la variabile vera e' la frequenza centrale.**
Delle 13 chiavi, dieci sono artefatti -- il banco AFE (`0x1720`, `0x1721`,
`0x1725`, `0x1728`, `0x173a`) mostra `[0x0180]` contro `[0x0180, 0x03ff]`, cioe'
segmenti dove il bring-up si ferma dopo lo spegnimento e segmenti dove arriva
all'accensione. Le dipendenze reali dal solo canale sono **`PHY.WR 0x0727` e
`0x0927`** (per-core, stride 0x200) con valore binario 0 oppure 4.

Questo conferma in modo indipendente la nota del vecchio
`decorrelate_channel_bw.py`, che su 4 sole trace d6220 diceva "la variabile
indipendente reale e' la freq centrale, per questo solo-canale esce vuoto".

### Le dipendenze dalla larghezza si calcolano, non si tabellano

78 chiavi, e i pattern sono regolari:

| chiave | 20 MHz | 40 MHz | 80 MHz | forma |
|---|---|---|---|---|
| `PHY.MOD 0x?ef` mask `00ff` | 0x0f | 0x1e | 0x3c | raddoppio |
| `PHY.WR 0x0262` | 200 | 400 | 800 | raddoppio |
| `PHY.WR 0x0263` | 25 | 50 | 100 | raddoppio |
| `PHY.WR 0x0250` | 0x2c19 | 0x2c32 | 0x2c64 | byte basso 25/50/100 |
| `PHY.WR 0x025b` | 8 | 16 | 31 | raddoppio (31, non 32) |
| `OBJ.WR 0x0018`, `0x001a` | 0x149 | 0x14a | 0x14b | +1 per passo |
| `OBJ.WR 0x004a` | 0x191 | 0x192 | 0x193 | +1 |
| `OBJ.WR 0x0944` e famiglia | 0x32ab | 0x32cb | 0x32eb | +0x20 |

I raddoppi sono conteggi di campioni o durate che scalano con la banda: quelli
il codice li calcola. Gli incrementi di 1 e di 0x20 sono indici in tabelle, e
quelli vanno risolti nel blob.

`0x025b` fa 8/16/**31**: se fosse un raddoppio puro sarebbe 32, quindi e' un
valore saturato o un massimo-meno-uno. Da guardare.

### Etichettare i segmenti: il chanspec sta in shared memory

`OBJ.WR 0x00a0` porta il chanspec, ed e' l'etichetta esatta di ogni segmento --
non serve fidarsi dell'ordine, che si sfasa quando la cattura perde record. E'
cosi' che si e' scoperto che la fase 20b era **sfasata di uno**: il primo
segmento e' la coda della fase precedente.

Due avvertenze sul valore: per 40 e 80 MHz porta il canale **centrale** (5g36/40
diventa `ch=38`), e a fine segmento puo' comparire il chanspec del ciclo
successivo, quindi si prende la **prima** scrittura.

### Cosa NON usare

I record `CHANSPEC` (da `wlc_phy_chanspec_set`) non servono come confine: ne
arriva uno per fase o zero, e sono in ritardo di un ciclo. Per marcare i confini
servirebbe agganciare anche `wlc_phy_chanspec_set_acphy`.

I segmenti troncati falsificano l'analisi: confrontando un segmento da 713
chiavi con uno da 1554 escono 85 false dipendenze "dinamiche". `--minchiavi`
li scarta.

### Cosa si e' potuto chiudere, e cosa no

Incrociando le chiavi dipendenti con gli indirizzi cablati in `phy_ac.c`:

    solo-larghezza   78 chiavi  ->  25 cablate nel codice
    centro-freq     130 chiavi  ->  35 cablate
    solo-canale      13 chiavi  ->   9 cablate

Le funzioni piu' interessate: `idle_tssi_meas` (8 + 15 + 2),
`coeff_bank_init_bw20_5g` (4), `chanspec_tail` (4), `set_channel` (8, che sono i
registri CRS), `post_noise_shaping_rx_regprog_core` (6).

**Chiuso: tre write parametrizzate per banda** in `coeff_bank_init_bw20_5g`.
`0x0250` (byte basso 25/50/100), `0x0262` (200/400/800), `0x0263` (25/50/100).
Il valore a 20 MHz del port **coincide** con quello del DSL su tutte tre, quindi
si trasferisce la scala e non un valore di un'altra board. I gate d6220 non si
muovono: 99.94% e 99.97%.

**Non chiuso, e perche':**

- `0x06ef` mask `0x00ff`: il DSL fa 0x0f/0x1e/0x3c, ma il port ha **tre** write
  a quell'indirizzo (`0x0017`, `0x0e00`, `0x000f`) e non e' distinguibile quale
  corrisponda. Serve il confronto sul d6220, non sul DSL.
- `0x025b`: il DSL scrive 8/16/31 -- stessa scala ma **saturata**, 31 e non 32
--
  e il port non lo scrive affatto. Da capire se il d6220 lo scriva: potrebbe
  mancare una write.
- tutto il resto: i valori vengono dal DSL/6.30 e il port modella d6220/7.14.89.
  La struttura si trasferisce, i numeri no.

### TODO

- risolvere nel blob d6220 le tabelle indicizzate dalle chiavi `centro-freq`
  (130) e dalle `solo-larghezza` con incremento di indice;
- capire `PHY.WR 0x0727`/`0x0927`, la sola dipendenza dal canale puro;
- capire perche' `0x025b` satura a 31;
- una cattura d6220 multicanale, per avere i valori sulla board che il port
  modella invece di trasferirli dal DSL.

## Analisi quantitativa del driver proprietario

Quattro blob disponibili, tutti MIPS32 BE non strippati. Serve a rispondere a
una
domanda operativa: quando una differenza fra due catture e' del chip e quando e'
della versione vendor che le ha prodotte.

| device | versione wl | SDK | radio |
|---|---|---|---|
| D6220 | 7.14.89.14 | `cpe4.16L03.0-kdb` | 4352 |
| Archer VR400 | 7.14.89.3303 | `cpe4.16L03.0u1` | 4352 |
| agcombo (AGSOT 1.0.8) | 7.14.43.21 | `cpe4.16L02A.0-kdb` | 4360 |
| D6400 | 6.37.15.24 | `cpe4.16L01A.0-kdb` | non verificata |

#### La versione identifica un rilascio Broadcom, non l'OEM

D6220 (Netgear) e VR400 (TP-Link) condividono `7.14.89` e la stessa release SDK.
Sulle 101 funzioni `acphy` comuni **di pari dimensione**, su 30024 parole di
codice: 1633 parole differiscono, di cui **1630 sono rilocazioni** (in un file
REL l'addendo sta dentro l'istruzione, quindi cambia con il layout) e **3 sono
costanti vere** -- due in `force_spurmode_acphy`, una in `txctl1_calc_ex`.

Cosa autorizza a concludere: per le funzioni di pari dimensione, i blocchi di
byte hanno **la stessa forma di istruzioni**, e le costanti immediate coincidono
tranne tre. Non autorizza a concludere che il codice AC sia identico in
generale: il confronto vale solo dove la dimensione combacia, e non dice nulla
sulle funzioni che differiscono in dimensione.

**Conseguenza usabile**: ladder e tabelle estratte da un blob sono affidabili
**per versione**. Il ladder `crs_min_pwr` a `.rodata +0x040e84`, gli offset e le
maschere sono proprieta' del rilascio, non dell'esemplare, e si possono usare
per
un'altra board con la stessa versione.

#### I conteggi dei simboli non sono confrontabili fra artefatti diversi

`readelf` conta le funzioni `acphy` cosi':

    D6220        204        <- .o_save
    VR400        102        <- .ko
    agcombo      102        <- .ko
    D6400        162        <- .o_save

La differenza non e' di funzionalita': **`.o_save` e' l'oggetto prima del link
finale**, e conserva funzioni che il link poi scarta; le `.ko` sono il modulo
finale. Confrontare un `.o` con un `.ko` sul numero di simboli non misura
niente.

La prova sta nelle due `.ko`: VR400 (`7.14.89.3303`) e agcombo (`7.14.43.21`),
versioni **diverse**, hanno insiemi di funzioni `acphy` **identici** -- jaccard
1.000. Quindi l'insieme dipende da come la build e' configurata, non dalla
versione.

Nota minore sui nomi: parte delle "funzioni esclusive" di una build sono
suffissi
`.isra.N` / `.constprop.N` che GCC assegna in ordine di elaborazione. Fra
D6220 e
D6400, 10 delle 14 esclusive erano solo questo.

#### Correlazione cattura / versione

    0x70e590e = 7.14.89.14   d6220     (4 catture)
    0x70e2b15 = 7.14.43.21   agcombo   (2 catture)
    0x61e6607 = 6.30.102.7   DSL       (1 cattura)

**Va messa accanto a ogni reperto.** Una differenza fra catture di versioni
diverse non e' un reperto sul chip finche' non e' confermata a versione pari.
Vale per la doppia entrata nell'analogico e per tutto il resto.

Esempio concreto di quanto serva: `radar_detect_on_off_cfg_acphy` e' 144 byte in
7.14.43, con due scritture allo stesso registro (`0x01ec <- 0x0002` poi
`<- 0x9c40`), e 52 byte in 7.14.89, dove la prima non c'e' piu'. Nelle catture:
`0x9c40` compare una volta in ogni cattura di ogni board, `0x0002` solo nei due
attach dell'agcombo. Il port modella 7.14.89 e quindi non deve emetterla -- non
e' un gate sul chip.

#### Strumenti

`reverse-tools/similarity.py` calcola la matrice fra build.
`reverse-tools/cmp_funcs.py` confronta i corpi a due livelli: byte identici, e
identici a meno delle costanti (immediato a 16 bit e target a 26 azzerati, cosi'
le rilocazioni non contano).


### Bug corretto: il check su PLLCTL3 bloccava il d6220

`op_init` confrontava `PLLCTL3` con `0x00133333` sul 4352 e tornava `-ENODEV` se
non combaciava, sulla base di "il 4352 arriva con quel valore e il vendor non lo
tocca". **Falso**: quella e' una proprieta' del **DSL**, le cui catture
contengono solo letture del PLL. Il d6220 scrive `0xc31`/`0x100e` a `#15`/`#23`
come il 4360, quindi su un d6220 reale quel ramo avrebbe **rifiutato il
bring-up**. Ora legge e logga, senza decidere: non c'e' una condizione fondata
per rifiutare, e inventarne una blocca hardware buono.

### Nota di metodo: tre misure inquinate prima di quella buona

Vale la pena registrarlo perche' l'errore era sempre nell'impostazione, non nel
porting.

1. Mappata la rescan contro `switch_channel`. Sbagliato: la rescan e' un flow
   completo, va contro `full`.
2. Letto il commento del profilo DSL credendolo dell'agcombo, concludendo che il
   profilo dichiarasse due catene invece di tre.
3. Il profilo agcombo era senza le word FEM, quindi `femctrl` leggeva 0 e **il
   guard aggiunto in questa serie** saltava la tabella di controllo FEM in
   silenzio -- il warning va su stderr e non era stato guardato. Prima
   divergenza a @230 invece di @1970.

Tre volte lo strumento diceva "porting incompleto" e la causa era la misura.
Prima di leggere una percentuale su una board nuova: verificare il flow, la
finestra, il profilo, e **stderr**.

## Stato dei due flussi principali

Misurato con un diff LCS, non col conteggio per indice di `compare.py` (che dopo
una insert/delete conta divergente tutto il resto e sovrastima: sul flow 1
riporta 1017 dove le op divergenti sono 15).

| | op in comune | divergenti |
|---|---|---|
| `full` / attach ch36 BW20 | 24998/25013 = **99.94%** | 15, di cui 10 artefatti di scheduling |
| `switch_channel` / down→bss ch36 BW20 | 21000/21007 = **99.97%** | 7, lunghezza identica |

### Chiuso in questo giro

- **gain LUT `0x42`/`0x62`/`0x82`** (384 op sul flow 2). I default di core 0 e
  core 1 valgono `0x0002` e `0x0200` e **non dipendono dalla fase**: sono gli
  stessi nelle due catture. Solo il core 2 cambia (`0x0962`/`0x0758` al primo
  bring-up, `0xabd0`/`0xa9c6` sullo switch successivo). Il port aveva i due rami
  invertiti su c0/c1 e valori sbagliati su c2.
- **`TBL 0x0c` off `0x62` e `0x66`** (4 op). Stesse costanti sbagliate
  (`0xfe02`, `0x0100`) in un secondo punto: valgono `0x0002` e `0x0200`.
- **Max index TX** (2 op), vedi sopra.

### Aperto sui due flussi

- **Il filtro IQ dipendente dalla frequenza non va derivato: lo calcola
  l'hardware.** L'equivalente di `wlc_phy_cal_rx_fdiqi_acphy` nel port e'
  `rxiqcal_run_meas_iters`, due iterazioni per catena (cmd `0x?084` poi
  `0x?056`) = i due tap di un filtro complesso, coerente con i tre toni di
  `dds_seed`/`_second_tone`/`_third_tone`. `rxcal_afe_iter` arma la cal
  scrivendo `cmd` su `0x0380`, attende il bit 15, legge 2 celle a `rd_off` e le
  **riscrive identiche** a `wr_off`. La copia si vede in chiaro nella cattura:
  `RD 0x0c[0x8e]/[0x8f] -> WR 0x0c[0x50]/[0x51]`, `RD [0x91] -> WR [0x53]`.

  Conseguenze, applicate: il `TODO: derivare i valori TBL.WR dalla stima RXIQ
  runtime` era **stantio** ed e' rimosso, e il campo `wr_vals[2]` della tabella
  `iters[]` era **codice morto** -- dichiarato, inizializzato con i valori
  trascritti, mai letto -- ed e' rimosso.

- **`TBL 0x0c` off `0x60`/`0x64` dal percorso AFE** (3 op flow 1, 2 flow 2). Il
  port scrive `afe_res[0].v` e `afe_res[2].v`, che portano `0x64`/`0x22`/`0x04`
  dove il vendor ha `0x66`/`0x27`/`0x03` (attach) e `0x69`/`0x26` (down→up). I
  valori dipendono dalla fase. **Non** derivano dai risultati della cal AFE:
  quelli finiscono su `0x50`-`0x55`, non su `0x60`/`0x64`.

  La storia completa delle celle nella cattura di attach mostra due percorsi
  distinti:

      #17381  WR [0x60]=0x0066  [0x61]=0x000e     blocco fresco
      #17415  WR [0x64]=0x0027  [0x65]=0x0003
      #25692  RD [0x60]=0x0066  [0x61]=0x000e     RMW con modifica reale
      #25703  WR [0x60]=0x0065  [0x61]=0xfffd
      #25712  RD [0x64]=0x0027  [0x65]=0x0003
      #25723  WR [0x64]=0x0027  [0x65]=0x0004

  Il sito nel port non e' **nessuno dei due**: provate entrambe le ipotesi --
  riscrivere il valore letto, e scrivere lo stesso blocco del sito gemello in
  `rxiqcal_finalize` -- e **entrambe peggiorano** (flow 1 da 15 a 18 op, flow 2
  da 7 a 11). Reverted. E' una terza forma non identificata.

  **SALAME**: nel RMW la seconda cella va da `0x000e` a `0xfffd`, cioe' da 14 a
  -3, delta **-17** -- esattamente il coefficiente `a` della catena 0. Due punti
  soli, quindi indizio e non altro, ma se regge il RMW somma il coefficiente
  RXIQ alla cella.
- **`0x06a1`** (2 op flow 2) -- **spiegato: stato pre-cattura, non un difetto.**
  Il solve da' `0x4b` dove il vendor scrive `0x4c`. Misurato strumentando il
  solve sul down->up:

      core 0: ii=80526475 qq=92834643 iq=1279425 a=-16
              v=1208591 sqrt=1099 resto=790  -> b=0x4b   (vendor 0x4c)
      core 1: v=1169028  sqrt=1081 resto=467  -> b=0x39   (vendor 0x39, esatto)

  Il round-up scatta se `resto > b`. Al core 0 manca `v + 310` su 1208591, cioe'
  **0.026%**; al core 1 servirebbe `v + 614` e infatti non scatta. Non e' la
  formula: una perturbazione minima degli ingressi fa attraversare la soglia a
  un core solo.

  L'accumulatore e' uno shift register a 2 posizioni e il solve somma le ultime
  due letture. Provate **tutte** le coppie dei 6 round osservati: nessuna da'
  `0x4c` sul core 0, e `5+6` -- quella che il port usa -- e' anche l'unica che
  da' il valore esatto sul core 1. Quindi il contributo mancante non e' dentro
  la finestra: gli accumulatori non vengono azzerati fra channel setup e il
  driver stock porta un residuo da prima della cattura.

  **Stessa causa del banco `0x0910` che vuole `3` alla prima passata del
  down->up**: entrambi i residui del flow 2 sono stato portato da prima della
  finestra. Non riproducibili da questa cattura, e non difetti del port -- che
  infatti sull'attach da freddo combacia bit-exact su entrambi.

  **Escluso per bound, non per fit.** `reverse-tools/fit_rxiq_solve.py` prova le
  forme candidate contro i 4 punti di verita' (2 catture x 2 catene,
  accumulatori
  in ingresso e coefficienti scritti dal vendor). Il termine di ampiezza e'
  `sqrt(qq/ii * 2^20 - correzione)` con correzione >= 0 in ogni variante, quindi
  `qq/ii` e' un limite superiore. Sul punto critico serve `qq/ii >= 1.153141975`
  e nessuna combinazione lo raggiunge:

      rapporto delle somme, ultimi 2/3/4/6:  1.152846 1.152754 1.152619 1.126695
      media dei rapporti,   ultimi 2/3/4/6:  1.152825 1.151779 1.151427 1.102817
      rapporti per-round: 0.967 1.044 1.150 1.150 1.159 1.147

  L'unico valore osservato sopra soglia e' il round 5 da solo (1.158998), che
  non
  e' ne' una somma ne' una media e rompe gli altri tre punti. Sugli altri tre
  punti invece piu' combinazioni raggiungono la soglia: e' quel punto a essere
  irraggiungibile, non la formula a essere sbagliata.

- **Il solve e' la forma N-PHY, il vendor ha un arcotangente -- ma i dati dicono
  divisione.** L'AC-PHY ha `AtanTbl` (72 byte, referenziata solo da
  `wlc_phy_cordic`/`wlc_phy_inv_cordic`) e raggiunge `inv_cordic` da
  `wlc_phy_calc_iq_mismatch_acphy`, `wlc_phy_cal_rx_fdiqi_acphy` e i due
  percorsi
  PAPD. Il port implementa la forma a divisione di `brcmsmac`, dove il cordic
  serve solo a generare toni.

  Cio' che i 4 punti discriminano:

  | forma per `a` | punteggio |
  |---|---|
  | `-(iq<<10)/ii` sulle somme delle ultime 2 (port) | **4/4** |
  | media di `atan2(iq_r, ii_r)` sulle ultime 2 | **4/4** |
  | `atan2` delle somme (qualunque finestra) | 3/4 |
  | `atan2`/`asin` normalizzati su `sqrt(ii*qq)` | 0/4 |

  Il punto che discrimina e' `down->up c1`: la tangente da' `43.516 -> 44` (il
  valore vendor) e l'arcotangente `43.49 -> 43`. Cadono ai due lati del bordo
  `43.5`, quindi a queste ampiezze le due forme **sono** distinguibili su un
  punto, e il dato esclude l'`atan2` applicato alle somme.

  Per `b` nessuna delle 9 varianti x 4 regole di arrotondamento discrimina: 13
  combinazioni danno 3/4 con valori identici, incluse `cos(phi)`,
  `sqrt(qq/ii - sin^2 phi)`, `- tan^2 phi` e la proiezione
  `(qq - iq^2/ii)/ii`. Il termine di ampiezza resta non determinato da questi
  dati.

  **Limite da tenere presente**: la forma del port e' validata solo nel regime a
  piccolo angolo che queste catture esercitano, `|a| <= 44/1024 ~ 2.5 gradi`. Su
  una board con sbilanciamento peggiore la forma a divisione e quella
  trigonometrica divergerebbero e nessun gate attuale lo vedrebbe.
- **`RAD.WR`** (2 op flow 1: `0x224`/`0x225`; 3 op flow 2: `0x3`/`0x5`/`0x203`).
  Non classificati.
- **10 `MAC.MCTRL` sul flow 1.** Cadono su un passaggio `cpu0 -> cpu1` e sono
  plausibilmente il refcount MAC di un altro contesto interlacciato dal tracer.
  Riprodurle nel driver migliorava il flow 1 e peggiorava il flow 2 di 1180 op:
  e' un artefatto di scheduling, che l'harness non modella per disegno. Per
  portarle a zero non si tocca il driver, si decide cosa il gate considera
  confrontabile.

## I valori trascritti sono impalcatura, non dati

**Questo punto viene prima del gate.** Una parte del programming non e'
derivata:
sono valori letti dalle catture e cablati per far partire il bring-up. Non sono
dati del driver, e inseguire il 100% di corrispondenza sui due flussi non li
rende tali -- li radica.

Fra quei valori ci sono gain, indice massimo di potenza TX, default delle LUT di
gain, ampiezza del generatore di tono e soglie CCA. Su una catena RF diversa da
quella da cui sono stati letti non sono "un po' sbagliati": possono
sovrapilotare il PA. Il rischio non e' un diff che non torna, e' hardware rotto
su una board che nessuno di noi ha in mano.

### Comportamento: si programma solo la configurazione da cui i valori vengono

`b43_phy_ac_set_channel` ora rifiuta tutto cio' che non e' **ch36 BW20 su chip
4352 o 4360**, con `-EOPNOTSUPP` e un warning che dice il perche'. Prima il
filtro era la tabella dei canali, che accetta tutto il 5 GHz: sintonizzare ch100
faceva scrivere le costanti di ch36.

E' un limite di **impalcatura, non di capacita'**: ch44 e BW40 sono osservati ma
non coperti op-per-op, e si sa che almeno due costanti differiscono --
l'ampiezza del tono vale `0x0154` su ch36 e `0x0152` su ch44, e le soglie
`crs_min_pwr` cambiano indice. Ogni canale che si aggiunge a quel filtro va
giustificato con una cattura, non con "probabilmente va bene".

### Il chip_id non descrive il front-end

Fra i pin RF del chip e l'antenna c'e' il **front-end**: PA esterno, eLNA,
switch T/R, filtri. Lo stesso 4352 sta su board con PA diversi, e allora lo
stesso indice di potenza TX produce una potenza d'uscita diversa -- un indice
tarato su una board puo' portarne un'altra in compressione o oltre il massimo
d'ingresso del suo PA.

L'SROM lo descrive. Sulle tre board in repo:

| | boardtype | aa5g | maxp5ga0 | pa5ga0[0..2] |
|---|---|---|---|---|
| d6220 | `0x668` | 3 | `72,70,86,0` | `0xff33,0x175b,0xfd32` |
| dsl3580l | `0x668` | 3 | `76,76,76,76` | `0xff4d,0x1690,0xfd24` |
| agcombo | `0x633` | 7 | `74,74,82,82` | `0xff3e,0x167e,0xfd20` |

La **topologia** e' condivisa -- `femctrl=6`, `pdgain5g=10`,
`subband5gver=0x4` identici su tutte e tre -- ma la **taratura del PA** e' per
board: `pa5ga` e `maxp5ga` differiscono su tutte e tre, e `boardtype` non
distingue d6220 da DSL.

### `femctrl`: era assunto, ora e' letto

`femctrl` seleziona lo schema di controllo del FEM -- linee di enable del PA e
stato dello switch T/R -- e vale 6 su tutte e tre le board, verificato in NVRAM.

`b43_phy_ac_set_regtbl_on_femctrl` scriveva una tabella `fem6_tbl[32]` cablata
**assumendo** femctrl 6, senza mai leggerlo. E' il caso piu' diretto di tutti:
non e' un valore di gain un po' fuori taratura, e' pilotare le linee di
controllo del front-end con lo schema di un'altra board -- il PA abilitato
quando non dovrebbe, o lo switch nello stato sbagliato durante il TX.

Ora `femctrl` viene letto e la funzione si ferma se non e' 6, con un warning.
Meglio un bring-up che fallisce che un front-end pilotato a caso.

**Risolto: offset fissato e patch aggiornata.** `bcmsrom_tbl.h` (GPL, in
asuswrt-merlin e bcmdhd -- gli URL erano gia' in `sprom-rev11/cross_check.md`)
da' `femctrl` a `SROM11_FEM_CFG1` con maschera `0xf800`, bit 15:11. Con i valori
NVRAM la word attesa e' `0x30A1`, e nei dump raw sta al byte **0x0AA**, con
`FEM_CFG2` = `0x00A1` a `0x0AC`: dodici campi su dodici, su due board.
`patches/0001` ora definisce i due offset, i dodici campi in `struct ssb_sprom`
e la loro estrazione in `bcma_sprom_extract_r11` (`git apply --check` passa su
torvalds/linux master). L'harness non porta piu' `femctrl` come valore cotto: il
profilo board porta le due word raw e le decodifica con le stesse maschere.

Nota storica sul perche' mancava: il valore e' identico su entrambe le board di
riferimento, quindi il value-matching che ha fissato gli altri offset qui non
funziona -- un vincolo solo, 17 word candidate. Serviva la fonte canonica, non
un terzo dump.

Il draft ampio in `sprom-rev11/` lo aveva gia' nella `struct` e nel percorso
NVRAM, ma senza offset raw: `bcma_sprom_extract_r11` non poteva riempirlo, e
su questo porting conta quel percorso, non il filler NVRAM.

Quindi la chiave non e' il board type. **Un valore di potenza trascritto non si
scrive mai senza confrontarlo con cio' che l'SROM dichiara.** Applicato
all'indice massimo di potenza TX: la costante di impalcatura `0x38` e' ora
clampata al limite derivato da `maxp5ga`. Sulle tre board quel limite vale
`0x42`, `0x46` e `0x44`, tutti sopra `0x38`, quindi il clamp e' un no-op e i
gate non cambiano; su una board con `maxp5ga` piu' basso impedisce di superare
il massimo dichiarato dal suo front-end, e lo dice in un warning.

### Marcatore `SCAFFOLD(ch36)`

I siti che scrivono valori trascritti e toccano RF sono marcati
`SCAFFOLD(ch36)` -- e `SCAFFOLD(femctrl6)` per la tabella di controllo FEM --
distinti da `TODO(formula)`: il secondo dice "manca la
formula", il primo dice "questo non e' un valore, e fuori da ch36 e'
pericoloso". Sono greppabili, e sono cinque: soglie `crs_min_pwr`, ampiezza del
tono, blocco `0x60`/`0x64`, default LUT di gain del core 2, e **indice massimo
di potenza TX** -- quest'ultimo il piu' pericoloso del file, perche' un indice
troppo alto sovrapilota il PA.

La regola operativa: un `SCAFFOLD` si rimuove solo derivando il valore, oppure
allargando il filtro dei canali con una cattura a supporto. Non si rimuove
perche' il gate passa.

## Audit di cio' che e' cablato: cosa e' derivabile davvero

Rifatto a fondo dopo aver derivato il solve RXIQ e il filtro fdiqi, perche' il
pattern che e' emerso li' ricorre altrove. Tre categorie.

### 1. TODO stantii: il valore e' GIA' derivato

- **Tabella 18-iter della cal AFE.** Diceva "valori hardcoded dal trace d6220
  ch36, TODO per derivarli dal readback". Falso: gli unici dati statici sono i
  `pre[]`, che sono offset di pre-clear. I risultati li legge `rxcal_afe_iter`
  dal readback e li riscrive. I `result 0xNNNN` nei commenti sono osservazioni.
- **`wr_vals[2]` in `run_meas_iters`.** Campo dichiarato, inizializzato con i
  valori trascritti, **mai letto**: codice morto, con il TODO che lo
  accompagnava. Rimossi entrambi.

Entrambi i marcatori sono stati corretti: lasciarli spinge a rifare un lavoro
gia' fatto.

### 2. Descrizioni sbagliate: non sono la grandezza che dicono

- **`0x0724 = 0x03ff` e `0x0736 = 0x0154`.** Il commento li dava per
  "coefficienti I/Q calcolati runtime da rxcal_imbalance". Non lo sono: il
  driver stock scrive **gli stessi valori su entrambe le catene**
  (`0x0724`/`0x0924`, `0x0736`/`0x0936`) e in entrambe le fasi, mentre i
  coefficienti per catena differiscono (`a` = -17 contro -44). Sono una coppia
  di
  costanti **per canale**, scritte insieme e azzerate insieme: arm/disarm del
  generatore di tono. E il commento citava anche la costante sbagliata --
  `0x0152` e' ch44, ch36 vale `0x0154`, come gia' scritto accanto a `gw_hi`.
  Quindi non serve una formula da `rxcal_imbalance`, serve una mappa
  canale -> valore, e i punti noti sono due.

### 3. Cablato ma parzialmente derivabile

- **Blocco B2m su tabella `0x0007`.** Gruppi di 3 celle per catena,
  `(0x100,0x103,0x106)` e `(0x101,0x104,0x107)`. Nelle due catture il vendor
  legge `(0x0000, 0x2f13, 0x00f3)` e scrive `(0x0000, 0x4f7f, 0x00f3)` sulla
  catena 0 e `(0x0000, 0x2f7f, 0x00f3)` sulla catena 1. Le **due celle esterne
  sono copie della lettura** -- che il port butta in `dummy_rd` -- e nella cella
  centrale il byte basso va a `0x7f` in entrambe le catene e in entrambe le
  catture. Resta il byte alto (`0x2f -> 0x4f` sulla catena 0, invariato sulla
  catena 1): un punto per catena, non abbastanza.

### Costanti che sono in realta' dati di board

Una costante trascritta da cattura e una derivata dall'SROM sono
indistinguibili nel sorgente: entrambe sono letterali. La differenza si vede
solo su un'altra board -- cioe' quando e' tardi.
`reverse-tools/find_srom_backed_consts.py`
cerca la sovrapposizione in anticipo: ogni letterale del driver che coincide con
un valore dichiarato dall'SROM della board di riferimento e' un candidato.

Due stadi, perche' il primo da solo e' rumoroso. **Match di valore**: le
costanti piccole e ubique collidono per caso (`0x0002` compare 262 volte nel
driver e coincide con `boardflags2`), quindi contano i match **specifici** --
pochi usi, campo di potenza o gain. **Conferma canonica**: `bcmsrom_tbl.h` dice
se quel campo esiste davvero nel layout, e allora il match e' reale.

Trovato e sistemato: **`tssifloor5g[grp]`**, cablato come `0x03ff` su PHY
`0x0724` in due punti. Sulle board in repo il campo **non e' programmato** e la
SROM legge `0xffff`, che mascherato con `0x03ff` da' esattamente la costante
cablata -- per questo la trascrizione era invisibile. Su una board che lo
programma davvero, scrivere `0x03ff` crudo ignorerebbe in silenzio il floor
dichiarato. Ora il driver lo legge, e `patches/0001` estrae
`tssifloor2g` + `tssifloor5g[4]` (word 95..99, maschera `0x03ff`).

Candidati che restano da guardare nel codice, dallo stesso strumento:

| valore | usi | campi SROM | nota |
|---|---|---|---|
| `0x0085` | 1 | `aga0..2` | uso singolo, da guardare |
| `0x0046` | 4 | `maxp5ga0/1` | potenza |
| `0x0047` | 5 | `agbg0..2` | |
| `0x004c` | 6 | `maxp5ga2` | potenza |
| `0x0048`, `0x0056` | 9 | `maxp5ga0/1` | potenza |
| `0x0042` | 12 | `maxp2ga0..2` | potenza (2G, forse irrilevante) |

Da leggere con la testa: pochi usi piu' un campo di potenza vuol dire "vai a
vedere quella riga"; molti usi vuol dire quasi certamente coincidenza. Lo
strumento non decide, restringe.

### Il pattern da cercare, meccanicamente

Su 53 `actab_read_bulk` nel port, **circa 36 scartano il valore letto**
(`&discard` x11, `&dummy_rd` x10, piu' i `tblr_dummy_*` e `dummy_baseidx*`). In
due casi accertati il valore scartato e' esattamente quello che il vendor
scrive: la copia in `rxcal_afe_iter` e le celle esterne del B2m. Quella lista e'
il posto giusto da cui ripartire, ed e' greppabile: ogni lettura che il vendor
fa e noi buttiamo e' informazione che abbiamo e non usiamo.

Attenzione al rovescio: una lettura scartata puo' essere legittima. Il vendor
legge anche per sincronizzare o per sbloccare un gate, non solo per usare il
dato -- il peek su `0x019e` e il peek finale su `0x0270` sono di quel tipo. Il
criterio e' se il valore ricompare in una scrittura successiva.

## Catture ancora utili (al prossimo riflash DSL)

Attach da freddo non catturabile (wl si deregistra). Restano: secondo canale
5G (terzo `lpf_cap0`), BW80, 2.4G (mappa radio 2G non validata), cicli down/up
(budget dei poll). Dettaglio in `dsl-capture-plan` (output di sessione).
