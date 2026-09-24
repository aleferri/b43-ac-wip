# TODO di validazione — divergenze note del bring-up

La localizzazione per-funzione ora non usa più i fingerprint indovinati dai
sorgenti: l'harness marca ogni funzione con `B43_AC_FN()` (attivo con
`AC_FN_MARKERS=1`), quindi `fn_map.py span` e `fn_map.py coverage` hanno
confini esatti. La copertura si misura contro
la cattura **grezza** (come `compare.py`), non contro il collassato.

## Il set di catture, e quali conteggi sono del set vecchio

Il set corrente del d6220 e' **43 segmenti a freddo** -- 25 a 20 MHz, 12 a 40,
6 a 80 -- e **44 segmenti `up` a caldo**, 87 in tutto. L'agcombo sta a 26 a
freddo e 26 a caldo, la dsl3580l a uno sweep solo.

Il set precedente era **26 a freddo e 52 `up`** (26 configurazioni per due
cicli), 78 in tutto. Quindi ogni frase di questo documento che conta `26`, `52`
o `78` segmenti, e ogni riferimento a un segmento per numero d'ordine, e' del
set vecchio: la numerazione non si e' conservata -- il `cold17` di allora
(ch36 bw40) oggi e' `cold26`, il `cold24` (ch36 bw80) e' `cold38`, mentre
`cold17` e `cold24` oggi sono ch132 e ch161 a 20 MHz. Dove i conteggi sono
stati rifatti sul set corrente la sezione lo dice; le altre portano un avviso
in testa.

**La ricattura non ha solo allargato il campione.** I 21 segmenti con la
guardia radar sono stati ripresi **con il CAC completato**, e sopra i 5250 MHz
il vendor esegue le calibrazioni che sul set vecchio non si vedevano. Misurato
sui 43 segmenti a freddo di oggi:

| testimone | set vecchio, sopra 5250 | set corrente |
| --- | --- | --- |
| `PHY.RD 0x0380` (generatore di tono) | zero su tutti | 209-912, zero **solo** sui sei radar-meteo |
| `PHY.RD 0x0270` | zero su tutti | 72-189, stesse sei eccezioni |
| `PHY 0x0724`/`0x0736` (rxgain) | zero dal ch52 in su | 48-60 dal ch52 in su, 9 sui sei radar-meteo |

I sei radar-meteo -- ch120, ch124, ch128 a 20 MHz, ch116 e ch124 a 40, ch116 a
80 -- sono ancora vecchio stile, e sono loro l'unico zero rimasto. E' la stessa
misura che ha fatto passare `may_calibrate_tx()` dalla soglia sui 5250 al flag
della guardia radar, e invalida ogni conclusione del set vecchio che avesse la
soglia come termine.


## Stato copertura bring-up (rfkill + op_init), per sequenza

| funzione | d6220 | DSL | agcombo |
|---|---|---|---|
| radio_2069_init | 100% | 100% | 100% |
| r2069_prefregs_init | 100% | ~92% | 100% |
| radio_2069_pwron | 100% | 100% | 100% |
| radio_2069_rccal (+setup/run) | 100% | 84% | 84% |
| radio_2069_afe_lpf_stage | 100% | ~4% | 100% |
| op_init sub non-tabella (set_pdet, pre_init, mode_init) | 100% | 100% | 100% |

La localizzazione per-funzione nelle catture non e' piu' annotata nei sorgenti:
era una tabella generata, incolonnata a mano nei commenti e mezza vuota
("n/l" su un lato). Si rigenera con `reverse-tools/fn_map.py fingerprints`,
che la ricava dalle fingerprint del sorgente corrente invece di fidarsi di
un commento che nessuno riallinea.

d6220 e agcombo: bring-up radio coperto integralmente, gap 0%. Le divergenze
sono tutte sul DSL (wl 6.30).

## Il DSL non e' un oracolo

Il bersaglio del port e' il d6220, e il confronto si fa contro le sue catture.
Il DSL-3580L serve per una cosa sola: gira wl 6.30 su kernel 2.6.30, quindi
espone percorsi che sulle catture d6220 non si vedono -- non perche' il d6220
non li faccia, ma perche' quando sono state prese gli hook non c'erano. Quello
che se ne ricava si porta nel driver e poi si torna sul d6220.

Percio' le divergenze del DSL elencate sotto NON sono debito del port verso
quella board: sono differenze di versione o di taratura, e stanno qui come
contesto. Nessun punteggio si misura sul DSL, e i suoi segmenti non vanno usati
come oracolo -- fra l'altro la fase probe la' non mostra i marcatori
`PHY.MOD 0x0520` e `PHY.RD 0x07af` che sul d6220 ci sono, e non e' una cosa da
capire: e' una board che non stiamo portando.

## Avvisi nel driver

I punti dove il driver scrive qualcosa che non sa derivare emettono un
`b43warn` una volta per sito, con la macro `b43_phy_ac_todo()`. Una volta e non
ogni volta perche' alcuni stanno in un ciclo di calibrazione; per sito e non
globale perche' quale punto si e' toccato e' l'informazione utile. Non e'
`B43_WARN_ON`, che segnala uno stato che non dovrebbe accadere: questi
accadono per costruzione, e il messaggio serve a chi legge un dmesg dopo che
qualcosa non ha funzionato.

Oggi `grep -n b43_phy_ac_todo src/*.c` elenca i siti.

## L'oracolo cieco, e perche' gli ancoraggi vanno verificati

I commenti dei sorgenti citano l'indice dell'op nella cattura da cui la
sequenza e' stata trascritta. Quelli di `channel_setup_tail` nella banda
`#39189`-`#39554` puntavano a `router-data/d6220/wl-diag-wl1-attach-to-bss-ch36.txt`,
**rimossa nel commit `b6c6942`**, e quella cattura non registrava nessuna op
`OBJ` ne' `TPL`: solo PHY, RAD, TBL, `MAC.MCTRL`, `MAC.MHF`, GPIO e PMU.

Le conseguenze sono due e vanno tenute separate. La prima e' che l'ordine del
lavoro di shared memory dedotto da la' **non e' mai stato vincolato da
un'osservazione**: lo strumento non lo vedeva. La seconda e' che una cattura
rimossa non e' un oracolo, quindi quegli ancoraggi non si "risolvono" andando a
ripescarla, si riancorano a una cattura presente o si cancellano.

`reverse-tools/anchors.py class` fa il controllo per tutti. Risolve i
`RETVAL` sulla lettura che li precede, accetta una `PHY` dove il codice scrive
una tabella -- il marcatore `TBL` e le op sulla porta dati stanno allo stesso
indice -- e segnala gli ancoraggi che in nessuna cattura presente cadono sulla
classe di op che il codice emette li'. Al primo giro: 20 orfani in `phy_ac.c`,
3 in `radio_2069.c`, zero in `rxiqcal_phy_ac.c` e `tables_phy_ac.c`. Le sette
parentesi maccontrol della coda sono riancorate a `cold01 #13665-#13672`, dove
combaciano op per op e maschera per maschera. **Restano 11 orfani in
`phy_ac.c`** -- `#39189`-`#39199`, `#39545`, `#39554`, `#12375`, `#30919` -- e
3 in `radio_2069.c`.

### Le "5 pulses" sono quattro conf_tx

Il ciclo di `channel_setup_tail` commentato "5 pulses mac wake/suspend" e' la
cosa piu' sbagliata che l'oracolo cieco ha lasciato. `cold01` mette quelle
parentesi a `#14083`, `#14168`, `#14247` e `#14326`: **quattro**, separate da
~79 op, e ognuna ha dentro un carico.

Il carico di ogni giro:

| | |
| --- | --- |
| blocco EDCF, una coda diversa a ogni giro | `0x0260-0x027e`, poi `0x0240-0x025e`, `0x0280-0x029e`, `0x02a0-0x02be` -- ognuno letto nella cella di coda e riscritto per intero, 16 parole |
| template probe response | `TPL.RAMW 0x0700` e `0x004a` (PRTLEN) |
| SSID | `0x0160-0x017e`, con `0x0048` (PRSSIDLEN) |
| PLCP degli otto rate | otto terzetti `OBJ.RD 0x01dX` + tre `OBJ.WR` a `0x0994+n` |

Il primo giro ha in piu' `0x00cc` due volte, `0x001e` (TIMBPOS), `TPL.RAMW
0x0480` e `0x001a` (BTL1). E' la forma di quattro `conf_tx` di mac80211, una
per coda, che arrivano dopo il setup di canale.

Il conteggio non si corregge da solo, ed e' misurato: portarlo a quattro senza
il carico fa scendere `cold01` da 27840/29030 a 27838, con due mancanti in
piu', perche' l'LCS appaiava la quinta coppia a una parentesi altrove. Il
numero di parentesi diventa misurabile solo quando ognuna ha dentro la sua
passata.

Da qui il muro a `@11823` si spiega: e' `OBJ.RD 0x00cc`, l'inizio della prima
di quelle quattro passate, e nel port non c'e' niente. `emit_core_bss_config1()`
in `test/unit/main.c` ne implementa un pezzo, ed e' **codice morto**, dichiarata e
definita e mai chiamata. `b43_phy_ac_prb_rsp_plcp()` emette i terzetti PLCP
dalla testa della coda PHY, dove l'oracolo cieco permetteva di metterla, mentre
la cattura viva li mette in coda a ogni passata: sono 16 occorrenze in
`cold01` contro le 6 del port.

## Il bit `0x80` di shm `0x00cc`: chiuso, e' il campo della maschera

Non e' un bit a se': e' il bit alto del campo 6-8, che la sezione sull'header
BSS identifica come la maschera delle catene. `0x44` e `0xc4` sono lo stesso
valore con il campo a 1 e a 3 -- `(0x44 >> 6) & 7 = 1`, `(0xc4 >> 6) & 7 = 3`.

La misura che lo chiude, sugli 87 segmenti del set corrente: i segmenti che
scrivono `0x44`/`0x45` sono **esattamente** quelli dove `0x05d6`/`0x05d8`
portano la maschera parziale `1` invece di `coremask`, e sono dodici -- ch36-48
a 20 MHz e ch36, ch44 a 40 MHz, a freddo e nei corrispondenti `up`. Gli altri
75 scrivono `0xc4`/`0xc5` con la maschera a `3`. Sui quattro a 40 MHz il valore
cambia **dentro** il segmento, `0x45` e poi `0xc5`, ed e' lo stesso segmento in
cui la cella della maschera porta prima `1` e poi `3`.

`emit_core_bss_config()` di `test/unit/main.c` scrive i due letterali `0x0044` e
`0x0045` trascritti da `cold01`: giusti su quei dodici segmenti e sbagliati
sugli altri 75, che e' la ragione per cui la voce stava qui. Il valore va
composto dalla maschera in forza, non trascritto -- che e' il debito gia'
descritto nella sezione dell'header BSS, dove sta anche cosa resta opaco (il
bit 0 e il bit 2).

La lettura che precede le due scritture torna `0x44` su tutti e 87 i segmenti,
quindi il valore scritto resta una decisione del driver e non un'eco.

**Perche' la scansione non lo aveva trovato.** Rifatta sul set corrente -- 1554
celle presenti in tutti e 87 i segmenti, ogni classe -- una cella con la stessa
partizione non esce lo stesso, e la ragione e' il criterio: cercava un valore
costante dentro ciascuno dei due gruppi, e sui quattro segmenti a 40 MHz la
cella della maschera ne porta due. Un compagno che cambia dentro il segmento e'
invisibile a una scansione per segmento, ed e' il motivo per cui il criterio
andava posto per scrittura e non per segmento.

## La parola su shm 0x00b8

Una scrittura sola, `OBJ.WR 0x00b8 = 0x7148`, fra il clear della finestra
statistiche e il `mac_suspend` di `post_cal_finalize_iter3()`. Non emetterla
teneva il muro posizionale di `cold01` a `@13093`; emetterla lo porta a
`@15943`, cioe' **2850 op contigue per una op**. Vale la pena sapere perche' una
sola op pesi cosi': il grezzo non si muove -- resta 96.84% -- perche' le op che
seguono erano gia' appaiate dall'LCS, solo fuori posizione. E' la misura di
quanto le due metriche dicano cose diverse.

Cosa e' stabilito sul valore:

- **0x7148 su 111 scritture su 111**: 7 nello sweep a freddo del d6220, 104 in
  quello a caldo, 7 su agcombo. Non dipende da canale, larghezza, chip ne'
  primo bring-up, e non e' un contatore -- sette attach diversi lo scrivono
  identico;
- non e' letto da nessuna parte nella traccia e non compare nell'NVRAM, quindi
  non e' la copia di qualcosa;
- le tre letture che precedono sempre il blocco -- PHY `0x0070`, `0x0640` e
  `0x0840` -- **non** lo determinano: sul DSL la stessa terna precede valori
  diversi;
- sul DSL, che gira wl 6.30 e non e' un oracolo, la cella prende `0x1130` in
  regime piu' `0x003c`, `0x012c`, `0x060e` e `0x251c` una volta ciascuno. Il
  discriminante sembra la versione del blob e non l'hardware;
- `b43.h` non nomina la cella. Come intero e' 28998, e sul DSL 4400: nessuna
  delle letture come rate, durata o lunghezza di frame regge su tutti i valori.

Nel percorso a freddo la scrittura c'e' solo sotto i 5250 MHz, ma non e' un
gate di frequenza: nello sweep a caldo il vendor la emette a ogni canale,
ch52 e ch100 compresi. La ragione e' che sopra i 5250 il vendor non fa il
clear della finestra dentro `post_cal_finalize_iter3()` -- 19 scritture di
`0x0308` contro 20 -- quindi la scrittura non e' assente per se': manca il
blocco che la contiene. Non e' gatata proprio per questo: erediterebbe un
gate che non le appartiene, e la op di troppo che lascia sui 19 segmenti alti
e' una sola, parte del surplus di quella fase.

## BSLOTS e REGGAP: due celle che nessun codice puo' prevedere

Il muro di `cold02`, `cold03` e `cold04` stava a `~@11830` su `OBJ.WR 0x026a`,
cioe' BSLOTS del blocco EDCFQ best effort: il vendor ne scrive 2 dove il port
scrive 9. Non e' un difetto del port. **BSLOTS e' il backoff estratto a caso
all'inizio del contention window**, e la misura su tutti e 43 i segmenti e
tutte e quattro le code, 172 punti, lo mostra senza margine:

| coda | CWMIN | valori osservati |
| --- | --- | --- |
| best effort | 15 | tutti da 0 a 15 tranne il 3 |
| background | 15 | tutti da 0 a 15 |
| video | 7 | tutti da 0 a 7 |
| voce | 3 | 0, 1, 2, 3 |

Uniforme in `[0, CWMIN]` su ognuna, e con 43 segmenti tre code su quattro
coprono il proprio intervallo per intero, alla best effort manca il solo 3. E
**`REGGAP = AIFS + BSLOTS` regge su tutti e 172 i punti**, quindi la seconda
cella non e' indipendente: e' la prima piu' una costante nota.

Percio' le due celle sono dichiarate in `VAL_NONDET` in `test/unit/compare.py`, dove
si confrontano indirizzo, classe e posizione ma non il valore. Non e' una
scorciatoia per nascondere un difetto: pretendere quel valore vorrebbe dire
indovinare un numero casuale, e la relazione di REGGAP e' la prova che il resto
del blocco e' capito. Sposta i muri di `cold03` a `@15887` e di `cold04` a
`@15801`, 4055 e 3970 op contigue.

### Quel che resta su cold02

`cold02` si e' mosso poco, da `@11828` a `@11981`, e la ragione e' diversa: su
quel segmento il vendor fa **piu' caricamenti di template** degli altri. Il
censimento delle celle del percorso beacon:

| segmento | `0x004a` | `0x00cc` | `0x0018` | `0x001a` |
| --- | --- | --- | --- | --- |
| cold01 ch36 | 10 | 21 | 5 | 3 |
| cold03 ch44 | 12 | 25 | 6 | 4 |
| cold02 ch40 | 16 | 37 | 9 | 7 |
| cold04 ch48 | 18 | 37 | 9 | 7 |
| cold17 ch36 bw40 | 21 | 45 | 11 | 9 |
| i 19 da ch52 | 7 | -- | -- | -- |

Sui 19 alti sono sempre esattamente 7. Sui sette bassi variano, e in ogni riga
`0x0018` e' `0x001a` piu' due. I **primi cinque** caricamenti hanno invece la
stessa forma su ogni segmento -- uno per la config BSS piu' i quattro delle
passate conf_tx, con la stessa spaziatura -- e sono quelli che il port emette;
gli altri stanno molto piu' avanti, nella fase periodica. La durata delle
catture e' la stessa a meno di due secondi, quindi non e' il tempo trascorso a
spiegare la differenza. Cosa la spieghi non e' stabilito, ed e' il prossimo
punto da caratterizzare per la contiguita' dei tre segmenti che restano
indietro.

## idle_tssi_meas, e il rilascio che non e' parte della misura

Il muro dei 19 segmenti alti a `@9609` era causato da **591 op di troppo del
port prima di quel punto, 563 delle quali di `idle_tssi_meas`**: una fase su
quattro spiegava il 95%.

La fase e' assente per intero sopra i 5250 MHz al primo bring-up, e la conta
del suo apritore lo dice esatta:

| `RAD 0x0548` | vendor | port | di cui la fase |
| --- | --- | --- | --- |
| ch36 | 18 | 18 | 9 |
| ch52 | **9** | 18 | 9 |

I 9 che restano sopra la soglia vengono da `radio_2069_pwron` (6) e
`radio_percore_setup_1` (3), che girano da entrambe le parti. Confermano
`RAD 0x004e`, `0x0166`, `0x024e`, `0x0366` (30 sotto, 0 sopra) e `PHY 0x0747`,
`0x0732`, `0x0733` con i mirror a `+0x200` (20 sotto, 0 sopra).

**Ma il gate secco rompe la macchina a stati del port**, e vale saperlo perche'
e' il tipo di errore che il punteggio non mostra: il flow si ferma con
`txpwrctrl_setup precondition failed status=0x0878 want=0x000c` e il port emette
11244 op invece di 18786. La causa e' che la fase chiude con
`rx_gate_with_adc_hold(dev, false)`, e quel rilascio porta la transizione
`RX_WAITED | RX_OFDM` che la fase dopo pretende.

Il rilascio **non e' parte della misura**: e' la restituzione di un gate che il
chiamante ha armato. E la cattura lo conferma -- `PHY 0x0140` fa 37 accessi a
ch36 e **5** a ch52, non zero, mentre la chiusura della misura vera,
`PHY.WR 0x0401 = 0x7733`, fa 4 accessi a ch36 e **zero** a ch52. Il corpo
sparisce, il rilascio resta. Quindi il gate copre il corpo e lascia fuori il
rilascio.

I 19 segmenti passano da 74.9-75.6% a **83.0-84.1%**, e i due a 80 MHz da 53.5%
a 83.3%. Le op di troppo scendono da ~3730 a 1950 e i valori sbagliati da
~100-340 a 46-129. Le 591 op di troppo prima del muro diventano **22**:
`adc_hold` 8, `clip_det` 6, `txpwrctrl_enable` 2, `classctl_write` 2,
`cca_pulse` 2, e due singole.

### Perche' il posizionale resta a @9609

Il grezzo sale di otto punti e il muro non si muove, ed e' istruttivo. Le tre
invocazioni saltate emettono **tre** coppie RD+WR su `0x0140`, mentre il vendor
a ch52 ne ha una piu' una `MOD` altrove. Condizionare il rilascio al gate
armato -- `RX_WAITED` senza `RX_OFDM` -- e' stato provato e non cambia niente,
quindi il gate viene riarmato fra le invocazioni e tutti e tre i rilasci sono
legittimi come tali: e' la loro **forma** a non tornare, non il fatto che ci
siano. Il prossimo passo su quella famiglia e' quella forma, non altre fasi.

## Il payload TX IQ/LO: due campi da 8 bit, non una parola

La prima divergenza di `cold01` era `PHY.WR 0x000f val=0xff02` contro `0x0002`,
cioe' il payload che `rxcal_afe_finalize_gain_luts()` scrive nelle tabelle
`0x0042`/`0x0062`/`0x0082`. Il sito scrive 128 offset per tre core con valori
uniformi per core e uno scalino a 0x21, e quella **struttura e' giusta**: sono
i valori a essere sbagliati.

Le due passate sulle tre tabelle, estratte dalla cattura:

| | 0x42 | 0x62 | 0x82 off 0x00-0x20 | 0x82 off 0x21-0x7f |
| --- | --- | --- | --- | --- |
| vendor, 1ª passata | `0xff02` | `0x0200` | `0x16e6` | `0x14dc` |
| port, prima | `0x0002` | `0x0200` | `0x0962` | `0x0758` |
| vendor e port, 2ª passata | `0x0000` | `0x0000` | `0xf8fc` | `0xf6f2` |

La seconda passata, l'azzeramento di `rxiqcal_coeff_tables_reset()`, combaciava
gia'. La prima aveva tre valori su quattro sbagliati, e i valori che il port
portava non corrispondono a nessuna delle catture presenti.

### La relazione, che e' una derivazione

Le due voci della `0x0082` **non sono indipendenti**. La parola e' due campi da
8 bit, e fra la voce degli offset bassi e quella degli alti la differenza e'
`(-2, -10)` per campo:

| segmento | off 0x00-0x20 | off 0x21-0x7f | delta per byte |
| --- | --- | --- | --- |
| cold01 ch36 bw20 | `(22, 230)` | `(20, 220)` | `(2, 10)` |
| cold02 ch40 bw20 | `(254, 42)` | `(252, 32)` | `(2, 10)` |
| cold03 ch44 bw20 | `(29, 31)` | `(27, 21)` | `(2, 10)` |
| cold04 ch48 bw20 | `(212, 16)` | `(210, 6)` | `(2, 10)` |
| cold17 ch36 bw40 | `(223, 244)` | `(221, 234)` | `(2, 10)` |
| cold18 ch44 bw40 | `(40, 65)` | `(38, 55)` | `(2, 10)` |
| cold24 ch36 bw80 | `(33, 2)` | `(31, 248)` | `(2, 10)` |
| coppia del bring-up successivo | `(171, 208)` | `(169, 198)` | `(2, 10)` |

Otto punti su otto, quattro canali, tre larghezze e le due condizioni di
bring-up. **La sottrazione va fatta per campo**: a 80 MHz il byte basso passa
da `0x02` a `0xf8`, e in aritmetica a 16 bit il prestito darebbe `0x010a`
invece di `0x020a`. E' proprio quel punto ad avere fatto vedere che i campi
sono due. Ora il driver tiene una parola e ricava l'altra.

### Le LUT 0x42/0x62/0x82 sono il risultato della cal LO, per indice

La voce della `0x0042` sui sette segmenti che eseguono la fase: `0xff02`,
`0xfd02`, `0xfe02`, `0xfe01`, `0x0202`, `0x0101`, `0xffff`. Non c'e' dipendenza
dal canale da cercare perche' non e' una funzione del canale: **e' la parola
di LO leakage che la cal TX IQ/LO ha appena prodotto per quel core**, cioe'
cio' che `rxcal_afe_calibrate()` ha scritto in IQLOCAL `0x62 + 4*core`, copiata
su tutti i 128 indici di potenza. Le tabelle `0x42/0x62/0x82` sono le LOFT
LUT del tx power control, una voce per indice, e il vendor le riempie con la
stessa parola perche' non ha una misura per indice.

Verifica su cold01 e sul flow a caldo `01-up-ch36-bw20`:

| | IQLOCAL 0x62 | LUT 0x42 | IQLOCAL 0x66 | LUT 0x62 | IQLOCAL 0x6a | LUT 0x82 |
| --- | --- | --- | --- | --- | --- | --- |
| cold01 | 0xff02 | 0xff02 | 0x0200 | 0x0200 | 0x1eea | 0x16e6 / 0x14dc |
| 01-up | 0xff01 | 0xff01 | 0x0201 | 0x0201 | 0x93f1 | 0x8bed / 0x89e3 |

Il core 2 e' la stessa relazione con una **base** sotto: la LUT vale la parola
LO piu' la base per campo, e la base e' quella che `rxiqcal_coeff_tables_reset()`
scrive dopo -- zero per i core 0 e 1, `(-8, -4)` fino all'indice 0x20 e
`(-10, -14)` da 0x21 per il core 2. Il passo `(-2, -10)` fra le due voci e' il
passo della base, non della cal. L'agcombo, che la terza catena la ha, dice
che la base e' del core e non della board: la' la parola LO del core 2 e'
`0xff00` su cold01 e la LUT `0xf7fc/0xf5f2`, `0x0000` su cold18 e la LUT
`0xf8fc/0xf6f2`, `0xfe02` sul flow a caldo ch36 e la LUT `0xf6fe/0xf4f4` --
sempre parola piu' base, voce per voce. Perche' solo il core 2 abbia una
base, e perche' scatti a 0x21, non ha prove: resta una costante con un SALAME.

Il driver ora ricava le tre LUT da `afe_res[1]`, `[3]`, `[5]`: niente valore
trascritto, e il `b43_phy_ac_todo()` che dichiarava i coefficienti "scritti da
tabella" e' tolto. Sul ferro la parola viene dalla cal della stessa corsa; nel
harness viene dall'oracolo, e con l'oracolo per cella (sotto) e' giusta anche
a caldo, dove prima il LUT usciva `0xfe02` contro `0xff01`.

Il paragrafo che segue e' quello del passo precedente, quando i valori erano
ancora quelli di `cold01` trascritti.

`cold01` passa da 96.84% a **98.59%**, i valori sbagliati da 259 a **3**, le
regioni da 296 a 40, e il posizionale da `@15943` a `@24865` -- **8922 op
contigue in piu'**. Gli altri 25 segmenti non si muovono, perche' il fit e' su
questo. La divergenza che resta a `@24865` e' `PHY.WR 0x06a1 val=0x51` contro
`0x50`: era il residuo sul coefficiente `b` della RX IQ, chiuso piu' sotto con la
media per tono.

### La base delle LUT segue la sotto-banda, non solo il core

La sezione sopra dichiara la base "una costante del core, non della board",
misurata su ch36 e confermata sull'agcombo. E' vera a meta': su ch36 il gruppo
pa5g e' 0, e su quel gruppo la base e' davvero quella. Letta la prima parola
delle tre LUT su tutti e 43 i segmenti, la partizione e' netta:

| | gruppo 0 e 1, < 5500 | gruppo 2, ch100-144 | gruppo 3, ch149-165 |
| --- | --- | --- | --- |
| `0x0042` core 0 | nessuna base | nessuna | nessuna |
| `0x0062` core 1 | nessuna base | nessuna | `(-10, -5)` |
| `0x0082` core 2 | `(-8, -4)` | `(-6, -5)` | `(-6, -5)` |

Misurata come differenza fra la parola del vendor e quella del port: lo scarto
del core 2 e' `(+2, -1)` identico su tutti e ventitre i segmenti che divergono,
quello del core 1 e' `(-10, -5)` identico su tutti e otto quelli di UNII-3.
Cinque canali per tre larghezze ciascuno, quindi non e' un fit su un punto, e i
venti segmenti di gruppo 0 e 1 non si muovono -- che e' il controllo negativo.

```
valori sbagliati sui 43 segmenti   11211 -> 9445
segmenti mossi 23, fermi 20        gate periodico MATCH, cold01 invariato
```

Lo scalino a 0x21 -- `(-2, -10)` in piu' da quell'indice -- e' stato misurato
solo sull'indice 0 e non e' stato rimisurato per sotto-banda.

**SALAME** sul perche': che la base esista, che scatti a 0x21 e che segua la
partizione di pa5g invece di un'altra non ha prove. Quel che si sa e' che segue
quella, su 43 segmenti.

### Lo scalino segue la sotto-banda anche lui — chiuso

La sezione sopra lascia in sospeso proprio quella riga, e il residuo era li'.
Sui segmenti di UNII-3 restavano ~200 parole sbagliate per LUT su `0x0062` e
`0x0082` -- 400 dei 427 valori sbagliati di cold21, cioe' quasi tutto il suo
debito -- e su quelli del gruppo 2 ne restavano ~330 su `0x0082`.

**Le due passate non sono due riempimenti.** La lettura precedente -- "il
vendor riempie la LUT due volte con parole LO diverse" -- e' sbagliata, e il
controllo e' il core 0: li' la seconda passata scrive `0x0000` su tutte e 128
le celle, che e' la base nuda e non un risultato di calibrazione. Le due
passate sono il riempimento di `rxcal_afe_finalize_gain_luts()` e, quattromila
op piu' tardi, il reset di `rxiqcal_coeff_tables_reset()`. La seconda quindi
**misura la base direttamente**, senza doverla dedurre da una differenza:

```
cold21   0x0042  0x00-0x7f = 0x0000
         0x0062  0x00-0x1a = 0xf6fb   0x1b-0x7f = 0xec00
         0x0082  0x00-0x1c = 0xfafb   0x1d-0x7f = 0xf6f6
```

Letta cosi' su tutti gli 87 segmenti dei due sweep che eseguono la fase -- 36 a
freddo e 37 `up`, i restanti non la eseguono -- la tabella e' questa, senza
eccezioni:

| | gruppo 0 e 1 | gruppo 2, ch100-144 | gruppo 3, ch149-165 |
| --- | --- | --- | --- |
| `0x0042` | 0 | 0 | 0 |
| `0x0062` | 0 | 0 | `f6fb`, `ec00` da 0x1b |
| `0x0082` | `f8fc`, `f6f2` da 0x21 | `fafb`, `f6f6` da 0x1d | `fafb`, `f6f6` da 0x1d |

Cioe' **anche lo scalino segue il gruppo**, sia dove cade sia quanto vale: il
core 2 passa da `(-8, -4)` a `(-10, -14)` con un gradino di `(-2, -10)` a 0x21
sotto i 5250, e da `(-6, -5)` a `(-10, -10)` con un gradino di `(-4, -5)` a
0x1d sopra; il core 1, solo su UNII-3, va da `(-10, -5)` a `(-20, 0)` con un
gradino di `(-10, +5)` a 0x1b. Il port ne aveva uno solo, `(-2, -10)` a 0x21,
applicato a tutti i gruppi.

```
valori sbagliati sui 43 segmenti   9359 -> 3189
minimo 96.93 -> 99.16   mediana 98.58 -> 99.44   massimo 99.90 invariato
cold01 e gli altri undici del gruppo 0/1 invariati -- il controllo negativo
gate periodico MATCH; 10-up-ch104 93.55 -> 95.49, gli altri due fermi
```

I gradini cadono a 0x21, 0x1d e 0x1b e le loro taglie non hanno una relazione
visibile fra loro: qui sono tabellati, non derivati, e vale lo stesso **SALAME**
della sezione sopra.

### Il residuo del gruppo 2 non e' fatto di valori sbagliati

Chiusa la base, il segmento peggiore dello sweep e' cold12 (99.16%) con 163
valori sbagliati, di cui 127 sono letture: `OBJ.RD 0x0308` e `0x030c`, la
finestra delle statistiche. **Non sono sbagliate.** Estratte le due sequenze
di valori latchati e confrontate elemento per elemento, vendor e port danno
125 valori su 125 identici. Quei 127 sono il conto di `classify()` dopo uno
spostamento: da un certo punto in poi `difflib` appaia il latch *k* del vendor
col latch *k+1* del port, stessa chiave e valore diverso, e ogni latch a valle
ne produce due. **Una** divergenza strutturale, non centoventisette valori.

La divergenza e' il blocco E delle soglie CRS. Ridotto il segmento alla sola
sequenza latch (`L`) / blocco CRS (`C`):

```
vendor  C C L L L L L L L L L L L L L L L L L L L L L L L L L L C L L …
port    C C L C L L L L L L L L L L L L L L L L L L L L L L L L L L L …
```

Il port lo emette al primo completamento di campione dopo il bring-up -- e'
l'arm di `k == 1` in `b43_phy_ac_watchdog()` -- il vendor al ventiseiesimo.
Sedici op fuori posto.

#### Quanto vale davvero, sui 43 segmenti

Contati i blocchi CRS (`PHY.MOD 0x0321` sotto maschera `0x00ff`) e la loro
posizione in latch su ogni segmento a freddo:

| forma | segmenti |
| --- | --- |
| latch 0, valore funzione della sola larghezza (`3a`/`3c`/`3d`) | 43 su 43 |
| un blocco al latch 1 | 35 su 43 |
| un blocco al latch 61-62 | tutti e 19 quelli che ci arrivano |
| blocchi irregolari (latch 12, 21, 26, 40, 45, 50, 53, 70) | 6 segmenti |

Il blocco tardivo e' il **bss-up**: su cold12 sta a 154 ms dall'evento
`BSS_UP` della timeline, compare su tutti e soli i segmenti DFS che chiudono
il CAC -- che dura 60 s, da cui il latch ~61 -- e **non** e' un periodico da
60 s, perche' nessuno dei segmenti che arrivano a 125 giri ne ha uno al giro
121. Quello il port lo emette gia', da `b43_phy_ac_bss_up()`.

Resta il blocco di mezzo, e il port lo mette al latch 1. Ci azzecca su 35
segmenti su 43. Sbaglia su quattro -- ch112 (latch 26), ch116 (53), ch132 e
ch136 (il vendor non ne ha nessuno, due blocchi in tutto) -- piu' due dei sei
radar-meteo. Il costo e' sedici op per segmento, su sei segmenti.

#### Cosa non lo spiega

Quattro strade provate e chiuse, per non rifarle:

- **non e' un periodico** e non e' `pwork_60sec`: vedi il controllo al giro
  121 sopra, e il commento di `b43_phy_ac_op_pwork_60sec()`, la cui
  giustificazione vecchia ("due scritture su 76 dei 78 segmenti") era misurata
  sul set precedente;
- **non e' il campione**, e questa e' la prova piu' netta. Con la scala di
  `b43_phy_ac_crs_note_noise()` il valore di cold12 dipende solo dal campione
  sopra o sotto 1526: i suoi latch 10 e 21 leggono 1522 e 1532, quindi il
  valore calcolato passerebbe a `0x34` e tornerebbe a `0x39`, e il vendor su
  cold12 `0x34` non lo scrive mai. Nessuna regola "emetti quando il valore
  cambia" sopravvive a questo;
- **non e' un minimo o una soglia attraversata**: il campione che precede il
  blocco di cold12 e' 1567, e sei campioni precedenti sono piu' bassi;
- **non e' `0x008c`**, il puntatore letto in testa a ogni latch: e' costante
  dentro il segmento (cambia col canale, `0xd06c` su ch108, `0xd070` su ch112)
  e non si muove mai dove il blocco esce;
- **non e' il canale**: ch100, ch104, ch108, ch140 e ch144 ce l'hanno al latch
  1, ch112, ch116, ch128, ch132 e ch136 no, e a 40 MHz ch124 ce l'ha e ch116
  no.

Finche' non c'e' un testimone, l'arm a `k == 1` resta: sbagliarlo su sei
segmenti costa meno che toglierlo e sbagliarlo su trentacinque.

## Il coefficiente b della RX IQ: non e' l'arrotondamento

Il port riproduce esattamente gli accumulatori e il coefficiente `a` -- `a`
combacia su tutti i punti misurati -- e sbaglia `b` di un LSB su parte dei
casi. La formula e' `b = sqrt(qq*2^20/ii - a^2) - 2^10` e l'ultimo passo e'
l'arrotondamento della radice, quindi la tentazione e' cercarlo la'.

**Non e' la' e la prova e' negativa.** Venti punti, presi da sette segmenti a
freddo del d6220 e cinque di agcombo, con la parte frazionaria della radice
esatta e la scelta del vendor:

| frazione | vendor | | frazione | vendor |
| --- | --- | --- | --- | --- |
| 0.0118 | per difetto | | 0.4849 | **per eccesso** |
| 0.0337 | per difetto | | 0.4883 | **per difetto** |
| 0.1403 | per difetto | | 0.5179 | **per difetto** |
| 0.1594 | per difetto | | 0.6530 | per eccesso |
| 0.1663 | per difetto | | 0.7125 | per eccesso |
| 0.1802 | **per eccesso** | | 0.7295 | per eccesso |
| 0.3391 | per difetto | | 0.7926 | per eccesso |
| 0.4297 | **per eccesso** | | 0.8086 | per eccesso |
| 0.4361 | **per eccesso** | | 0.8929 | per eccesso |
| 0.4836 | **per eccesso** | | 0.9986 | per eccesso |

Nessuna soglia separa queste venti: 0.4849 va per eccesso e 0.4883 per
difetto. La soglia di mezzo LSB, che e' quella che il driver usa, ne prende
quattordici su venti, e **tutte le eccezioni stanno entro 0.1 dalla soglia**
tranne 0.1802. Quindi l'errore e' nel valore sotto radice -- circa lo 0.02%,
abbastanza da far ribaltare i casi al limite -- e non nel modo di
arrotondarlo. Spostare la soglia a 0.4 ne prenderebbe diciassette su venti, ma
sarebbe un fit su questi dati e nasconderebbe l'errore vero.

Ricerca fatta e chiusa: nessuna combinazione dei sei peek per catena -- tutte
le 63 sottoinsiemi -- da' contemporaneamente il valore di `a` e quello di `b`
del vendor, e nessuna variante di `v` (divisione troncata, `a` non quantizzato,
`a^2` non sottratto) porta la radice al valore che serve. Il difetto sta piu' a
monte del solve.

Quindi la cella entra in `VAL_TOLLERANZA` con 4 LSB su 10 bit: gli scarti
osservati sono di 1 LSB tranne uno di 3 su agcombo. E' una calibrazione, e un
LSB su dieci bit di reiezione d'immagine e' una radio infinitesimamente meno
buona, non un difetto funzionale -- il driver deve ancora funzionare.

**La tolleranza vale per il gate posizionale e non per il punteggio**, ed e'
voluto: `cmp_skip.py` continua a contare quelle op fra i valori sbagliati, cosi'
il residuo resta visibile nei valori sbagliati di `cold01` invece di
sparire, mentre la contiguita' non si ferma su un LSB. Il posizionale passa da
`@24865` a `@25157`, e la divergenza dopo e' `OBJ.WR 0x0300`, la finestra
statistiche in shared memory.

## Il residuo sul coefficiente b della RX IQ: non e' la radice

L'ipotesi naturale, viste le due divergenze da un LSB, e' che la radice del blob
sia approssimata a tabelle invece che esatta. **Verificata e negativa**, e da
due lati indipendenti.

**Il primo: l'implementazione di Broadcom.** `brcmsmac` porta lo stesso
algoritmo in `wlc_phy_calc_rx_iq_comp_nphy()`, e la radice la' e' `int_sqrt`
liscia: un floor, nessuna tabella, nessuna interpolazione, e nemmeno
l'arrotondamento che il port fa. La libreria `phy_qmath.c` non ha una radice
affatto. Non c'e' una tabella da cercare.

**Il secondo: la soglia non regge.** Spostando la soglia di arrotondamento
della radice, con tutto il resto uguale:

| soglia | esatti | bassi di 1 | alti di 1 |
| --- | --- | --- | --- |
| 0.40 | 96 | 38 | 10 |
| 0.50 (attuale) | 84 | 54 | 6 |
| 0.76 | 72 | 72 | 0 |
| 0.98 | 46 | 94 | 0 |

Il massimo e' 96 su 148. Una soglia sbagliata darebbe un errore di **un segno
solo**, e invece qui scambia i bassi con gli alti: il difetto sta a monte della
radice.

### Il residuo vero, misurato

148 punti, cioe' ogni scrittura di `0x?a1` appaiata ai due round di
accumulatori che la precedono, sui sette segmenti a freddo che eseguono la fase
e sui ventisei `up` a caldo:

| errore su b | punti |
| --- | --- |
| 0 | 84 |
| −1 | 54 |
| +1 | 6 |
| −6, −13 | 4 (i due core a 80 MHz, contati due volte) |

Quindi il driver **non** e' "bit-esatto sull'attach e un LSB a caldo", come
diceva: e' esatto sul 57% dei punti, 20 su 28 a freddo e 64 su 120 a caldo, con
un bias verso il basso. Il commento nel sorgente e il testo del
`b43_phy_ac_todo()` sono corretti di conseguenza.

Su `cold01` core 0 servono **+156 sotto la radice su 1.219.765**, 1,3 parti su
10.000; su `cold02` core 0 servono **+37**, 3 parti su 100.000. Deltà diverse
di un fattore quattro, quindi non e' un offset costante.

### Era una media, ma sui coefficienti

La composizione dei round e' il punto, e non nel modo che sembrava. Mediare gli
**accumulatori** invece di sommarli non cambia nulla -- il rapporto `qq/ii` e'
invariante di scala e la troncatura vale una parte su 33 milioni: 84 esatti in
entrambi i casi. Mediare i **coefficienti**, cioe' risolvere i due round uno
per uno e arrotondare `b` a intero prima di fare la media, cambia tutto:

| gruppo | punti | somma accumulatori | media dei b |
| --- | --- | --- | --- |
| **core 0** | **74** | **40** | **68 (92%)** |
| **core 1** | **74** | **44** | **36** |

**Rimisurato sul set corrente, il divario si chiude quasi del tutto.** Stessi
due modelli, stessi 148 punti ma dei 43 segmenti a freddo e dei 44 `up`:

| gruppo | punti | somma accumulatori | media dei b |
| --- | --- | --- | --- |
| core 0 | 74 | 68 | **71** |
| core 1 | 74 | **54** | 53 |

La media resta avanti sul core 0, ma di tre punti invece che di ventotto, e sul
core 1 i due modelli sono appaiati. Sul campione largo la scelta fra i due non
e' piu' netta come sembrava: prima di trattarla come chiusa va capito perche'
il set precedente la mostrasse cosi' marcata -- il sospetto e' che dipenda da
quali segmenti eseguono la fase, non dal modello. La distanza fra i `b` dei due
round va da 0 a 7, quindi la media non e' un altro nome per il massimo.

Due cose provate e cassate. Tenere la precisione frazionaria nella radice per
round e mediare dopo riporta al risultato della somma -- con 4 o 5 bit
frazionari si torna a 84 -- quindi e' **l'arrotondamento a intero prima della
media** a fare la differenza. E l'insieme dei round e' quello giusto: mediare
su tre, quattro, cinque o sei, o su una coppia piu' arretrata, peggiora
entrambi i core (18/74 e 22/74 su tre round).

Il coefficiente `a` sul set precedente veniva invece dalla somma, 142 punti su
148 contro 116, e quell'asimmetria fra i due coefficienti era la cosa strana da
spiegare. **Sul set corrente si inverte e sparisce**: la media da' 140 su 148
e la somma 124, quindi la media vince su `a` come su `b` e non c'e' piu' nessuna
asimmetria. Era un effetto del campione.

### agcombo scioglie il dubbio: non e' una regola per core

Sul d6220 il core 1 non era descritto da nessuno dei due modelli, e le due
letture possibili erano "il core di indice 1" o "l'ultimo core abilitato" --
che su una board a due catene sono la stessa cosa. **agcombo ha tre catene e le
separa**, e la calibrazione e' la stessa.

Senza i segmenti a 80 MHz:

| board | core | somma | media |
| --- | --- | --- | --- |
| agcombo | 0 | 6/12 | **12/12** |
| agcombo | 1 | 4/12 | **10/12** |
| agcombo | 2 | 8/12 | **12/12** |
| d6220 | 0 | 40/72 | **68/72** |
| d6220 | 1 | **44/72** | 36/72 |

Rifatta sul set corrente -- agcombo freddo e caldo, 42 punti -- la separazione
fra le catene non si vede piu': core 0 13 e 13, core 1 12 contro 11, core 2 11
contro 12, cioe' i due modelli pari su tutte e tre. L'argomento "non e' una
regola per core" regge ancora, ma ora perche' **nessuno** dei due modelli e'
per core, non perche' la media vinca ovunque.

Su agcombo **la media vince su tutte e tre le catene, l'ultima compresa**:
34 su 36. Quindi non e' "il core 1" e non e' "l'ultimo core": la regola e'
uniforme, e il residuo del core 1 del d6220 e' una anomalia di quella
combinazione, non una struttura. Insieme, senza bw80, sono 138 punti su 180 con
la media contro 102 con la somma.

La firma dell'errore lo conferma. Il core 0 del d6220 con la media fa 68 esatti
e 4 bassi di uno, e il core 1 di agcombo 10 esatti e 2 alti di uno: residui
piccoli e di un segno. Il core 1 del d6220 fa 36 esatti, 16 bassi e 20 alti,
cioe' e' **centrato ma disperso** -- non un bias da correggere, un rumore che
un altro modello dovrebbe spiegare.

Percio' la media e' applicata a tutti i core. Il prezzo e' che su `cold01` il
core 1 passa da esatto a un LSB alto, `0x38` contro `0x37`, mentre il core 0
passa da sbagliato a esatto: il posizionale del segmento di riferimento avanza
comunque, ma di due op sole, da `@24865` a `@24867`: le altre 290 fino a
`@25157` sono la tolleranza di `VAL_TOLLERANZA`, non questa modifica -- un
errore di attribuzione che era stato scritto qui e va corretto. Il valore vero
della media sta altrove: i tre gate a caldo salgono di 0.02 e sui 26
segmenti a freddo il bilancio e' +0.01 su uno e -0.01 su due.

### Gli 80 MHz: il port fa due passate di misura dove il vendor ne fa sei

Sui dieci eventi a 80 MHz nessuno dei due modelli prendeva un punto, con errori
`-13`, `-6`, `-3`, `+3`, `+4`. La causa e' trovata, e non e' l'aritmetica.

**Le letture degli accumulatori non sono tutte passate di misura.** Le prime
appartengono alla ricerca di guadagno in loopback, e si riconoscono perche'
sono le uniche precedute da un incremento di PHY `0x0b22`
(`gainctrl_final_apply`, che solo `loopback_gain_search` chiama). Contandole
cosi':

| segmento | letture | ricerca in loopback | passate di misura |
| --- | --- | --- | --- |
| d6220 ch36 bw20 | 6 | 3 | 3 |
| d6220 ch36 bw80 | 9 | 3 | **6** |
| agcombo ch36 bw20 | 6 | 4 | 2 |
| agcombo ch36 bw80 | 10 | 3 | **7** |

**Il port ne fa due a ogni larghezza**: su `cold24` emette 5 letture contro le 9
del vendor. Le quattro passate mancanti sono anche una fetta delle op mancanti
di quel segmento, e la tabella del tono `0x000e` lo conferma da un'altra
direzione -- 11 caricamenti nel vendor a 80 MHz contro gli 8 a 20.

E la media su piu' di due passate e' un **arrotondamento per eccesso**. La
proprietà che lo rende verificabile senza rischi: su due valori il ceiling e il
round-half-up **coincidono**, quindi a 20 e 40 MHz non cambia nulla, 69 punti su
90 in entrambi i casi.

| modello | 20/40 MHz | 80 MHz |
| --- | --- | --- |
| ultime 2 passate, round (attuale) | 69/90 | 0/5 |
| ultime 2 passate, ceil | 69/90 | 0/5 |
| tutte le passate di misura, round | 26/90 | 2/5 |
| **tutte le passate di misura, ceil** | 35/90 | **4/5** |

A 80 MHz passa da 0 a 4 su 5 -- `cold24` entrambi i core esatti, `cold25` due
core su tre -- e l'unico residuo e' `cold25` core 0 a un LSB alto.

### I sei toni sono lo stesso periodo a passi diversi

La struttura del blocco e' `gain tono acc` finche' la ricerca converge, poi
`tono acc` per ogni passata di misura, poi la scrittura dei coefficienti. **Il
totale e' 6 fino a 40 MHz su entrambe le board e su ogni segmento**, e a 80 MHz
e' 9 sul d6220 e 10 su agcombo.

E i toni non sono trascrizioni nuove. Il periodo e' sempre lo stesso 20 voci, e
ogni caricamento lo campiona a un passo:

| passata | passo |
| --- | --- |
| ricerca in loopback, tutti i round | +1 |
| 1ª di misura | +1 |
| 2ª | −1 |
| 3ª | +3 |
| 4ª | −3 |
| 5ª | +4 |
| 6ª | −4 |

Verificato sulle 160 voci di ognuno degli undici caricamenti di `cold24`. I due
toni che il driver portava trascritti -- il "secondo" e il "terzo", con il
commento "il primo con le posizioni 1..19 in ordine inverso" -- sono i passi
+1 e −1 dello stesso periodo, e le quattro tabelle in piu' che servivano a 80
MHz non vanno trascritte: si generano. Il passo e' la frequenza del tono in
unita' di rate/20, quindi la sequenza e' un rastrello: una coppia sopra e sotto
la portante fino a 40 MHz, tre coppie a 80, dove la banda da coprire e' quattro
volte.

### Il risultato

Le due modifiche insieme -- sei passate a 80 MHz, e la media per eccesso su
tutte le passate di misura con il flag che le separa dalla ricerca:

- le letture degli accumulatori su `cold24` passano da 5 a **9 contro 9**;
- i coefficienti a 80 MHz diventano **esatti su entrambi i core**, `0x4d` e
  `0x39` contro i `0x47` e `0x2c` di prima;
- `cold24` passa da 89.20% a **94.01%**, con le op mancanti da 3034 a **1152**
  e i valori sbagliati da 627 a 619.

Nessun altro dei segmenti di allora si muoveva, i tre gate a caldo non si muovevano e il
periodico resta `MATCH`: la media per eccesso su due valori coincide con il
round-half-up, quindi fino a 40 MHz e' la stessa aritmetica.

Un inciampo da ricordare: `-((-b) / n)` e' un ceiling in Python e un **floor**
in C, dove la divisione tronca verso zero. La prima stesura lo usava e dava
`0x4c` invece di `0x4d`; la forma giusta e' `(b + n - 1) / n`.

Il muro di `cold24` resta a `@13470`, quindi le 1152 op mancanti che restano
sono altrove e prima di quel punto.

### Dove pesa: la forma della divisione

`brcmsmac` non divide per `ii`. Normalizza `qq` al bit 30 -- `qq << (31 - nbits(qq))`
-- e sposta `ii` di `nbits(qq) - 11`, quindi **tronca il divisore**: con gli
accumulatori di `cold01` core 0 il divisore diventa 1008 invece di 1008.4, lo
0.04%, che e' piu' del difetto da spiegare.

Confronto sui 148 punti, con `esatto` = la divisione a 64 bit che il port fa
oggi e `norm=N` = la forma normalizzata al bit N:

| modello | freddo | caldo | totale |
| --- | --- | --- | --- |
| esatto, arrotondato (attuale) | 20/28 | 64/120 | 84 |
| esatto, floor | 14/28 | 32/120 | 46 |
| norm=31, arrotondato (forma brcmsmac) | 16/28 | 80/120 | **96** |
| norm=31, floor (brcmsmac letterale) | 18/28 | 54/120 | 72 |
| norm=32, arrotondato | 22/28 | 74/120 | **96** |

Le due migliori pareggiano a 96 e **nessuna domina**: `norm=31` e' meglio a
caldo e peggio a freddo, `norm=32` il contrario. Il codice non e' stato
cambiato per questo -- non c'e' un vincitore -- ma la direzione e' segnata: il
quoziente si forma con una normalizzazione, e quale sia va deciso con un
oracolo che mostri il valore intermedio, non con i coefficienti finali.

## La tolleranza su b: la soglia e' la misura del residuo

`VAL_TOLLERANZA` in `test/unit/compare.py` confronta `PHY 0x?a1` con una soglia
invece che per uguaglianza, e la soglia era `+-4` LSB. Era **quattro volte il
residuo vero**, e questo e' costato un giro: la differenza fra sommare gli
accumulatori e mediare i coefficienti -- cioe' fra il modello sbagliato e
quello giusto -- vale 1 LSB a 20 e 40 MHz, e a `+-4` il gate posizionale la
ingoiava. E' venuta fuori solo perche' a 80 MHz sforava, 6 e 13 LSB.

La regola che ne segue: **una tolleranza va dimensionata sul residuo del
modello che si crede corretto, non sul residuo che si sopporta.** Se e' piu'
larga smette di coprire l'ultimo bit e diventa una fascia dove un errore
strutturale si nasconde.

Misura rifatta sul set corrente con `reverse-tools/rxiq_points.py` -- estrazione
sui 43 segmenti a freddo e sui 44 `up`, **148 punti di soluzione**, modello
della media per tono contro il valore del vendor:

| gruppo | -1 | 0 | +1 |
| --- | --- | --- | --- |
| freddo bw20 | 5 | 37 | 2 |
| freddo bw40 | 1 | 18 | 1 |
| freddo bw80 | 3 | 7 | 0 |
| caldo bw20 | 7 | 35 | 0 |
| caldo bw40 | 1 | 17 | 2 |
| caldo bw80 | 2 | 10 | 0 |
| **totale** | **19** | **124** | **5** |

**Massimo `|residuo|` = 1**, quindi la soglia resta `+-1`. E **simmetrica**, e
sul set corrente lo e' senza bisogno del caldo: entrambi i segni compaiono gia'
a freddo. Sul set precedente a freddo si vedeva il solo `+1`, ed e' una
differenza di campione, non di modello.

Dei 24 residui non nulli, **21 sono sulla catena 1 e 3 sulla catena 0**: il
debito e' quasi tutto la', coerente con il residuo del core 1 del d6220 che
agcombo ha mostrato non essere una regola per catena. La catena 2 non compare
in nessuna cattura del d6220, quindi la sua voce nella lista non e' mai stata
esercitata ed e' dichiarata senza copertura.

Stringere da `+-4` a `+-1` **non muove nessun gate**: `cold01` resta a
`@25157`, ch36 bw80 a `@13470`, i tre a caldo (78.05, 80.91 e 78.79%), il
periodico a `MATCH`. Cioe' i tre LSB in piu' erano pura franchigia. Quei quattro
numeri sono del set precedente e non sono stati rifatti.

Sul lato radio la franchigia residua e' innocua: un LSB e' 1/1024 del guadagno
nominale, 0.0085 dB, che impone un tetto alla reiezione d'immagine attorno ai
66 dB contro i 30-45 dB che l'hardware limita comunque. Il rischio della
tolleranza non e' mai stato in aria, era di nascondere un modello sbagliato.

## Il latch del rumore e' un contesto a parte, non la coda dello switch

Il port emette il latch della finestra statistiche e il blocco E delle soglie
CRS in coda al channel setup, dietro la cella `0x0026 = 0xffff`. Posizionalmente
torna su 29 segmenti a freddo su 43, ed e' il motivo per cui sta li'. Ma non e'
la coda dello switch, e la CPU della traccia lo dimostra.

**La lettura `OBJ.RD 0x008c` che apre il latch sta su `cpu1` su tutte e 4758 le
sue occorrenze** dei tre sweep -- 2665 sul d6220 a freddo, 1590 a caldo, 503
sull'agcombo -- senza una sola eccezione. Tutto quel che le sta intorno si
distribuisce invece su entrambe le CPU:

| blocco | cpu0 | cpu1 |
| --- | --- | --- |
| latch `0x008c` | **0** | 2665 |
| clear `0x0308` | 1248 | 1422 |
| cambio di modo `0x0520` | 1337 | 1405 |
| spazzata `0x0768` | 4130 | 4394 |
| tabella `0x000c` del setup | 6514 | 8875 |
| poll del CAC | 5857 | 7927 |

Due controlli che chiudono la questione:

- su **21 dei 43 segmenti** la cella `0xffff` sta su `cpu0` e il latch che la
  segue di **una op** sta su `cpu1`. Una continuazione sincrona non cambia CPU;
- delle scritture CRS, le **63 che seguono un latch** entro trenta op sono tutte
  su `cpu1`, mentre le **86 che non lo seguono** -- quelle di `chanspec_tail()`,
  dentro il setup -- si dividono 28 e 58 come il resto del setup.

Un blocco inchiodato a una CPU mentre il codice che lo precede e quello che lo
segue si spostano e' un contesto di interruzione, non un thread. Nel vendor e'
il completamento del campione di rumore: la coda del setup lo **arma**, e
`wlc_phy_noise_sample_intr()` lo consuma quando il campione e' pronto,
calcolando li' la soglia CRS minima. Il blocco E non e' quindi codice del
channel setup: e' la callback.

**Cosa spiega.** Nei tre millisecondi dopo la cella `0xffff` la cattura mostra
tre ordini degli stessi ingredienti, e senza questo modello nessuno dei tre e'
derivabile da uno stato che il driver legga:

| ordine | segmenti |
| --- | --- |
| latch, blocco E, poll | 29 |
| poll, latch, blocco E | ch36 a 80 MHz |
| giro di watchdog intero, poi latch e blocco E | ch36 a 20 MHz |

Col completamento come evento sono tutti e tre lo stesso codice: il setto arma,
il work periodico gira per conto suo -- il primo cambio di modo sta a +1.004 s
dalla cella su 42 segmenti su 43, e a +0.002 s sul solo ch36 a 20 MHz, e prima
della cella non ce n'e' nessuno in nessun segmento -- e la callback cade dove
l'hardware la fa cadere.

**Cosa comporta.** Il punto di rientro va aggiunto come gli altri callback che
l'harness gia' consegna (`WD`, `POLL`, `TPL`, `BSS_UP`): `timeline.py` sa
riconoscerlo, perche' il latch e' l'unico blocco pinnato su `cpu1`. Il driver
perde il ramo in coda a `b43_phy_ac_bss_up()` e guadagna una funzione che il
core chiama dal suo percorso di interruzione. Finche' non e' fatto, `cold01`
non e' un segmento di riferimento sensato per quella finestra, e il suo muro a
`@25619` misura questo e non altro.

## Da dove ripartire

Stato: gate a freddo su `cold01` a **28552/28574 = 99.92%** con **zero valori
sbagliati**, 22 op mancanti e zero op del port di troppo. Le 22 sono tutte
del core o di bcma (OTP/SROMCTL/`CAL.INIT`, `0x0184`, `0x00b0`, `0x05dc`,
`TPL.RAMW 0x48`, le coppie `0x05d6/0x05d8`, `OBJ.RD 0x0/0x2`, `MAC.MHF.RD`,
`OBJ.RD 0x40`): dentro il perimetro `compare.py` stampa `MATCH`. Gate
periodico a `MATCH`. Il salto da 98.80% viene per tre quarti dall'offload
della probe response dichiarato fuori scopo, non da lavoro sul port: vedi il
TODO post-WIP piu' sotto.

### Lo sweep a freddo intero, sullo stesso albero

**Questa tabella e' una fotografia, non lo stato di oggi.** Le colonne "di
troppo" e "mancanti" sono scese di un ordine di grandezza da quando e' stata
presa -- `cold02` stava a 98.09% e sta a 99.90%, `cold38` a 98.16% e sta a
99.40% -- e le due osservazioni che la seguono, sulle famiglie e sui sei
radar-meteo, si reggono su quei numeri. I numeri correnti, con la stessa
procedura, sono nel README: minimo 99.16%, mediana 99.44%, massimo 99.90%. Qui
si tiene la fotografia perche' il ragionamento che porta e' quello che ha
sciolto la separazione a 5250 MHz, non perche' i numeri valgano ancora.

Tutti e 43 i segmenti, non un campione, rimisurati sul set corrente. Le colonne
sono la riga `grezzo` del gate, le sue tre voci e il verdetto posizionale:

| segmento | grezzo | val. sbagliato | mancanti | di troppo | posizionale |
| --- | --- | --- | --- | --- | --- |
| cold33 ch124 bw40 | 99.83% | 0 | 29 | 0 | `MATCH` |
| cold32 ch116 bw40 | 99.72% | 0 | 29 | 18 | `@12587` |
| cold14 ch120 bw20 | 99.50% | 18 | 29 | 18 | `@10752` |
| cold15 ch124 bw20 | 99.50% | 18 | 29 | 18 | `@10761` |
| cold41 ch116 bw80 | 99.49% | 28 | 29 | 0 | `@10770` |
| cold16 ch128 bw20 | 99.35% | 18 | 51 | 22 | `@10754` |
| cold39 ch52 bw80 | 98.40% | 95 | 289 | 302 | `@11267` |
| cold38 ch36 bw80 | 98.16% | 95 | 295 | 266 | `@13919` |
| cold02 ch40 bw20 | 98.09% | 0 | 295 | 274 | `@25760` |
| cold04 ch48 bw20 | 98.04% | 40 | 252 | 251 | `@25754` |
| cold03 ch44 bw20 | 97.94% | 19 | 282 | 295 | `@25799` |
| cold29 ch60 bw40 | 97.85% | 64 | 390 | 369 | `@10762` |
| cold28 ch52 bw40 | 97.77% | 63 | 395 | 390 | `@15693` |
| cold27 ch44 bw40 | 97.45% | 92 | 290 | 303 | `@11388` |
| cold31 ch108 bw40 | 97.43% | 229 | 312 | 283 | `@16336` |
| cold40 ch100 bw80 | 97.39% | 262 | 395 | 366 | `@11260` |
| cold30 ch100 bw40 | 97.21% | 275 | 312 | 283 | `@10762` |
| cold35 ch140 bw40 | 96.99% | 237 | 404 | 357 | `@12605` |
| cold42 ch132 bw80 | 96.71% | 429 | 362 | 396 | `@11262` |
| cold34 ch132 bw40 | 96.35% | 277 | 481 | 466 | `@12588` |
| cold08 ch64 bw20 | 96.21% | 2 | 930 | 901 | `@16935` |
| cold20 ch144 bw20 | 96.20% | 218 | 380 | 351 | `@11359` |
| cold07 ch60 bw20 | 96.17% | 0 | 948 | 901 | `@16729` |
| cold05 ch52 bw20 | 95.79% | 91 | 894 | 977 | `@16931` |
| cold10 ch104 bw20 | 95.61% | 232 | 848 | 819 | `@10762` |
| cold43 ch149 bw80 | 95.48% | 488 | 457 | 435 | `@6574` |
| cold26 ch36 bw40 | 95.45% | 86 | 584 | 639 | `@11400` |
| cold06 ch56 bw20 | 95.41% | 103 | 955 | 1088 | `@12601` |
| cold19 ch140 bw20 | 95.31% | 234 | 926 | 897 | `@10762` |
| cold23 ch157 bw20 | 95.22% | 429 | 288 | 287 | `@11396` |
| cold25 ch165 bw20 | 95.18% | 437 | 282 | 295 | `@11399` |
| cold13 ch116 bw20 | 95.09% | 318 | 815 | 926 | `@10755` |
| cold18 ch136 bw20 | 95.01% | 342 | 807 | 944 | `@10762` |
| cold12 ch112 bw20 | 94.88% | 232 | 1026 | 997 | `@10762` |
| cold22 ch153 bw20 | 94.87% | 417 | 380 | 351 | `@11398` |
| cold09 ch100 bw20 | 94.82% | 312 | 888 | 995 | `@12603` |
| cold21 ch149 bw20 | 94.75% | 427 | 380 | 351 | `@11389` |
| cold01 ch36 bw20 | 94.64% | 115 | 623 | 796 | `@25619` |
| cold17 ch132 bw20 | 94.58% | 307 | 935 | 1092 | `@10759` |
| cold24 ch161 bw20 | 94.38% | 419 | 458 | 437 | `@11392` |
| cold11 ch108 bw20 | 94.23% | 330 | 1023 | 1162 | `@10755` |
| cold36 ch149 bw40 | 94.26% | 448 | 464 | 435 | `@11399` |
| cold37 ch157 bw40 | 94.21% | 448 | 464 | 435 | `@11398` |

**Le due famiglie di prima non ci sono piu'.** Sul set precedente i segmenti si
dividevano in una famiglia bassa quasi perfetta e in diciannove segmenti sopra
i 5250 tutti uguali fra loro, con la voce "di troppo" a **esattamente 1808** su
diciotto di essi -- ed era quella costanza a dire che la causa era una sola. Qui
la voce di troppo va da 0 a 1162 senza ripetersi, e il punteggio sta in una
fascia sola, da 94.21% a 99.83%, con mediana intorno al 96%.

**I sei migliori sono i sei radar-meteo.** ch120, ch124 e ch128 a 20 MHz,
ch116 e ch124 a 40, ch116 a 80 stanno tutti sopra il 99.3%, e `cold33` e'
l'unico `MATCH` posizionale dello sweep. Sono gli unici segmenti ripresi ancora
senza il CAC completato, cioe' quelli in cui il vendor **non** esegue le
calibrazioni: dove non le esegue il port lo segue quasi op per op. Quindi
quasi tutto il residuo del port sta nelle calibrazioni che la ricattura ha
acceso, non nel channel programming.

**Il conto per canale non e' piu' il conto per famiglia.** `cold01`, che e' il
gate di riferimento, e' fra i peggiori a 94.64%, e i suoi vicini di banda
`cold02`-`cold04` stanno sopra il 97.9%: la differenza e' nel numero di giri di
watchdog che il segmento contiene, non nella banda. Prima di leggere un
punteggio come debito di un canale va normalizzato sul numero di giri.

La voce "di troppo" **non** e' debito per-canale, e su `cold04` era misurata: la
differenza fra i due flussi come multiinsieme, sulla finestra di confronto, era
di 62 op sole -- cinque giri in piu' del latch `0x0308-0x0314`, `OBJ.RD 0x008c`
compreso -- contro le 271 che il conteggio posizionale attribuiva. Sul set
corrente `cold04` sta a 251 op di troppo posizionali; la misura a multiinsieme
non e' stata rifatta, perche' vuole gli stessi filtri di `compare.py` (op solo
vendor, perimetro, finestra di allineamento) e riprodurla a mano da' un numero
che non e' lo stesso. Le altre ~209 sono le stesse op in un altro posto,
sfasate da quei cinque giri. La causa sta sotto.

### Il latch della finestra di rumore non cade su ogni tick

Misurato su `cold04`, dove e' la causa delle 271 op di troppo, e poi contato su
tutti e 87 i segmenti. I tick si delimitano sul mode change `PHY.MOD 0x0520`;
il tick 0 non conta, e' la coda della calibrazione e porta le ricariche che
precedono la fase.

Su `cold04`, per tick:

| testimone | cosa conta | vendor | port |
| --- | --- | --- | --- |
| `PHY.MOD 0x0520` | i tick della fase | 18 | 18 |
| `OBJ.WR 0x0018`/`0x001a` | le ricariche del beacon | 9 + 7 | 9 + 7 |
| `OBJ.RD 0x0772` | le passate della spazzata dei contatori | 5 per tick | 5 per tick |
| `OBJ.RD 0x0308` | i giri del latch della finestra | **14** | **18** |

Tick, ricariche e spazzata tornano; la sola voce che non torna e' il latch, che
il loop emette su ogni tick tranne il primo e che il vendor salta sui tick 1, 5,
9, 13 e 15. Sono 14 op per giro, cioe' le 62 op di troppo del multiinsieme.

**Non e' un poll, e la traccia lo esclude.** La finestra `0x0308-0x0314` non
viene attesa: si legge e si azzera -- sette letture e sei clear -- e non c'e'
nessun valore atteso da raggiungere. Soprattutto, nel tick che salta il latch
non c'e' nessuna lettura in piu' che possa fare da condizione: le ultime op
prima del punto in cui il latch cadrebbe sono `OBJ.RD 0x015a` e `OBJ.RD 0x014e`,
le stesse dei tick che latchano, e `OBJ.RD 0x008c` compare solo quando il latch
c'e' -- e' la prima op del blocco, non un test che lo precede. Un predicato sul
valore letto non e' esprimibile: non c'e' niente da cui dipendere.

**L'invariante, rifatto sugli 87 segmenti del set corrente.** I giri si
delimitano con gli eventi `WD` di `reverse-tools/timeline.py` invece che a mano
sul mode change -- il primo giro non ne ha e va contato lo stesso -- la ricarica
e' l'evento `TPL` dello stesso strumento, e il latch e' `OBJ.RD 0x0314`, la
cella che la spazzata di clear non tocca. Il giro 0 non conta.

| | giri con ricarica | saltano | giri senza ricarica | saltano |
| --- | --- | --- | --- | --- |
| 43 segmenti a freddo | 498 | **108** | 2244 | 4 |
| 44 segmenti `up` a caldo | 195 | **39** | 1435 | 3 |

Il salto sta quindi sui giri con ricarica, ma non **solo** la': sette giri senza
ricarica lo saltano lo stesso. Sono pochi e vanno confrontati con la lista dei
latch persi che `watchdog_turns.py` estrae per conto suo -- quella misura i
colpi che il timer del vendor ha perso -- prima di dire se sono la stessa cosa
o un secondo caso.

**Il caldo non e' piu' un controesempio, e non per il motivo scritto prima.** La
ricarica esiste in due forme, e le distingue il `MAC.MCTRL` di sospensione
subito dopo la scrittura di `BTL0`/`BTL1` -- e' la stessa distinzione che
`beacon_reloads.py` usa per selezionarle.

| | giri con ricarica | forma `susp` | salti sulla `susp` | senza susp | salti |
| --- | --- | --- | --- | --- | --- |
| 43 segmenti a freddo | 498 | 498 | 108 | 0 | -- |
| 44 segmenti `up` a caldo | 195 | 159 | 36 | 36 | 3 |

Sul set precedente il caldo portava 214 ricariche su 216 nella forma senza
sospensione e due sole `susp`, e la conclusione era che la condizione la' non
ci fosse. La ricattura la smentisce: a caldo la forma `susp` oggi e' la
maggioranza, e il tasso di salto e' lo stesso delle due condizioni -- 108 su 498
a freddo, 36 su 159 a caldo, cioe' 22% contro 23%. Freddo e caldo vanno quindi
trattati insieme, e la forma `susp` resta il termine da cui partire.

**Il buffer del beacon non c'entra.** Misurato sul set precedente: la
corrispondenza col `BTL1` era esatta su un segmento e inesistente sull'insieme
-- 26 `BTL1` con latch contro 7 senza, 25 `BTL0` con latch contro 13 senza. Non
e' stata rifatta, ma il tasso di salto uniforme fra le due condizioni non le da'
un motivo per tornare.

**Ristretto alla forma `susp`, il salto alterna dentro la sequenza -- in cinque
segmenti su otto.** Da qui in avanti i conteggi e i nomi dei segmenti sono del
set precedente e non sono stati rifatti; sul set corrente i segmenti che
alternano sono 18 a freddo e 20 a caldo, quindi la casistica va riaperta. Sui tick consecutivi che portano una ricarica il primo
latcha, il secondo no, il terzo si':

| segmento | tick con ricarica (T = latch, F = salto) | alterna |
| --- | --- | --- |
| `cold03` | 1T, 6T 7F, 13T 14F | si |
| `cold04` | 1F, 4T 5F, 8T 9F, 12T 13F, 14T 15F, 16T | si |
| `cold15` | 8T 9F, 15T 16F | si |
| `cold17` | 1T 2F, 4T 5F, 7T 8F, 9T 10F, 11T 12F, 13T 14F, 19T 20T | si |
| `cold18` | 1T 2F, 3T 4F, 5T, 7T, 9T, 12T 13F, 14T 15F, 16T 17F | si |
| `cold01` | 1T, 17T 18T | **no** |
| `cold02` | 1T, 4T 5T, 9T 10T, 14T 15T, 18T 19T | **no** |
| `cold24` | 1T 2T, 4T 5T 6T 7T 8T, 11T 12T, 14T 15T 16T 17T | **no** |

Dentro un segmento la regola non ha eccezioni salvo due, ai bordi: `cold04`
tick 1, il primo tick della fase, e `cold17` tick 20, l'ultimo della scadenza.
Fra segmenti invece si spacca in due, e `cold02` ha esattamente la struttura di
`cold04` -- coppie di tick con ricarica ogni quattro tick, forma `susp`,
campioni nello stesso intervallo 1143-1439 contro 1078-1298 -- e non salta un
solo latch. Non separano: canale (ch44, ch48, ch136, ch36, ch44 contro ch36,
ch40, ch36), larghezza (BW20 e BW40 da un lato, BW20 e BW80 dall'altro),
cadenza delle ricariche, valore del campione, ne' il tempo che resta nel tick
dopo la ricarica (0-3 ms in tutti e 71, sotto la risoluzione della traccia).

**Il controllo sull'indice CRS non e' eseguibile a freddo, ed e' una proprieta'
dello sweep.** L'idea era incrociare i salti col caso `nessun campione` della
regola del ladder CRS. Non si puo': su `cold04` le sedici scritture del banco
`0x0910-0x0913` finiscono a `#139344` e il primo mode change della fase probe e'
a `#139592`, quindi **il banco e' scritto prima che la fase cominci**; i 13
campioni latchati durante la fase non vengono consumati da nessuno, perche' il
segmento finisce con un rmmod. Il `nessun campione` della regola e' un ciclo
intero senza campione, non un tick saltato dentro la fase. Per vedere una
conseguenza servirebbe una cattura in cui la fase probe precede una scrittura
del banco, cioe' due cicli nello stesso caricamento: lo sweep a caldo li ha, ma
la' la condizione non c'e'.

**Dove sta il tick nel port, che e' meta' del motivo per cui la regola non si
trovava.** La fase probe e' emessa da dentro `op_switch_channel`:
`op_switch_channel` -> `set_channel_calibrations()` -> `rxiqcal_finalize()`, e
li' c'e' un loop di 18-21 iterazioni che riemette i pezzi del watchdog. In wl
non esiste nessuna fase probe: quello che la traccia mostra e' il watchdog che
scatta 18-20 volte mentre il tracer era ancora acceso. Cercare la regola del
latch come schedule sull'indice di quel loop e' quindi cercarla su un indice che
il vendor non ha -- ed e' coerente col fatto che l'alternanza torni dentro un
segmento e che le due eccezioni cadano sul primo tick e sull'ultimo, cioe' dove
il loop comincia e finisce.

Il corpo del tick era scritto **tre volte**: in `b43_phy_ac_watchdog()`, nel
corpo del loop e nel tick di chiusura, in due rotazioni diverse. Ora e' uno,
`b43_phy_ac_wd_body(dev, reloads, noise_cal, tail)`, e la rotazione resta al
sito di chiamata: il watchdog a regime mette la fase di campionamento prima, il
loop dopo, il tick di chiusura non la ha. Le op emesse non cambiano -- la
traccia di `cold01` e' identica byte per byte -- e i gate non si muovono:
`28552/28574`, periodico `MATCH`, i tre a caldo a 86.04/89.28/85.93.

**La fase e' inerte in un driver vero.** Il loop e il tick di chiusura girano
solo se il chiamante ha chiesto una scadenza: `probe_ticks` a zero -- e in b43
nessuno lo imposta -- e la fase non emette niente, i giri del watchdog arrivano
da `op_pwork_15sec()` dove devono. Serviva un termine in piu' sul tick di
chiusura, perche' `probe_watchdog_tick[0]` azzerato combacia col tick 0 e senza
guardia un driver vero emetteva un corpo li'. La differenza sul ferro non e'
cosmetica: venti latch-and-clear consecutivi dentro un cambio di canale
azzerano venti volte l'accumulatore del rumore, che e' l'ingresso dell'indice
CRS, in un tempo in cui non ha potuto riempirsi.

**Il tick di chiusura c'era in tutti i segmenti e il port lo emetteva in
meta'.** Trovato guardando cosa segue l'ultimo mode change della fase, che e' la
domanda che nasce dal chiedersi se il loop sia l'ultima cosa di
`rxiqcal_finalize()` -- non lo e': dopo il blocco ci sono ancora ~240 righe.

Rifatto sul set corrente, contando cosa segue l'ultimo `PHY.MOD 0x0520
mask=0xc` del segmento:

| testimone dopo l'ultimo mode change | 43 a freddo | 44 `up` a caldo |
| --- | --- | --- |
| il poll (`OBJ.RD 0x0768`) | 43 su 43 | 44 su 44 |
| il latch (`OBJ.RD 0x0308`) | 38 su 43 | 39 su 44 |
| il measure block (`0x0725`) | 0 su 43 | 1 su 44 |

Il poll e' struttura su tutti e 87 i segmenti e il latch manca su dieci, cinque
per condizione; il measure block dopo l'ultimo mode change sul set corrente
praticamente non c'e' piu', mentre sul set precedente stava su 13 segmenti a
freddo su 26. La condizione che allora spiegava quei 13 -- la scadenza a 19 --
non e' piu' verificabile cosi' e va riaperta: la ricattura ha cambiato dove le
finestre dei segmenti si chiudono. Quel che la misura di allora ha prodotto nel
driver resta valido, ed e' il punto della voce: il port condizionava **tutto**
il corpo di chiusura al measure block, quindi dove la scadenza era diversa da 19
non emetteva niente dove il vendor emette poll e latch. Sui gate a freddo la correzione recupera **1024 op
mancanti** contro 16 di troppo (le 8+8 del latch che `cold17` e `cold18` non
hanno):

| famiglia | grezzo prima | grezzo dopo |
| --- | --- | --- |
| ≤ 5250 MHz | 96.55% - 99.92% | 96.76% - 99.92% |
| > 5250 MHz | 84.66% - 85.78% | 84.92% - 85.84% |

`cold01` non si muove, ed e' il punto: la' la scadenza e' 19, quindi il ramo
sbagliato non veniva mai preso e il difetto era invisibile al gate di
riferimento.

**Un sintomo aperto a caldo, da non confondere con questo.** Sui tre gate a
caldo la correzione da' +0.37, 0 e **-0.35**: su `19-up-ch104-bw20` le op
mancanti non scendono (297 prima e dopo) e quelle di troppo salgono di 80.
Vuol dire che il poll di chiusura del vendor era **gia'** appaiato a qualcosa
che il flow `up` emetteva, e ora il port ne emette due dove il vendor ne ha uno.
Il poll di troppo nel flow `up` e' un difetto preesistente che questa
correzione ha reso visibile, non un effetto suo: i candidati sono le quattro
`wd_stats_poll_opt()` fuori dal watchdog (`phy_ac.c:5654`, `:5661`, `:5725`,
`:5760`).

**Debito che resta.** La forma giusta e' che la fase non stia nell'albero di
chiamata di `switch_channel` per niente: N chiamate del watchdog da sopra --
`pwork` sul ferro, il flow dell'harness in prova. Cosa lo blocca oggi, e adesso
e' misurato: la finestra dell'oracolo del tick a regime **apre a metà ciclo**,
sul `sample_phase` (`#27036`, `MAC.MCTRL` di sospensione e i peek TSSI), e
chiude sul latch. `b43_phy_ac_watchdog()` ha quella rotazione perche' e' stata
scritta per quella finestra, mentre la fase emette `corpo, sample`: N chiamate
del watchdog darebbero `sample, corpo` e ruoterebbero di un gruppo l'intero
flusso della fase, spostando il gate a freddo. La rotazione semanticamente
giusta e' quella della fase -- si legge la finestra e la si azzera subito dopo,
con un secondo di accumulo prima del latch successivo, mentre nella rotazione
dell'oracolo la clear precede di 4 ms il latch dello stesso giro. Quindi il
passo che sblocca e' ritagliare la finestra dell'oracolo sul poll invece che sul
`sample_phase`: e' una scelta di estrazione, non un cambio di cattura, e va
fatta prima di spostare il loop.

**Conseguenza pratica: sul latch non si tocca niente.** Un tick per latch e' la
forma giusta per 25 segmenti a freddo su 43 e per 24 `up` su 44 -- sugli altri
il salto alterna dentro la sequenza, ed e' il censimento qui sopra. La voce
"di troppo" dei cinque segmenti che alternano e' contabilizzata qui invece di
essere letta come debito del port. Un parametro che passi la lista dei tick col
latch alzerebbe il punteggio su cinque segmenti senza modellare niente, e
coprirebbe l'unico posto dove questa differenza si vede.

La domanda aperta e' una sola e va posta cosi': **cosa fa alternare
`cold03/04/15/17/18` e non `cold01/02/24`**, a parita' di forma della ricarica,
di struttura delle sequenze e di intervallo dei campioni. Le due eccezioni ai
bordi -- primo tick della fase e ultimo della scadenza -- vanno spiegate dalla
stessa regola o dichiarate separate.

**Un difetto che la ricattura ha chiuso da se'.** Sui segmenti a caldo del set
precedente `beacon_reloads.py` restituiva `AC_BEACON_RELOADS=0:`, perche' filtra
la sola forma `susp` e la' quella forma quasi non c'era: il port non emetteva
nessuna ricarica dove il vendor ne emetteva 214. Sul set corrente la forma
`susp` a caldo e' la maggioranza -- 159 giri su 195 -- e lo strumento restituisce
liste piene, per esempio `[1, 2, 6, 7]` su `01-up-ch36-bw20` e `[2, 64, 65, 66]`
su `09-up-ch100-bw20`. Il filtro non va quindi allargato alla forma senza
sospensione: quello che resta da verificare e' se le 36 ricariche senza `susp`
del caldo vadano emesse anch'esse, e quanto pesano sui punteggi a caldo.

### Audit: cosa resta di estraneo dentro `switch_channel`, e cosa non ha piu' un chiamante

Fatto col metodo che ha funzionato per la coda: gli stacchi di tempo dentro il
segmento. Su `cold04`, mappa degli eventi:

| t | op | evento | chi lo emette |
| --- | --- | --- | --- |
| 858.59-858.67 | ~150 | preambolo analogico, `afe_arm`, `PMU.RC`, MHF | `phy_ops->switch_analog`, dal core |
| *stacco 2260 ms* | | | |
| 860.93-861.09 | 11384 | MACCTL e GPIO del core, rfkill, `op_init`, `set_channel` | `op_switch_channel` e il core |
| 861.79-865.71 | ~800 | 5 ricariche beacon e i blocchi SHM rate/EDCF, con attese di ~650 ms fra loro | `channel_setup_tail`, `conf_tx`, `tail2` |
| 865.71-869.84 | ~16000 | le calibrazioni post-channel | `set_channel_calibrations` |
| 869.84-889.99 | ~4500 | 18 giri di watchdog piu' la chiusura | fase probe, dentro `rxiqcal_finalize` |
| *stacco 457 ms* | | | |
| 890.45-890.47 | 460 | bss-up | `b43_phy_ac_down()` |

**Dentro `op_switch_channel` non resta niente di estraneo.** La sua regione --
`#109336`-`#122340` -- e' un burst continuo di 11384 op in **156 ms**, e lo
stacco interno piu' grande e' di 19 ms: nessun evento di altri e' interlacciato
la' dentro. Il preambolo analogico e' separato da 2.26 s e sta gia' fuori, nel
callback che il core chiama.

**Il problema e' l'inverso, e piu' grosso.** Quattro blocchi non hanno **nessun
chiamante in `src/`**: li chiama solo `main.c` dell'harness.

| funzione | chiamanti in `src/` | chiamanti nell'harness |
| --- | --- | --- |
| `b43_phy_ac_channel_setup_tail()` | 0 | 1 |
| `b43_phy_ac_channel_setup_tail2()` | 0 | 1 |
| `b43_phy_ac_set_channel_calibrations()` | 0 | 1 |
| `b43_phy_ac_down()` | 0 | 1 |

`bss_up` e' appena stata portata fuori e il suo hook e' una decisione aperta,
ma le altre tre erano cosi' da prima. Vuol dire che **sul ferro le
calibrazioni post-channel non girano affatto**: `op_switch_channel` fa il
channel setup e ritorna, e tutto quello che nella cattura sta fra 861.8 e
890.5 -- code del channel setup, calibrazioni, fase probe, bss-up -- esiste solo
perche' l'harness lo chiama. Il punteggio del gate non lo vede, per costruzione:
l'harness sta in piedi da se'.

Non e' un difetto di posizionamento come la fase probe, e' codice non collegato.
Va deciso hook per hook, e la suite di integrazione -- che compila b43 intero ed
esegue il probe -- e' lo strumento che lo misura, perche' e' l'unica che vede
cosa il driver chiama davvero. Il suo esito attuale, un attach che si chiude in
68 op, e' coerente con questo quadro.

### La coda di `rxiqcal_finalize()`: classificata

Il loop della fase probe **non e' l'ultima cosa** della funzione: dopo di esso
ci sono ancora ~240 righe di codice e 541 op nella cattura. E la coda non e'
un blocco unico: su `cold04` si spezza in due in modo netto.

| blocco | t | op | cosa e' |
| --- | --- | --- | --- |
| A | 889.992-889.993 | 81 | il tick di chiusura del watchdog: poll e latch |
| -- | *gap di 457 ms* | | |
| B | 890.450-890.469 | 460 | un evento solo, 19 ms |

Il blocco B, per sotto-blocchi, con l'attribuzione che danno i marcatori del
port (`AC_FN_MARKERS=1`):

| t | op | port | categoria |
| --- | --- | --- | --- |
| 890.450-890.463 | 47 | `prb_rsp_rate_po`, chainmask, terza passata dei dodici rate | potenza per-rate in shared memory |
| 890.464-890.467 | 306 | `txpwr_target` x2, `afe_gain_regs_reemit` | LUT est_pwr, potenza TX |
| 890.468 (#144562-144615) | 54 | restore TX IQ/LO e coefficienti RX-IQ | **l'unico pezzo che e' davvero un cal finalize** |
| 890.468 (#144616-144626) | 11 | MAC enable/suspend, GPIO `0x0004` clr e `0x0400` set, MACCTL `0x04000400` | frontend MAC/GPIO |
| 890.469 (#144627-144637) | 11 | `afe_arm(AFE_ON, 0, 1)` | **`switch_analog(on)`** |
| 890.469 (#144638-144642) | 5 | `pmu_req(dev, true)` | rilascio PMU |

**La coda contiene un `switch_analog(on)`.** Le op `#144627-144634` sono il
banco AFE sulla pagina di override `0x17xx`, poi `PHY.MOD 0x0408` con
`mask=0x0002` a zero e `PHY.WR 0x0417=0x0000`, `PHY.WR 0x0416=0x0001`: e'
esattamente `b43_phy_ac_switch_analog_once()` meno le dieci letture di
salvataggio, coi valori del restore cablati. Il commento di quella funzione lo
diceva gia' -- *"the bss-up copy of this unit in op_switch_channel can hardcode
them"* -- ma la copia era open-coded.

E' struttura, non un caso di `cold04`, e il set corrente lo conferma: nella coda
-- quel che segue l'ultimo mode change -- di **tutti e 43 i segmenti a freddo**
c'e' esattamente un banco AFE (`PHY.WR 0x173e`), un `PHY.MOD 0x0408`, un
`PHY.WR 0x0417` e un `PMU.RC`. A caldo la stessa terna su tutti e 44, col
`PMU.RC` due volte invece di una su 41 di essi e un segmento che porta il solo
banco.

L'unita' era scritta **tre volte** in `src/`: all'ingresso di
`switch_analog_once()`, nella sua coda a freddo, e qui. Ora e' una,
`b43_phy_ac_afe_arm(dev, mode, v417, v416)`, e i tre siti si distinguono solo
per i valori della coppia. Traccia di `cold01` identica byte per byte, gate
`28552/28574`, periodico `MATCH`.

**Cosa segue dalla classificazione.** Di sette blocchi uno solo appartiene a un
cal finalize. Gli altri sei sono, in ordine: un giro di watchdog (A), tre pezzi
di bss-up (potenza per-rate, potenza TX, frontend MAC/GPIO), un callback che in
b43 e' `phy_ops->switch_analog` e che il core chiama da altri quattro siti, e il
rilascio PMU che fa coppia con la richiesta del preambolo a freddo. Il gap di
457 ms fra A e B dice che B non e' la coda della calibrazione ma un evento
successivo: il tempo dice quello che l'ordine delle op da sole non poteva dire.

**Portata fuori.** Il blocco B e' ora `b43_phy_ac_down()`, e
`rxiqcal_finalize()` finisce dopo la fase probe. Lo stato che il blocco consuma
-- il readback dei LO DAC e i coefficienti TX IQ/LO salvati nel blocco D -- era
locale a `rxiqcal_finalize()` e sta in `@lo_dac` e `@txiqlo_coef` nello stato
del PHY, come gia' fanno `rxgain_saved` e compagnia. Traccia di `cold01`
identica byte per byte, gate `28552/28574`, periodico `MATCH`, e i punteggi
degli altri canali fermi (97.97 su `cold04`, 85.78 su `cold05`, 96.76 su
`cold17`, 98.44 su `cold24`, 86.41/89.28/85.58 a caldo).

Il taglio e' netto sul confine che il tempo indica: la prima op del blocco B e'
la terna `hi/lo/hi` sul settimo contatore `0x077c`, che sta a t=890.451 e non a
889.993 con la chiusura del watchdog -- l'`OBJ.RD 0x0040` che la incornicia nei
due lati e' `UCODESTAT`, del core, e il port non la emette.

**Chi la chiama.** In `src/` nessuno, e non e' una dimenticanza: quale hook di
b43 regga ogni pezzo e' la decisione aperta. Nell'harness la chiama
`run_switch_channel()` subito dopo `set_channel_calibrations()`, per la stessa
ragione per cui chiama `emit_core_bss_config()` e le passate di `conf_tx`:
quello e' lo stack sopra il driver e l'harness deve sostituirlo.

Conseguenza da tenere presente: **finche' nessun hook e' cablato, sul ferro quel
blocco non viene emesso.** Prima veniva emesso dentro `switch_channel`, cioe' al
momento sbagliato; ora non viene emesso affatto. I due candidati che la tabella
sopra rende ovvi sono `phy_ops->recalc_txpower` per i due pezzi di potenza
(b43 lo chiama da `b43_phy_txpower_check()`, che gira dopo `switch_channel`) e
il percorso `switch_analog`/rfkill per l'arm analogico e il PMU. Il write-back
dei coefficienti e il frontend MAC/GPIO non hanno un hook ovvio e vanno decisi
guardando da dove il vendor li emette in una cattura che contenga due bss-up
distinti.

### Quanto del denominatore non e' del driver

Da misurare prima di scegliere il lavoro, perche' cambia il tetto e non solo il
punteggio. Le ricariche del template beacon e della probe response sono un
blocco di forma fissa -- `0x00cc` letta e riscritta, `0x001e`, la `TPL.RAMW`
del beacon, `BTL0`/`BTL1`, il suspend del MAC, `TPL.RAMW 0x0700`, `PRTLEN`,
l'SSID `0x0160-0x017e`, `PRSSIDLEN`, poi una passata del PLCP -- e ognuna vale
**57 op esatte**. Il conteggio delle passate, testimone `TPL.RAMW 0x0700`:

| passate | segmenti |
| --- | --- |
| 7 | i 19 sopra i 5250 MHz, meno `cold15` |
| 10, 11, 12, 16, 18, 20, 21 | `cold01`, `cold15`, `cold03`, `cold02`, `cold04`, `cold18`, `cold17`/`cold24` |

Sullo stesso albero, quindi non e' il driver a deciderlo. Non correla con la
durata -- tutti i segmenti stanno fra 33.0 e 38.9 s, e `cold05` a 34.9 s ne ha
7 mentre `cold17` a 38.9 s ne ha 21 -- e non e' un orologio come la scadenza
della fase probe: su `cold01` le cinque tardive distano 1.53, 1.31, 2.35,
**16.38** e 1.31 s. E' il numero di volte che lo stack sopra ha ripubblicato il
beacon durante la cattura. Il port ne emette 5, che sono quelle ancorate a
`b43_op_config()` e alle passate `conf_tx`.

Costo, e quota delle mancanti che ne dipende:

| segmento | costo passate | mancanti | quota |
| --- | --- | --- | --- |
| cold01 | 285 | 355 | 80% |
| cold02 | 627 | 793 | 79% |
| cold03 | 399 | 551 | 72% |
| cold04 | 741 | 899 | 82% |
| cold17 | 912 | 1074 | 85% |
| i 19 da ch52 | 114 | 749 – 877 | 13-15% |

Il controllo che rende la conclusione utilizzabile e' il contrasto con una fase
vicina: `prb_rsp_rate_po` (testimone `OBJ.RD 0x099a`) sta a **3 passate su
tutti e 43 i segmenti a freddo e tutti e 44 quelli `up` dello sweep a caldo**,
e `OBJ.WR 0x00ce` a 3 per segmento a freddo e 4 per segmento `up`, senza
un'eccezione. Stesso vicinato nella traccia, esito
opposto. Quindi il criterio esiste ed e' il conteggio fra segmenti: sotto quel
test le passate di template sono stimolo, `rate_po` e' struttura -- ed e' su
quella prova che la terza passata di `rate_po` e' stata scritta.

Conseguenza sul tetto: finche' `grezzo` tiene nel denominatore ripetizioni
decise dall'host, il 100% non e' una soglia raggiungibile. Serve una categoria
dichiarata, e non e' nessuna delle due esistenti -- non `SOLO_VENDOR`, perche'
b43 quelle op le emette davvero da `b43_update_templates()`, e non `PERIMETER`,
perche' non sono di altri. Il meccanismo di `cmp_skip.py` oggi salta op
singole; qui serve saltare un **blocco** con un conteggio, e il criterio
d'ingresso e' quello misurato sopra.

### Il tetto: quanto di quel che manca e' del PHY

Il denominatore e' l'unione, quindi 100% vuol dire che b43 emette anche le op
del core. Le mancanti di `cold01`, per classe:

| classe | mancanti | di chi e' |
| --- | --- | --- |
| `OBJ.WR` | 262 | shared memory del MAC, `main.c` |
| `OBJ.RD` | 51 | idem |
| `TPL.RAMW` | 19 | template RAM, core |
| `MAC.MCTRL` | 18 | core |
| `OTP.*`, `SROMCTL.RD`, `CAL.INIT` | 4 | codice srom di bcma |
| `MAC.MHF.RD` | 1 | `b43_hf_read()`, core |

**Zero op del PHY**, e i valori sbagliati sono tutti `PHY.WR`. Tutte e 355
sono del core, e si spaccano in due meta' che sono due lavori diversi: **285**
sono le cinque ricariche di template della sezione sopra, che nessun codice
del driver puo' emettere nel numero giusto, e **70** sono debito vero. Quindi
sul segmento di riferimento il tetto raggiungibile modellando il blocco di
config MAC/ucode e' 28483/28774 = **98.99%**, non il 100%.

Le 70, per gruppo, con lo stato di ciascuno:

| op | cosa e' | perche' non e' ancora emessa |
| --- | --- | --- |
| 9 | `0x05d6`, `0x05d8` (4 occorrenze) e `0x05dc` | non e' `KEYIDXBLOCK` e non e' del core: vedi sotto. Le altre tre celle del blocco le scrive ora `src/`; queste due mancano della maschera parziale, e `0x05dc` di un'origine |
| 19 | `TPL.RAMW` | 7 sono raggiungibili (`0x0200` in `emit_core_bss_config`, `0x0480` in `emit_core_bss_config1`, `0x0700` nelle 5 passate precoci) ma l'harness **non ha un emettitore `TPL.RAMW`**, e la regola di `PERIMETER` va **ristretta al solo `0x0048`**, non rimossa: rimuoverla scopre `#12960` e riporta il muro posizionale indietro di 18000 op |
| 18 | `MAC.MCTRL` | 13 nette delle passate, sparse |
| 7 | `0x0180`-`0x0186` | temporizzazioni probe response, `#1248-1251` e `#10319-10321`. `CORE_SHM` dice che b43 non le scrive mai |
| 4 | `0x0300`-`0x0306` = `0x2637` | dentro la regione stimolo |
| 4 | `OTP`/`SROMCTL`/`CAL.INIT` | bcma, fuori da `src/`, gia' scartate dal perimetro |
| ~15 | one-shot del core init | `0x26`, `0x50`, `0x52`, `0x74`, `0x94`, `0x96`, `0xa4`, `0xb4`, `0xd6`, `0xb0`, la terza coppia `0x0`/`0x2` a `#13468`, `MAC.MHF.RD`. Ognuno un piazzamento a se' |

Nota d'ordine: il muro posizionale a `@25157` **e'** l'inizio della prima
ricarica tardiva. Finche' quel blocco non ha una categoria, nessuna delle 70
sposta il posizionale di un'op -- alzano solo il grezzo.

La composizione cambia per famiglia, e va guardata prima di scegliere il
lavoro:

| segmento | grezzo | valore sbagliato | mancanti | di troppo |
| --- | --- | --- | --- | --- |
| cold01 ch36 bw20 | 99.90% | 0 | 29 | 0 |
| cold05 ch52 bw20 | 99.71% | 8 | 29 | 0 |
| cold24 ch36 bw80 | 98.65% | 93 | 201 | 141 |

Le due famiglie non hanno piu' pesi diversi di un ordine di grandezza, e sulla
famiglia alta la colonna delle op di troppo e' a zero: erano le calibrazioni
post-switch che il vendor sopra i 5250 non fa, e in coda il write-back dei
coefficienti IQ/LO di `b43_phy_ac_down()`, che il port emetteva a zero perche'
in un attach a freddo nessuno li aveva salvati. Quello che distingue le due
famiglie ora e' la colonna del **valore**: zero a ch36 bw20, 8 a ch52, 93 a
80 MHz. Il valore sbagliato cresce con la larghezza, non con la banda, e sotto
i 5250 a 20 MHz non c'e' affatto.

Sulla famiglia alta, cosa si sa ora.

**Il poll e' emesso.** `b43_phy_ac_cac_poll()` emette il blocco di 4 op, armato
da un MOD di `0x02e4` subito dopo la `MHF` clear che apre
`post_bringup_tail()`. Quanti turni e dove cadono lo dice il chiamante, come
per `probe_ticks` e le ricariche del beacon: la cadenza e' un orologio e
l'harness non ne ha uno. `reverse-tools/cac_polls.py` li legge dalla cattura e
`gates.sh` passa `AC_CAC_POLLS`.

Il piazzamento e' **posizionale e non temporale**, ed e' la cosa che e' costata
un tentativo: i turni si contano fra letture consecutive di `OBJ.RD 0x07da`, il
poll di statistiche che apre ogni giro di watchdog e che il port emette
identico (21 su ogni segmento dello sweep). Costruendo invece una griglia dai
timestamp con `probe_schedule.py` -- come fa `beacon_reloads.py` -- i confini di
tick non cadono dove il port emette i corpi e i turni finiscono un tick piu' in
la': 97.75% su `cold05`, con 126 mancanti e 148 di troppo alternati uno a uno.
Con il marcatore posizionale, 98.40%.

Il blocco in se' resta descritto qui sotto, perche' e' la prova che lo sostiene.
Il vendor esegue **4 op esatte** --
`MAC.MCTRL val=0x0 mask=0x1`, `PHY.RD 0x0251`, `PHY.RD 0x0252`,
`MAC.MCTRL val=0x1 mask=0x1` -- ripetute 134 volte su 22.6 s, cadenza misurata
151-152 ms in 131 intervalli su 133, con un solo salto iniziale di 2.6 s. 112
dei 133 intervalli distano **4 op**: le iterazioni sono contigue nel flusso,
cioe' fra due scatti il driver non fa nient'altro.

Il conteggio per segmento e' stato rifatto sul set corrente, e il termine che ne
esce non e' la soglia. `PHY.RD 0x0251` sta a **zero su ch36-48 e su ch144-165**
-- sotto e sopra i 5250 -- e a 160-722 turni a freddo su ch52-140, cioe'
esattamente sui canali con la guardia radar; a caldo la stessa partizione, 21-448
turni sugli stessi canali. Sul set precedente il blocco non compariva affatto a
caldo, ed e' quella assenza ad aver fatto scrivere il termine "primo bring-up":
la ricattura con il CAC completato la smentisce. Il valore letto e' zero in ogni turno dello sweep, quindi il
driver emette le letture e non ci fa niente: un branch su un valore mai
osservato sarebbe inventato.

La condizione e' il complemento esatto di `may_calibrate_tx()`, non
`cac_pending` da solo: a freddo il controllo e' pendente su ogni canale, ma solo
un canale con dovere radar lo aspetta. Letta la sola flag, il turno di arming
cade anche su ch36, dove la cattura non ne ha -- cinque op di troppo sui sette
segmenti bassi, che e' come e' stato trovato l'errore.

Il fatto che orienta tutto il resto: `PHY.RD 0x0251` e `0x0252` sono **le sole
due identita' che il percorso a ch52 tocca e quello a ch36 no**. Sugli altri
179 indirizzi il rapporto e' invertito -- ch36 li tocca, ch52 no. L'attach
sopra i 5250 e' percio' un **sottoinsieme stretto** di quello sotto, piu'
questo poll: non c'e' un percorso alternativo da scrivere, c'e' del lavoro da
non fare e due registri da leggere periodicamente.

La cattura si comporta come deve se e' il CAC: il poll parte 12.2 s dentro il
segmento, subito dopo l'attach, e l'ultima iterazione cade **80 ms prima della
fine della cattura**. Non si ferma mai. Il CAC dura 60 s e il segmento 34.9,
quindi la finestra finisce a controllo in corso -- ed e' per questo che le
calibrazioni non ricompaiono piu' tardi nel segmento: non perche' il canale le
proibisca per sempre, ma perche' il controllo non fa in tempo a finire.

**SALAME** su quali registri siano: che `0x0251`/`0x0252` siano il rivelatore
radar e' nostra e non verificata -- regge su `0x02e4`, che la regola `agcombo`
di `cmp_skip.py` gia' associa a `wlc_phy_radar_detect_on_off_cfg_acphy` e che
viene modificato subito prima della prima iterazione, ma `0x02e4` compare una
volta anche a ch36 e da solo non discrimina. Che il **motivo** del salto sia il
CAC invece non e' un'ipotesi sulla cattura: e' la regola, e la cattura la
conferma. Vedi il commento di `may_calibrate_tx()`.

### Chi possiede il CAC, e perche' cambia il denominatore

Il predicato di `may_calibrate_tx()` non e' piu' una soglia dedotta dallo
sweep. Le sotto-bande che portano il dovere radar le da'
`IEEE80211_CHAN_RADAR`, sullo stesso `ieee80211_channel` da cui questo codice
gia' legge `hw_value` e `center_freq`; lo stato del controllo lo da'
`ac->cac_pending`, che e' quello che mac80211 comunica al driver. Non
`ieee80211_channel.dfs_state`, che darebbe la stessa risposta ma facendo
leggere al driver una macchina a stati che non possiede e da cui non viene
notificato. Sui 78 segmenti di allora il risultato era
identico op per op -- tutti i gate dove erano, `cold01` a 98.75%,
`09-up-ch52-bw20` a 80.91%, periodico `MATCH` -- perche' quei due sweep si
fermavano a ch140 e la' soglia e regola coincidono. Sul set corrente non
coincidono piu' affatto, ed e' la ricattura dei DFS a separarle: vedi la tabella
in testa al documento. Divergono su U-NII-3: a
ch149-165 (5745-5825 MHz) non c'e' dovere radar, la soglia sopprimeva le
calibrazioni e la regola le fa girare. Verificato sul port: `PHY 0x0380` fa 0
accessi a ch52 e ch140, e 791 a ch149, ch157 e ch165 come a ch36. Nessuna
cattura puo' arbitrare quel caso, che e' la ragione per prendere la regola
dalla spec e non dallo sweep.

Ne segue una cosa che riguarda il punteggio, non il codice. Il CAC non e' del
driver: lo guida mac80211, per conto di cfg80211 e su richiesta di un AP in
userspace, e al driver chiede la rilevazione radar attraverso
`.start_radar_detection` -- chiamata con l'hardware **gia'** sintonizzato, che
e' esattamente la finestra in cui queste calibrazioni devono star fuori. Ed e'
il produttore che oggi manca: b43 non dichiara rilevazione radar, quindi
mac80211 non ci porta su un canale radar in AP per niente, e `cac_pending` non
ha nessuno che lo scriva a parte il flow. Quindi
**il poll da 4 op non e' parte dell'attach**: e' il work item della rilevazione
che gira durante il controllo, e non deve essere emesso da `switch_channel`. Le
sue 268 op non sono debito dell'attach, sono una funzionalita' assente, e
l'harness non ha un punto d'ingresso dove metterle. Quando ce l'avra' quelle
op escono dal denominatore del gate a freddo invece di entrare nel numeratore.

E in senso opposto: quando il produttore esistera', hostapd finira' il CAC
**prima** di far salire l'AP, quindi al bring-up `cac_pending` sara' falso dove
le catture a freddo lo hanno vero, e le calibrazioni girerebbero. La forma che il gate a freddo premia sopra i 5250 -- calibrazioni
saltate -- b43 la riprodurra' solo perche' l'harness dichiara il CAC non
fatto, che e' vero per un modulo appena caricato ma non e' la sequenza che
hostapd produce. Stessa regola, proprietario diverso: `AC_DFS_CAC_DONE`
esiste per esercitare l'altro caso.

**Cosa il vendor non esegue, e non e' "sopra la soglia".** La calibrazione RX
IQ, e non in forma ridotta: `PHY.RD 0x0380`, il comando del generatore di tono,
e `PHY.RD 0x0270` stanno a **zero** insieme. Sul set precedente lo erano su ogni
segmento sopra i 5250, e da li' veniva la soglia; sul set corrente, rimisurati
sui 43 segmenti a freddo, sono zero **solo** sui sei radar-meteo -- ch120, ch124,
ch128 a 20 MHz, ch116 e ch124 a 40, ch116 a 80 -- e altrove valgono 209-912 e
72-189, sopra i 5250 come sotto. I sei sono gli unici ripresi ancora senza il
CAC completato, quindi quello che questa misura separa e' la disponibilita' del
canale, non la frequenza. Sulle
celle di guadagno lo stesso: `TBL.WR id=0x07 off=0x100` 11 volte sotto e **1**
sopra, `TBL.WR id=0x0c off=0x63` 43-45 sotto e **1** sopra. La singola passata
che sopravvive e' `b43_phy_ac_rxiqcal_teardown_apply_defaults()`, la cui forma
combacia con il blocco del vendor a `#155558` e seguenti.

### Il blocco 0x05d4-0x05dc non e' KEYIDXBLOCK

`b43.h:281` chiama `0x05D4` KEYIDXBLOCK e il commento aggiunge `(v4 firmware)`.
Sul core AC quell'offset e' altro, e ci sono tre prove indipendenti.

La codifica non torna. `key_write()` in `main.c:826` scrive
`value = ((kidx << 4) | algorithm)` in `KEYIDXBLOCK + kidx*2`, quindi lo slot a
`+2` avrebbe valore almeno `0x10` e quello a `+6` almeno `0x30`. La cattura
scrive `0x05d6=1`, `0x05d8=1`, `0x05da=3`: **nibble alto a zero su tutti e
quattro gli slot**. E la catena di chiamata parte da `b43_op_set_key()`
(`main.c:4248`), non dall'init, mentre le catture a freddo sono prese prima di
qualsiasi associazione. Nella stessa direzione: `BT_BASE0`/`BT_BASE1` di `b43.h`
sono `0x0068`/`0x0468` e la cattura carica i template beacon a `0x0200`/`0x0480`
-- il layout della shared memory dell'AC non e' quello del firmware v4.

**Il valore e' la maschera delle catene**, e la seconda board lo dimostra. Su
D6220, che ha `txchain=rxchain=3`, le celle `0x05d4`, `0x05da` e `0x05dc`
portano `0x3`; sull'agcombo, che ha `7`, portano `0x7`. Su tutti e 43 i segmenti
a freddo e nei 44 `up` del d6220, e su tutti e 26 i segmenti a freddo
dell'agcombo. Due conteggi di catene diversi, due valori diversi, sempre
uguali a `coremask`: derivato, non trascritto -- il driver la maschera la ha
gia'. Le scrive `b43_phy_ac_chainmask_block()`, a tutti e quattro i siti, e il
perimetro e' stato ristretto a `0x05d6`-`0x05d8` nello stesso passo.

**Le due celle in mezzo sono chiuse come tabella, ma il predicato che le
accende no.** Dove sono parziali portano una maschera che dipende dalla
larghezza, dal numero di catene popolate e da quale dei quattro siti le scrive;
altrove portano `coremask` come le altre tre. Il predicato nel driver aveva un
termine di troppo: la maschera parziale compare su dodici segmenti del set
corrente, sei a freddo e i sei `up` corrispondenti -- ch36-48 a 20 MHz e ch36,
ch44 a 40 -- mentre sul set precedente i 52 `up` davano `coremask` ovunque, ed
era quell'assenza a giustificare il termine sul primo bring-up. A parita' di
canale e larghezza i due sweep si comportano uguale, quindi il predicato e' la
sola sotto-banda, ed e' cosi' che `b43_phy_ac_chainmask_block()` lo scrive oggi:
il gate di `01-up-ch36-bw20` passa da 90.96% a 91.04%, otto valori sbagliati in
meno (due celle per quattro siti), e nessun altro segmento si muove.

I quattro siti non sono intercambiabili: tre stanno nel channel setup e nel
`down`, il quarto sta in `b43_phy_ac_txpwr_adjust()` e sopra i 20 MHz porta una
coppia piu' larga. Questo corregge la lettura precedente, che dava la terza
occorrenza sempre piena: a 20 MHz non lo e' su nessuna delle due board.

| catene | sito | bw20 | bw40 | bw80 |
| --- | --- | --- | --- | --- |
| 2 (`coremask` 0x3) | setup / down | 1, 1 | 1, 3 | 3, 3 |
| 2 | `txpwr_adjust` | 1, 1 | 3, 3 | 3, 3 |
| 3 (`coremask` 0x7) | setup / down | 1, 5 | 5, 5 | 5, 7 |
| 3 | `txpwr_adjust` | 1, 5 | 7, 7 | 7, 7 |

Riprodotte op per op su tutti e quattro i siti, tre larghezze, due board: 24
punti, zero divergenze. L'indice e' `hweight8(coremask)` e non `num_cores`, che
sulla d6220 vale tre con due catene cablate.

**Perche' una tabella.** Le dodici coppie le riproduce anche una legge: la
maschera di *p* catene su *n* presa spaziando le catene il piu' possibile --
due su tre sono la 0 e la 2, da cui `5` e non `3` -- con *p* che sale insieme
alla larghezza e al numero di catene, piu' uno scatto al sito `txpwr_adjust`
sopra i 20 MHz. Funziona su tutti e dodici i punti, e non e' scritta cosi':
i suoi tre termini non hanno una ragione indipendente dalle stesse dodici
coppie, e il primo punto che la metterebbe alla prova -- una board a quattro
catene -- non esiste in nessuna cattura. Una tabella dice quello che si e'
misurato e si ferma li'; un conteggio di catene fuori tabella prende
`coremask` e lo dichiara con `b43_phy_ac_todo()`.

Nota di metodo, pagata prima che la voce si chiudesse: emettere **parte** di un
blocco contiguo costa il posizionale. Con solo `0x05d4`/`0x05da` ai quattro
siti, il muro era tornato da `@25164` a `@10896`, perche' il perimetro non
nascondeva piu' la prima occorrenza e il port non la emetteva. Il blocco va
emesso a tutti i siti o a nessuno.

**Chiuso:** `b43_phy_ac_rxgain_config_apply()`, 146 op, e' ora dietro
`may_calibrate_tx()`. Testimoni esclusivi `PHY 0x0724` e `PHY 0x0736`. Sul set precedente erano dieci
accessi a ch36 e zero dal ch52 in su; sul set corrente, rimisurati sui 43
segmenti a freddo, sono 27 su ciascuno dei sette a ch36-48 e fra 9 e 60 dal
ch52 in su, con il 9 sui soli sei radar-meteo. Cioe' il vendor la rxgain la
esegue anche sopra i 5250 e la salta dove il canale non e' disponibile: e'
esattamente il predicato con la guardia radar e il CAC, e non la soglia, che
sul set precedente dava lo stesso risultato per caso.

**Chiuso:** il write-back dei coefficienti IQ/LO in coda a
`b43_phy_ac_down()`, 44 op su venti dei ventisei segmenti. Il port lo emetteva
sempre, e sopra i 5250 a freddo con tutti i valori a zero: il blocco D di
`rxiqcal_finalize()`, che li salva, sta dentro il gate della calibrazione e
la' non gira. Il predicato al sito di restore **non** e'
`may_calibrate_tx()`, ed e' il caldo a dirlo: su `09-up-ch52-bw20` e
`17-up-ch100-bw20` la calibrazione e' saltata e il write-back c'e' lo stesso,
con valori non nulli. E' `@iqlo_saved`, alzato dal blocco D: si riscrive quello
che si e' salvato. Lo sweep a freddo da solo non poteva arbitrare fra i due --
un canale per caricamento del modulo, quindi il salvataggio o e' avvenuto in
quello stesso segmento o mai. La famiglia alta va a **zero op di troppo**,
ch36 non si muove, il gate periodico resta `MATCH`.

**Aperto, e la difficolta' e' misurata.** Nessun'altra fase candidata passa il
test di `may_calibrate_tx()`. Applicato per funzione a ch52 -- tutte le
identita' della fase a zero nel vendor, con la tabella come identita' per il
lavoro che passa dalla porta dati -- non ne passa nessuna:
`rxiqcal_apply_body_core` ha 44 identita' a zero e 117 presenti,
`rxgain_perchan_config` 44 e 120, `rxgain_defaults_pulse` 18 e 50. E il test
piu' forte, il testimone esclusivo, per queste non e' nemmeno applicabile:
**non hanno una sola identita' che nessun'altra funzione del port emetta**,
perche' lavorano attraverso la porta di tabella e i banchi per-core condivisi.
`PHY.MOD 0x0723` e `0x0923` stanno a zero nel vendor a ch52 e a 17 ciascuno nel
port, ma sono emessi da tre funzioni diverse, quindi provano assenti i registri
e non la fase -- il controesempio di `idle_tssi_meas` descritto nel commento di
`may_calibrate_tx()`, nella stessa forma.

Il residuo non e' percio' "altre fasi da mettere dietro lo stesso gate". Il
port e il vendor eseguono le stesse fasi sopra la soglia, con **molteplicita'
diversa**: le due funzioni sopra emettono a ch52 esattamente quello che
emettono a ch36 -- 12 e 12 accessi su `0x0723`/`0x0923` -- mentre il vendor
sopra non ne emette nessuno. Il gate va dentro il corpo, e per metterlo serve
una prova che oggi non c'e'. Il posto dove cercarla e' la scomposizione per
offset qui sotto, che ha ch36 come controllo perfetto.

La scomposizione per offset che seguiva questa voce era sulle 1804 op di
troppo della famiglia alta, che sul set di allora erano state azzerate: il
confronto per (classe, indirizzo) sui 26 segmenti non trovava piu' una sola op
di troppo dal ch52 in su, salvo i giri del watchdog della voce seguente. Sul
set corrente la voce "di troppo" e' tornata a valere da 0 a 1162 op per
segmento -- vedi la tabella dei punteggi -- perche' sopra i 5250 il vendor ora
esegue le calibrazioni, quindi quel confronto va rifatto e non e' piu' il
consuntivo di questa voce. Il residuo di quella famiglia e'
tutto nella colonna del valore.

Metodo: il conteggio per (classe, indirizzo) e' ordine-indipendente e non si
fa ingannare dal disallineamento dell'LCS, che sui segmenti alti spalma il
difetto su venti funzioni e non dice niente. L'attribuzione per funzione con
`AC_FN_MARKERS=1` va usata solo con testimoni **esclusivi** di una funzione:
sulle porte di tabella (`0xd`/`0xe`/`0xf`) e su `0x19e`, che tutte le fasi
toccano, non conclude.

Trappola dello strumento: gli indici `port[...]` che l'ancoraggio restituisce
sono nella vista di `cmp_skip`, dopo l'offset di allineamento e le liste
solo-port. Leggerli come posizione nel flusso mette il blocco dove l'LCS ha
gia' consumato le controparti, e il sintomo e' op di troppo che appaiono con le
mancanti che non scendono. La mappa giusta la da' un run con
`AC_FN_MARKERS=1` indicizzato come `fns[keep[i]]`, non `fns[i]`.

Da tenere presente prima di emettere qualsiasi cosa in quella regione: il
blocco EDCF `0x0260-0x027e` e i parametri `0x0240-0x025e` sono dichiarati del
core in `CORE_SHM`, quindi il perimetro li scarta dal lato vendor ed emetterli
produce solo inserzioni. Verificato.

Per chi scrive la regola a blocchi: le cinque passate tardive si distinguono da
quelle che il port emette **per struttura e non per posizione**, e questo e' il
punto che rende la regola dichiarabile. Le tardive portano un
`MAC.MCTRL val=0x0 mask=0x1` fra la scrittura di `BTL0`/`BTL1` e la
`TPL.RAMW 0x0700`; le due precoci ancorate a `b43_op_config()` no, e le tre
`conf_tx` non hanno nemmeno il prologo del beacon. Le due varianti tardive
alternano `TPL.RAMW 0x0200` + `BTL0` e `TPL.RAMW 0x0480` + `BTL1`, cioe'
`b43_upload_beacon0()` e `b43_upload_beacon1()`, sempre partendo e finendo su
beacon0 -- la serie ha percio' lunghezza dispari, e infatti i conteggi
osservati sono 7, 9, 11, 15 sui segmenti a 20 MHz.

Attenzione a un dettaglio dell'ordine: `apply_skips()` scorre il flusso vendor
in avanti e salta le prime `max` occorrenze. Una regola che matcha tutte e
dieci le passate ne salterebbe le prime cinque, che sono proprio quelle che il
port emette, e le cinque emesse diventerebbero op di troppo. Il discriminante
del `MAC.MCTRL` interno serve esattamente a questo.

### Il latch della finestra sui giri in ritardo

Il port emetteva il latch della finestra statistiche su ogni tick della fase
probe, e i tick li conta `probe_schedule.py` dalla cadenza dei gruppi. Le due
cose coincidono solo se il timer del vendor non ha perso colpi, e su cinque dei
ventisei segmenti li ha persi.

**Non e' il giro che manca, e' la sua coda.** La prima lettura -- "il vendor ha
fatto meno giri, il port ne fa di troppo" -- e' sbagliata, e il conteggio per
(classe, indirizzo) lo diceva gia': le op di troppo erano *solo* le letture di
`0x0308-0x0314` e `OBJ.RD 0x008c`, e nient'altro. Un giro saltato porterebbe
via anche la spazzata dei contatori, il blocco di misura e il cambio di modo.
Provato lo stesso a ridurre il numero di giri: cold17 scende da 97.41% a
**94.18%** e cold04 a 96.17%, con le op di troppo giu' di un centinaio e le
mancanti su di millecento. E' il latch, non il giro.

Come si misura. Ogni latch e' una `OBJ.RD 0x0314` -- ultima cella della
finestra, e la spazzata non la tocca perche' si ferma a `0x0312`. Si prendono
quelli dal primo gruppo probe in avanti: quelli prima sono il latch a se' che
`stats_latch_and_crs()` emette sotto i 5250 MHz. Ognuno va sullo slot
`round((t - t0) / tick)` con due vincoli -- gli slot crescono, e quelli rimasti
bastano per i latch rimasti. Senza il primo, due latch a 2 ms l'uno dall'altro
(un risveglio in ritardo seguito dal recupero) finiscono sullo stesso slot e
contano un salto che non c'e'; senza il secondo un ritardo in coda spinge
l'ultimo oltre la scadenza. Gli slot vuoti sono i giri senza latch.

| segmento | scadenza | giri senza latch |
| --- | --- | --- |
| i 21 regolari | N | nessuno |
| cold03 ch44 bw20 | 18 | 6, 12 |
| cold04 ch48 bw20 | 18 | 2, 9, 11, 13, 17 |
| cold15 ch136 bw20 | 19 | 7, 9 |
| cold17 ch36 bw40 | 21 | 2, 5, 7, 9, 11, 19, 20 |
| cold18 ch44 bw40 | 18 | 1, 3, 13, 15, 16, 17 |

Il controllo sta nei dati: sui ventuno regolari il numero di latch viene uguale
alla scadenza e la lista viene vuota, quindi il metodo non inventa salti dove
non ce ne sono. La causa e' la macchina carica -- divari fino a 2.6 s dove la
cadenza e' 1.004, e nella stessa finestra i gruppi probe a 1.31 s -- non una
scelta del driver, che e' la ragione per cui il conteggio viene dal chiamante
come `probe_ticks`, le ricariche del beacon e il poll di CAC.

`reverse-tools/watchdog_turns.py` lo legge dalla cattura, `gates.sh` passa
`AC_WD_NOLATCH` e `@probe_nolatch_tick` lo porta a `b43_phy_ac_wd_turn()`. Le
letture di troppo su `0x0308-0x0314` e `0x008c` passano da 22 per cella a zero,
e i cinque segmenti salgono: cold03 98.65 -> **99.36**, cold04 97.95 ->
**98.08**, cold15 97.87 -> **98.64**, cold17 97.41 -> **97.88**, cold18 97.37
-> **97.99**. Gli altri ventuno non si muovono, il gate periodico resta `MATCH`
e il caldo non cambia.

**Aperto: la spazzata.** Anche `b43_phy_ac_wd_stats_clear()` salta dei giri, e
non gli stessi: su cold17 il latch manca su sette e la spazzata su sei, con
indici diversi. Restano 12, 30, 12, 36 e 31 op di troppo sui cinque segmenti,
tutte `OBJ.WR 0x0308-0x0312`. Due tentativi, nessuno dei due paga:

- spazzata gatata insieme al latch, stessa lista -- cold03 99.36 -> 99.32,
  cold04 98.08 -> 98.06, cold17 97.88 -> 97.81;
- spazzata con una lista sua, dai tempi di `OBJ.WR 0x0312` -- cold17 e cold18
  salgono di poco, cold03 e cold04 scendono, netto nullo.

Le mancanti salgono quanto scendono le op di troppo, quindi quelle scritture il
vendor le emette e il modello a slot non le colloca dove stanno. La lista la
misura `watchdog_turns.py --check`, e resta fuori da `--sh` finche' non c'e' un
criterio che la piazzi. Serve un testimone migliore di `0x0312`, o capire cosa
il vendor fa fra la spazzata e il cambio di modo su un giro in ritardo.

Trappola da non ripetere: emettere un latch in piu' o in meno sposta tutto il
flusso a valle, quindi su questi cinque segmenti il primo `@N` di `compare.py`
e le regioni di `cmp_skip.py` dopo la fase probe non dicevano niente sul driver
finche' la lista non veniva dalla cattura.

### Il banco a undici prese degli 80 MHz — chiuso

Il vendor programma per catena un banco di undici prese a `0x?6a4-0x?6ae`,
subito dopo i coefficienti RX IQ, **solo a 80 MHz**. La condizione e' la
larghezza e nient'altro: `PHY.WR 0x?6a4` c'e' su tutti e nove i segmenti a 80
in repo -- cold24 del d6220, cold25 dell'agcombo, i sei `up` a 80 dello sweep a
caldo -- e su nessuno dei sessantanove a 20 e 40. Sopra i 5250 a freddo manca
per la ragione generale, che la calibrazione non gira: agcombo cold26, ch52 a
80, e' l'unico segmento a 80 senza.

I valori sono costanti: le stesse tre serie, identiche su due board, su ogni
canale, a freddo e a caldo. Ognuna e' un nucleo antisimmetrico `1/k` attorno a
una presa centrale di `0x0400`, cioe' l'unita' in Q10 -- le prese a distanza k
valgono circa `±c/k` -- quindi un correttore di ritardo di gruppo con un solo
parametro libero:

| serie | c | prese |
| --- | --- | --- |
| A | ~60.5 | `000c fff1 0014 ffe2 003d 0400 ffc4 001e ffec 000f fff4` |
| B | ~182 | `0025 ffd2 003d ffa4 00b8 0400 ff4c 005b ffc4 002d ffdc` |
| C | ~208 | `002a ffcc 0046 ff97 00d3 0400 ff32 0067 ffbb 0034 ffd6` |

**Cosa scelga `c` non e' stabilito**, e per questo la tabella e' indicizzata su
(catene, core) e non su una formula: il d6220 prende `{A, B}` sulle sue due
catene e l'agcombo `{C, B, A}` sulle sue tre, quindi non e' l'indice del core a
decidere -- la catena che porta A e' la 0 su una board e la 2 sull'altra. Un
conteggio di catene fuori tabella non emette il blocco e lo dichiara con
`b43_phy_ac_todo()`: meglio nessun filtro che un ritardo di gruppo preso dalla
catena sbagliata. Lo emette `b43_phy_ac_bw80_fir_write()` in coda a
`b43_phy_ac_rxiqcal_apply_coefficients()`. cold24 passa da 98.65% a **98.73%**,
31 op mancanti chiuse, e nessun altro segmento si muove.

### La ri-emissione del blocco CRS, e una regola che non regge

Contato sul set corrente col testimone `PHY 0x0321`, sugli 87 segmenti -- 43 a
freddo e 44 `up` -- il blocco delle soglie CRS e del banco `0x0910` gira tre
volte su 56, quattro su 17, cinque su 4 e due su 10. Sul set precedente erano
tre volte su 76 di 78, con due sole eccezioni; la ricattura dei canali DFS ha
spostato la distribuzione, non l'ha solo allargata, e le due eccezioni di allora
erano `cold09` (ch100 bw20) con quattro e `03-up-ch40-bw20` con cinque.

Le due eccezioni si somigliano. Su cold09 la passata in piu' cade al quinto
latch e scrive una soglia piu' bassa -- `0x0321 = 0x0034` contro lo `0x0039`
della passata dentro il setup -- dopo che il campione di rumore e' sceso da
1621 a ~1460 e ci e' rimasto. Su hot03 le due in piu' bracchettano un picco: il
secondo campione del segmento vale 0x250e, cioe' 9486 contro i ~1300 di tutti
gli altri, la passata successiva alza a `0x0040`, e quattro tick dopo si torna
a `0x0034`.

Letta cosi' sembra un'isteresi asimmetrica -- sale subito, scende dopo quattro
campioni -- e **non regge**. Applicata ai livelli di `b43_phy_ac_crs_noise_th[]`
campione per campione sui 26 segmenti a freddo di allora, prevedeva una
ri-emissione dove la cattura non ne aveva nessuna su cinque segmenti, e quattro
dove ce n'era una. Il test che conta e' quello negativo: la regola deve dire
"niente" sui segmenti che non hanno niente, e ne sbagliava sei. Con la
distribuzione del set corrente il controllo va rifatto per intero, perche' i
segmenti a tre passate non sono piu' la quasi totalita'.

Quindi la ri-emissione non e' funzione del solo livello della scala. Restano da
provare: una soglia sul campione grezzo invece che sul livello, una finestra
piu' lunga di quattro, o un ingresso che non e' il campione di rumore. Il costo
era 16 op su un segmento del set precedente, quindi la voce vale per la regola,
non per il punteggio: se il vendor riprogramma il carrier sense a regime, su hardware lo
deve fare anche b43.

## La mappa di shared memory di b43.h e' quella del firmware v4

`b43.h:281` marca `KEYIDXBLOCK` con `(v4 firmware)`, e non e' l'unica cella in
quelle condizioni. L'audit incrociato -- ogni `B43_SHM_SH_*` di `b43.h` contro
gli accessi della cattura `cold01` -- separa le celle che tornano da quelle che
no.

**Tornano, e con prove indipendenti dal nome.** `PHYTYPE` (0x0052) porta `0x0b`,
che e' `B43_PHYTYPE_AC`; `PHYVER` (0x0050) porta 1. `SFFBLIM` (0x0044) e
`LFFBLIM` (0x0046) portano 3 e 2, **gli stessi valori che b43 scrive**
(`main.c:4911-4912`). `PRSSIDLEN` (0x0048) porta 7 e `PRSSID` (0x0160) porta
`test-ap` -- sette caratteri. `MACHW_L`/`MACHW_H`, `UCODEREV`, `UCODEPATCH`,
`WLCOREREV`, `TIMBPOS`, `BTL0`/`BTL1`, `PRMAXTIME`, `SPUWKUP`, `PRETBTT`,
`CHAN`, `KTP`, `UCODESTAT`, le cinque `HOSTF` e le tabelle rate/EDCF: tutte
coerenti. La mappa e' giusta in larga parte.

**Non tornano, e vanno gatate.** In ordine di gravita':

| cella | b43.h | sull'AC | conseguenza |
| --- | --- | --- | --- |
| `KEYIDXBLOCK` 0x05D4 | indice/algoritmo chiave, 54 slot fino a 0x0640 | 0x05d4/0x05da/0x05dc sono la maschera di catene; 0x05e0-0x0666 e' la corsa di azzeramento del PHY (`phy_ac.c:5554`) | **`b43_security_init()` -> `b43_clear_keys()` gira all'attach (`main.c:4957`) senza nessuna chiave e riscrive 54 word la sopra.** Si porta via la configurazione delle catene |
| `PSM` 0x05F4 | PSM transmitter address match (rev < 5) | dentro la stessa corsa 0x05e0-0x0666 | idem, se qualcuno la scrive |
| `TKIPTSCTTAK` 0x0318 | cache TKIP fase 1, 50 voci da 14 byte, arriva a 0x05d3 | zona senza evidenza nella cattura, ma stessa assunzione di layout | 700 byte in un posto non verificato. Solo con `modparam_hwtkip` |
| `BT_BASE0` 0x0068 / `BT_BASE1` 0x0468 | basi dei template beacon in template RAM | la cattura carica i beacon a **0x0200** e **0x0480** | template scritti nel posto sbagliato |

Il tavolo delle chiavi invece si autolocalizza: `dev->ktp` viene letto a runtime
da `KTP` (0x0056), e la cattura quella lettura la fa. Sono **hardcoded** solo
l'index block e la cache TKIP.

Nota anche il verso opposto: la cattura legge in continuo 0x0768-0x078a, i
contatori delle statistiche, che `b43.h` non nomina affatto, e `b43.h` nomina
0x0700-0x070e come `NPHY_TXIQW*`/`NPHY_TXPWR_INDX*` mentre la cattura non li
tocca. La shared memory dell'AC ha contenuto per cui non ci sono nomi.

### Come gatare

L'asse causale e' l'**ucode**, non il PHY: il layout e' del firmware, e b43 il
precedente lo ha gia' -- `b43_new_kidx_api()` in `xmit.h:372` gira su
`dev->fw.rev >= 351` per esattamente questa famiglia di celle. Il tipo di PHY e'
un buon proxy, disponibile prima che il firmware sia caricato e senza ambiguita'
qui (AC-PHY implica il core nuovo implica l'ucode nuovo), ed e' l'asse che
useremo; ma il motivo per cui la mappa cambia va scritto per quello che e'.

Forma: un accessore per cella invece di una costante, cosi' lo sbaglio sta in un
posto solo -- `b43_shm_sh_keyidxblock(dev)`, `b43_shm_tplram_bt_base(dev, n)` --
e i siti di chiamata non cambiano.

### Le tre celle del core init che si sono chiuse, e le due che no

Chiuse, e non trascritte: `PHYTYPE` (0x0052) e `PHYVER` (0x0050) le scrive ora
il port da `phy.type` e `phy.rev` in `shm_readback_block()`, che e' dove la
cattura le mette; b43 le scrive da se' a `main.c:4932-4933` con quegli stessi
due campi, e la cattura porta `0x0b` = `B43_PHYTYPE_AC` e `1`. `PRMAXTIME`
(0x0074) la scrive `txpwrctrl_setup()` con **0**, che e' il valore di
`b43_chip_init()` (`main.c:3307`). Perimetro ristretto nello stesso passo:
`(0x0050, 0x0056)` diventa `(0x0054, 0x0056)` e la voce di `0x0074` sparisce.

Chiuse anche `SPUWKUP` (0x0094) e `PRETBTT` (0x0096), e il posto dove guardare
non era b43: era **brcmsmac**, il softmac Broadcom aperto nello stesso albero.
`d11.h` conferma le due celle -- `M_SYNTHPU_DLY` e' `0x4a*2` = 0x94 e
`M_PRETBTT` e' `0x4b*2` = 0x96 -- e da la' viene la struttura.

`SPUWKUP` e' una **costante per tipo di PHY**, non un valore da indovinare:
`brcms_b_upd_synthpu()` ne sceglie una di quattro, 3700 per A-PHY, 1050 per
B-PHY, 2048 per N-PHY rev>=3, 300 per LCN. Il 512 della cattura e' il membro AC
della stessa famiglia, e va letto come il 2048 dell'N-PHY: una costante per PHY.
b43 qui sbaglia due volte -- applica il valore B-PHY a tutto e aggiunge un caso
adhoc/idle a 500 che in brcmsmac non esiste.

`PRETBTT`: brcmsmac la definisce e **non la scrive mai**, quindi lascia il
default dell'hardware; b43 scrive 250 in AP e 2 in adhoc, e la cattura porta 2
pur essendo in AP. Lo split 250/2 di b43 e' logica dei core vecchi: qui il
beacon lo costruisce l'ucode dal template in template RAM -- il carico che
`emit_core_bss_ssid()` rispecchia -- quindi l'host non ha niente da preparare e
il preavviso lungo non serve. Il 2 e' il valore di questo core, non il ramo
adhoc.

Lezione di metodo: per le celle del core, prima di dichiarare una costante non
derivabile, guardare brcmsmac. b43 e' il driver in cui stiamo scrivendo, ma non
e' la fonte piu' vicina all'hardware -- brcmsmac condivide con `wl` la
generazione di ucode e le mappe di shared memory, e le costanti stanno la'.

### L'index block e' a 0x05E0, e la codifica non e' cambiata

Trovato: fra le 54 catture con `KTP` leggibile, due dello sweep a caldo --
`02-up-ch36-bw20` e `03-up-ch40-bw20` -- installano chiavi vere. Sono le sole
con scritture non-zero nel tavolo. La sequenza fissa offset e codifica insieme:

```
OBJ.WR 0x05e2 = 0x0015    poi 8 word a ktp+0x10
OBJ.WR 0x05e8 = 0x0045    poi 8 word a ktp+0x40
```

`0x05e2` = `0x05E0 + 1*2` con la chiave a `ktp + 1*16`, quindi kidx **1**;
`0x05e8` = `0x05E0 + 4*2` con la chiave a `ktp + 4*16`, quindi kidx **4**. Slot
di gruppo e primo pairwise (`B43_NR_GROUP_KEYS` = 4): GTK e PTK.

E i valori dicono che la codifica e' identica a quella di b43:
`(1 << 4) | 5` = `0x15` e `(4 << 4) | 5` = `0x45`, cioe'
`((kidx << 4) | algorithm)`. **Si e' spostata solo la base.** L'algoritmo 5 e'
oltre la fine dell'enum di `b43.h` -- AES e' 3, WEP104 e' 4 -- quindi la
numerazione dei cifrari su questo ucode resta da mappare, ma e' un enum e non
cambia dove va la word. E' `patches/0013`.

### E la corsa 0x05e0-0x0666 non e' stato del PHY

Segue da sopra, e va nel verso opposto all'errore trovato prima: `src/` emette
`0x05e0`-`0x0666` come corsa di azzeramento del PHY, e invece **e' l'index
block che viene pulito**. Nella cattura a freddo le due meta' di
`b43_clear_keys()` stanno in fila -- 432 word di materiale a zero da `0x10f4`
(#12311-#12742), poi 68 word da `0x05e0` (#12791-#12858). 54 slot arriverebbero
a `0x064a` e la corsa va a `0x0666`, quindi 14 word in coda restano da
identificare.

Spostarla in `test/unit/main.c` non si puo': `wl` la esegue **dentro** la regione di
channel setup, dove b43 la farebbe in `b43_wireless_core_init()`. E' una
divergenza di posizione, non un doppione da spostare, e resta bloccata dallo
stesso punto d'inserzione che manca a tutto il resto. Per ora e' corretto il
commento, cosi' nessuno la legge come stato del PHY.

### Il tavolo delle chiavi c'e', ed e' meta' di key_write()

`key_write()` (`main.c:826`) fa due cose, e solo una e' sbagliata.

L'index block a `KEYIDXBLOCK + kidx*2` e' hardcoded, e `b43_clear_keys()` lo
gira 54 volte all'attach: arriva a 0x0640, cioe' sulla maschera di catene e
dentro la corsa di azzeramento del PHY.

Il materiale della chiave invece va a `dev->ktp + index*B43_SEC_KEYSIZE`, e
`dev->ktp` lo legge `b43_security_init()` da `KTP` a runtime. **Quel pezzo e'
gia' giusto**, e la cattura lo dimostra riga per riga: `KTP` letta a `#577719`
vale `0x087a`, quindi `dev->ktp` = `0x10f4`, e da la' partono **432 word tutte a
zero**, 432 indirizzi distinti a passo 2 -- 54 slot da 16 byte, esattamente il
ciclo di `key_write()` con `algorithm = NONE`. Nel segmento non c'e' **nessuna**
scrittura con un valore `(kidx<<4)|algo`.

Quindi `wl` su ucode42 fa la pulizia e durante la pulizia non scrive nessun
index block. La correzione dell'attach e' completa e provata: togliere le due
righe dell'index block e lasciare il resto. E' `patches/0013`.

La cattura con la chiave e' poi stata trovata (sezione sopra), quindi anche il
percorso `set_key` e' chiuso a meno dell'enum dei cifrari.

L'asse del gate e' l'identita' dell'**ucode**, come chiesto: `ucode42` esiste
gia' in b43, `b43_request_firmware()` lo seleziona a `main.c:2309` per core
revision 42 con PHY AC, e la cattura porta `WLCOREREV` = `0x2a` = 42. Il test in
`patches/0013` nomina la stessa cosa che nomina la selezione del firmware.
`ucode40`, l'altra voce AC, non ha catture dietro ed e' lasciata fuori di
proposito.

**E niente di tutto questo lo verifica un gate.** Sono modifiche a `main.c` e
`b43.h` del kernel, che l'harness non compila: vanno nella serie `patches/` e
restano senza copertura finche' l'harness non modella il blocco di config
MAC/ucode. Scriverle e' giusto, chiamarle verificate no.

## TODO post-WIP: offload della probe response in hardware

Saltato per il WIP, e la voce di `SOLO_VENDOR` in `test/unit/compare.py` lo dichiara.
Qui c'e' cosa serve per riprenderlo, perche' la voce va togliata insieme.

**Cosa fa b43 oggi.** Non fa rispondere il firmware ai probe: scrive
`PRMAXTIME=1` in `b43_wireless_core_init()` (`main.c:4918`), col commento che un
MaxTime di un microsecondo fa sempre scattare il timeout, *"so we never send any
probe resp"*. Coerentemente le tre celle del template -- `PRSSID` (0x0160),
`PRSSIDLEN` (0x0048), `PRTLEN` (0x004a) -- sono **definite in `b43.h` e mai
scritte**: zero usi in `main.c`. Le temporizzazioni `0x0180`-`0x0186` non sono
nemmeno nominate. Le probe response le costruisce mac80211 in software, quindi
l'AP resta scopribile: si paga in CPU e latenza sotto tempesta di probe, mai in
funzionalita'.

Nota che `b43_chip_init()` (`main.c:3307`) scrive prima `PRMAXTIME=0` con un
`FIXME` -- *"has to be set by ioctl probably"* -- e la 4918 lo riporta a 1.
Nessuno in b43 ha mai deciso davvero, e il vendor scrive 0, cioe' timeout
infinito.

**Cosa serve per implementarlo.**

1. `PRMAXTIME=0` invece di 1, cioe' rimuovere la scrittura di
   `b43_wireless_core_init()` e lasciare quella di `b43_chip_init()`. Attenzione:
   il vendor la scrive **una volta sola**, b43 due. Con l'offload attivo b43
   emette una op in piu' senza controparte, e va guardata prima di dichiararla
   `SOLO_PORT`.
2. Un `b43_write_probe_resp_template()` in `main.c` del kernel, sul modello di
   `b43_write_beacon_template()`, con la word di template RAM a **0x0700**. Non
   nell'harness: il doppione in `test/unit/main.c` e' stato rimosso proprio perche'
   stava modellando `wl` invece di rispecchiare `main.c`.
3. Le quattro temporizzazioni `0x0180`-`0x0186`. Il vendor le scrive due volte,
   a `#1248-1251` (quattro celle) e `#10319-10321` (tre, senza `0x0184`). La
   prima cade **dentro** `b43_phy_ac_op_init()`, fra `bcma_chipco_gpio_control()`
   e `b43_phy_ac_mode_init()`, quindi non a un confine che l'harness controlli:
   servira' un punto d'inserzione o una funzione del core chiamata da la'.
4. Togliere la voce da `SOLO_VENDOR`. Il confronto diventa piu' severo, che e'
   il verso giusto.

**E attenzione a come il punteggio si e' mosso.** Dichiarare l'offload fuori
scopo ha fatto **salire** `cold01` da 98.80% a 99.15%, non scendere: le 197 op
del vendor escono dal denominatore e le 95 del port dal numeratore, quindi il
denominatore perde piu' del numeratore. Il numero e' salito perche' l'obiettivo
si e' rimpicciolito, non perche' il port sia migliorato. E' esattamente il
rischio che il commento di `SOLO_VENDOR` mette in guardia -- "ogni voce e' un
pezzo di obiettivo dichiarato irraggiungibile" -- e qui non e' irraggiungibile,
e' rinviato. Il confronto onesto e' contro il 98.80% di prima, non contro il
99.15% di dopo.

## Gli ultimi due valori di ch36: chiusi

### `PHY 0x08a1`: il coefficiente e' la media per tono, non la somma degli accumulatori

Le due occorrenze di `0x08a1` su `cold01` -- vendor 0x37, port 0x38 -- erano
il coefficiente `b` del core 1 della RX IQ, e il solver le sbagliava perche'
componeva i round nel modo sbagliato. I round di misura sono **toni**: due a
20 e 40 MHz, a `+f` e `-f`, sei a 80 MHz. Quello che si misura su un tono
porta la parte dipendente dalla frequenza dello sbilanciamento; quello che va
in `0x?a0`/`0x?a1` e' la parte indipendente, cioe' la **media sui toni dei
coefficienti per tono**. Sommare gli accumulatori pesa ogni tono con la sua
potenza, e a 80 MHz -- dove il roll-off rende le potenze diverse e le `a` per
tono distano decine di unita', (-5, -16, -4, -18, -2, -19) su `cold24` --
esce un'unita' fuori. E' lo stesso motivo per cui la "media dei `b`" del
modello precedente batteva la somma senza che si sapesse perche'.

I punti sono ora estratti da tutti i segmenti che eseguono la fase, 7 a
freddo e 52 a caldo, con `reverse-tools/rxiq_points.py`: 118 scritture di
`0x?a1`, ognuna con i suoi round. Il tono corto (0x0272 = 0x400 campioni, un
sedicesimo della potenza) e i round della ricerca in loopback (preceduti da
`0x0b22`) non sono misure e restano fuori. Il risultato:

| modello | a esatta | b esatta | entrambe |
| --- | --- | --- | --- |
| somma degli accumulatori (prima) | 108/118 | 86/118 | 80/118 |
| media per tono (ora) | **117/118** | **90/118** | **89/118** |

Le dieci `a` che la somma sbagliava sono tutte a 80 MHz. Il residuo su `a` e'
un punto solo, `13-up-ch60-bw20` core 0: le due `a` per tono valgono -4.592 e
-14.380, la media -9.486, il vendor scrive -10. Nessun arrotondamento della
media lo da', e arrotondare prima i due toni (-5, -14) e mediare con il mezzo
lontano da zero lo da' ma rompe trenta casi di mezzo esatto altrove, dove il
vendor arrotonda verso lo zero. Va lasciato come anomalia di un punto.

Su `b` la forma per tono e' `b_r + 1 = 2^10 * sqrt(qq*ii - iq^2) / ii`, cioe'
la stessa di prima con la `a_r` **esatta** sotto radice invece di quella
arrotondata a Q10: su `cold01` core 1 il secondo tono sta a 57.49, e con la
`a_r` arrotondata passava a 57.50 e saliva a 58. E' quel mezzo LSB a fare il
`0x38` contro `0x37`. La media dei `b_r` e' arrotondata al mezzo in su, che su
due toni coincide con il ceiling che c'era.

Il core 0 del d6220 e' a 56/59, con tre punti bassi di uno. Il core 1 resta a
34/59 con un residuo simmetrico, 13 bassi e 12 alti, e il residuo **segue la
banda**: rispetto a qualunque funzione di questi sei accumulatori il vendor
esce circa 0.7 LSB piu' alto da ch100 in su che sotto i 5250 MHz, sul core 1
soltanto. Non e' il peso di `a^2` sotto radice, non e' la scelta di `a` (per
tono, media, o dell'altro core), non e' la forma della media (aritmetica sui
`b`, sulle potenze `qq/ii`, sui guadagni): provate tutte, la deviazione per
banda non si muove. `rxgainerr5ga*` in NVRAM e' `31,31,31,31` su tutte le
bande, quindi non e' nemmeno quello. Su agcombo la media e' pulita su tutte e
tre le catene, quindi non e' una regola per catena: manca un ingresso che non
sta negli accumulatori, e il `b43_phy_ac_todo()` al sito di scrittura resta
per questo. `VAL_TOLLERANZA` a `+-1` su `0x?a1` resta anch'essa: su `cold01`
non serve piu', sugli altri segmenti copre esattamente questo residuo.

### `TBL 0xc` offset 0x62 e 0x66: chiuso, era un save/restore

La tabella `0x0c` e' la IQLOCAL. A `0x60 + 4*core` stanno i coefficienti TX
IQ `{a, b}` e a `0x62 + 4*core` il word della LO leakage, due byte con segno.
Il valore di `0x62`/`0x66` che il port scriveva come costante non aveva una
formula da trovare: **il vendor lo legge e lo riscrive**. Su `cold01` il blocco
D di `rxiqcal_finalize()` legge `0x60`, `0x62`, `0x64`, `0x66` (`#30664-#30713`)
e la coda dello stesso finalize riscrive parola per parola cio' che ha letto
(`#36474-#36512`). La forma e' la stessa su ogni segmento che esegue la fase --
zero-init, risultato della cal, readback, rewrite del readback -- verificata
su `cold01`, `cold02`, `cold17` e su `01-up-ch36-bw20`:

| segmento | zero-init | cal | readback | rewrite |
| --- | --- | --- | --- | --- |
| ch36 bw20 | 0x0000 | 0xff02 | 0xff02 | 0xff02 |
| ch40 bw20 | 0x0000 | 0xfd02 | 0xfd02 | 0xfd02 |
| ch36 bw40 | 0x0000 | 0x0202 | 0x0202 | 0x0202 |
| up ch36 bw20 | 0x0000 | 0xff01 | 0xff01 | 0xff01 |

Il port faceva la lettura e la buttava in un `discard16`. Ora `rxiqcal_finalize()`
tiene le tre parole per core lette nel blocco D e le riscrive nella coda,
come fa gia' per i registri LO DAC `0x?002-0x?005`. Nessun valore trascritto:
sul ferro e' quello che sta in tabella al momento del salvataggio.

A caldo il rewrite resta sbagliato -- `0xfe02` contro `0xff01` -- perche' e'
sbagliato il risultato della cal che viene salvato, non il salvataggio: quel
debito sta nella prima applicazione, il sito con il `b43_phy_ac_todo()` sui
coefficienti TX IQ/LO scritti da tabella.

### L'harness non modellava la memoria delle tabelle

La stessa lettura, nel flow a caldo, tornava `0xacdc`: la coda dell'oracolo
sulla porta dati `0x000f` e' esaurita la' (1280 code esaurite su
`01-up-ch36-bw20`) e il fallback era `mirror_phy[0x000f]`, cioe' l'ultima
parola passata dalla porta -- la scrittura di `0x5f` che precede il blocco D.
Ogni cella passa dalla stessa porta, quindi l'ultima parola della porta non e'
la cella che si sta leggendo.

`test/unit/wrap.c` porta ora due cose:

- un **mirror per cella**, chiavato `(id, offset)`, aggiornato da ogni wrap di
  scrittura;
- un **oracolo per cella**: le parole sotto un marker `TBL.RD` della cattura
  vanno in una coda per `(id, offset)` e non nella coda piatta della porta,
  cosi' una lettura trova il suo valore anche quando i flow divergono
  altrove. Sta sopra i plan scritti a mano (che portano i valori di una
  corsa sola) e sopra il mirror.

Ordine di servizio di una lettura di tabella: oracolo per cella, plan a mano,
mirror per cella, fallback della porta. Con questo i tre gate a caldo salgono
da 78.47 / 81.38 / 79.25% a **86.26 / 89.50 / 86.14%**, e le regioni su
`01-up` da 930 a 469: prima i readback della cal a caldo venivano dai plan a
mano, cioe' dai valori di `cold01`, ed ogni derivata -- LUT LOFT comprese --
usciva sbagliata. Sul freddo non muove niente -- la' l'oracolo era in sincrono
-- e il periodico resta `MATCH`.

## Il target di potenza, registro `0x0646`: SROM, tetto regolatorio, margine

Il valore e' `min(maxp5ga[grp] - 2*nib, tetto) - 6`, la stessa catena di
`wlc_phy_txpower_recalc_target()` in brcmsmac: il tetto regolatorio si applica
**prima** del margine di 6, e `nib` e' il nibble minimo del campo `mcsbw*po`
della larghezza -- a 80 MHz quello a 20, vedi sotto. Non e' ad anello chiuso, e
sul set corrente la prova e' piu' forte di prima: il valore scritto e' lo stesso
a freddo e a caldo su **tutte e 43 le configurazioni**, confronto fatto segmento
per segmento fra i 43 freddi e i 44 `up`, mentre RX-IQ e idle-TSSI cambiano da
una condizione all'altra su ognuna.

### Il tetto lega in tutte e due le condizioni

Sul set precedente lo sweep a caldo era spiegato dalla sola SROM e il tetto non
serviva: quelle catture giravano col country impostato dallo userspace, un
dominio piu' permissivo della SROM su ogni canale. La ricattura non ha quella
condizione, e il conto sul set corrente e' netto: i 44 `up` scrivono, canale per
canale e larghezza per larghezza, **lo stesso valore dei freddi**, tetto
compreso -- 56 su ch36-48 a 20 MHz, 68 su ch100 a 40, 4 su tutta UNII-3. La
correzione `-2` sul primo blocco a 40 MHz, che era un fit su due punti del
percorso a caldo, non ha piu' nessun punto da spiegare.

### Il tetto e' del locale, non della board

Sui due sweep a freddo -- d6220 (`maxp5ga` 72/70/86, `mcsbw*po` con nibble
diversi per larghezza) e agcombo (74/74/82, nibble uguali) -- il registro
scrive gli **stessi** valori dove il modello SROM darebbe numeri diversi:

| configurazione | d6220 SROM | agcombo SROM | scritto, entrambe |
| --- | --- | --- | --- |
| ch36-48 bw20 | 66/64 | 68 | **56** |
| ch60 bw40 | 64 | 68 | **60** |
| ch100 bw40 | 80 | 76 | **68** |
| ch100 bw20 e bw80 | 80 | 76 | **76** |

Ovunque altro il valore e' la SROM. Il `+2` che ch36 e ch52 a 40 MHz e ch52 a 80
mostravano rispetto al caldo del set precedente non c'e' piu': sul set corrente
le due condizioni coincidono su ogni configurazione, quindi non c'e' nessun
termine del percorso a caldo da spiegare.

Riaggiunto il margine e l'antenna gain che tutte le board dichiarano
(`aga0..2 = 133`, cioe' 5 dB + 2/4 = 22 quarti), i quattro tetti fanno **21,
22, 24 e 26 dBm**, tutti interi: quattro valori tutti congrui a 2 modulo 4 non
sono un caso, e' la forma `EIRP - antgain` di un locale. Il DSL conferma il
meccanismo e non i numeri: sotto wl 6.30 scrive 56 anche su ch52-64 a 20 MHz
e 60 a 40 e 80 su tutta la banda bassa, anche sul down->up -- un locale piu'
vecchio, applicato sempre perche' su quel router nessuno imposta il country.

### Non viene da nessuna lettura, rifatto sugli 87 segmenti

Per ogni segmento del d6220 -- 43 a freddo e 44 `up` -- ho raccolto tutte le
letture con valore che precedono la prima scrittura di `0x0646` (contatori
statistici `0x0768-0x078a` esclusi) e ho cercato un indirizzo il cui valore
letto determini lo scritto. Il test e' funzionale, non di correlazione: stesso
valore letto, stesso scritto, sulle due condizioni insieme.

Gli indirizzi letti in **tutti** e 87 i segmenti prima di quella scrittura sono
173. Ne passa **uno solo**, ed e' quello gia' noto: `RAD 0x08dc`, con 43 valori
distinti sugli 87 segmenti: e' la word di PLL che il
driver stesso scrive dalla tabella canali 84 op prima (`#5029` su `cold01`) e
poi rilegge -- un'eco del canale, e ogni funzione del canale e' "funzione" di
lui. Le correlazioni di rango piu' alte dopo di lui sono flag a due stati che
il driver scrive e rilegge (`RAD 0x0170 = 0x0100` sui soli ch36-48, `PHY 0x0401
= 0x7733`), che marcano la sotto-banda e per questo co-variano con il tetto a
20 MHz, ma ch36 e ch44 a 40 e 80 MHz le leggono uguali e non legano. Le misure
analogiche (TSSI `0x0012/0x0013`, rccal `0x0414/0x0415`) hanno rho fra 0.0 e
0.4 e differiscono fra le due board dove lo scritto e' identico.

L'argomento indipendente dalle letture: stesso chip 4352 del d6220, stessa
configurazione ch52/20, wl 6.30 sul DSL scrive 56 dove wl 7.14 scrive 62. Un
numero che cambia col binario e non col chip sta nel binario: e' la CLM. La
traccia non copre OTP e chipcommon, ma una costante di chip non dipende dal
canale ne' cambia fra freddo e caldo sullo stesso board.

Un residuo che non e' del tetto, emerso dalla tabella: d6220 `ch52 bw80` a
freddo scrive 64 dove il caldo scrive 62, cioe' `+2` sopra la SROM coi nibble
a 20 MHz. Coi nibble di `bw40` (`0x10000000`, min 0) fa 64 esatto, e ch36 bw80
resta esatto con entrambi. A freddo l'80 MHz potrebbe prendere gli offset a 40
invece che a 20: una osservazione, non una regola. **SALAME**.

Che la differenza freddo/caldo del set precedente fosse il country impostato
dopo l'attach resta l'unica spiegazione coerente con le tre board, e non e'
nelle tracce: **SALAME** sul perche', non sul cosa. Sul set corrente quella
differenza non c'e' proprio, il che e' coerente con la spiegazione -- la
ricattura non passa da uno userspace che imposta il country -- e non la
dimostra.

### Il port: e' `recalc_txpower`, con il PPR di b43

Il registro e' `TxPwrCtrlTargetPwr` per core, una **potenza** in quarti di dBm
e non un indice: in phy_n.c e' `pi->tx_power_max` scritto su 0x1ea, e la
catena che lo produce e' `wlc_phy_txpower_recalc_target()`, che b43 porta gia'
come `b43_nphy_op_recalc_txpower()` sul suo `struct b43_ppr`: `clear` →
`load_max_from_sprom` → `apply_max(regolatorio)` → `add(-(6 + antgain))` →
`apply_min(8 dBm)` → `get_max`. Il `6 + antgain` sta in mainline sotto
`#if 0 / TODO: Enable this once we get gains working`: le catture AC dicono che
si puo' accendere, con l'antenna gain sul solo lato regolatorio -- a caldo la
SROM da sola e' esatta 26/26.

`src/ppr_ac.{c,h}` e' quel PPR per la SROM rev 11: stesse primitive, righe
OFDM e MCS a 20/40/80, caricamento dal `min` di `maxp5ga` sulle catene attive
(come fa b43) piu' il delta per core, e i `mcsbw*po` per larghezza. Un canale a
40 carica anche la riga a 20 (20-in-40), uno a 80 anche 20 e 40: il `max` sui
rate prende quindi l'offset piu' piccolo fra le larghezze contenute, ed e' la
forma, non un fit. `b43_phy_ac_txpwr_recalc()` la usa; `recalc_txpower`
restituisce `NEED_ADJUST` solo se il target e' cambiato e `adjust_txpower` lo
scrive sotto `mac_suspend`, come il core b43 si aspetta. Il CRS min power, che
stava nell'hook `recalc_txpower` per errore, e' `pwork_60sec`, stessa cadenza.
Il nibble minimo, `mcsbw5g_po(bw)` per il target e la correzione `-2` "primo
blocco" sono spariti.

Il PPR non serve solo al `max`. Gli offset per rate in SHM
(`prb_rsp_rate_po`) sono la distanza `(max − ppr[rate])·4` sulla tabella
finita, non piu' il nibble grezzo meno il nibble minimo: con il tetto giusto
gli undici residui che il vecchio commento chiamava "bonus" e "sotto maxp"
si muovono nella direzione attesa -- ch100/40 d6220 100→94 valori sbagliati,
agcombo 3029→3020, ch60/40 105→99, ch36/40 e ch36/80 −4 -- e con il tetto
sbagliato peggiorano, che e' la conferma per l'altro verso. `adjust_txpower`
rilancia l'intero `txpwrctrl_setup()` sotto `mac_suspend`, come phy_n. Il
pavimento TSSI-visible e' il piu' piccolo valore non saturato della LUT
est_pwr sulle catene attive, `max` con gli 8 dBm di b43: la funzione del
vendor non e' in nessuna fonte aperta e quella e' la derivazione disponibile
(**SALAME** sul fatto che coincida); su tutte le catture il target sta molto
sopra e non lega.

Cosa cambia sui dati: a freddo tutte le configurazioni sono spiegate, compresa
ch52/80 che coi soli nibble a 20 stava a +2. A caldo il residuo `-2` di
ch52/40 e ch52/80 e' sparito con la ricattura -- misurato sul port: `28-up-ch52-bw40`
e `39-up-ch52-bw80` scrivono `0x40` dove il vendor scrive `0x40`, esatti -- e
quel che resta fuori e' un'altra cosa, la limitazione di cfg80211 descritta qui
sotto.

I tetti del vendor sono per larghezza e cfg80211 porta un `max_power` per
canale da 20 MHz: `reg_ceiling()` prende il minimo sul blocco, che riproduce
i tetti a 20 e bounda 40/80 dove il vendor non lo fa. `gates.sh` esporta
`AC_MAX_POWER_MAP=36:21,40:21,44:21,48:21,100:26` in **tutte e due** le
condizioni, perche' il tetto lega anche a caldo. Il prezzo della limitazione e'
misurato, A/B sui dieci segmenti `up` che la mappa tocca: guadagna 6 op su
ch36, ch40, ch44 e ch48 a 20 MHz, su ch100 a 20 e su ch100 a 80; ne perde 4 su
ch36 e ch44 a 40 e su ch36 a 80, dove il tetto a 20 MHz bounda un canale largo
che il vendor non bounda; ch100 a 40 non si muove. Netto +24 op appaiate sullo
sweep a caldo, e i tre gate di default passano a 91.10, 86.03 e 91.04%. `patches/0001`
dichiara `antenna_gain_qdb` e non lo estrae: su hardware vale 0, la SROM lega
comunque, il tetto e' 5.5 dB troppo permissivo.

`patches/0001` ora estrae anche gli offset espliciti delle righe sub-band
(`sb20in40*`, `sb20in80and160*`, `sb40and80*`, `dot11agdup*`, `mcslr5g*`;
parole 200-218 di `bcmsrom_fmt.h`, GPL, in bcmdhd) e decodifica `agbg0`/`aga0`
in `antenna_gain_qdb[]`. I campi sub-band sono zero su tutte e tre le board:
il loader non li applica, perche' quale nibble vada su quale rate sta in
`wlc_phy_txpwr_apply_srom11`, che non e' aperta; il recalc avverte una volta
se una board li porta non nulli. `ppr_force_disabled` e' `b43_ppr_ac_force_
disabled()`: i rate sotto il pavimento TSSI-visible vanno a zero, non al
pavimento -- il loop non insegue una lettura che non ha.

### Tabella 0x21: non e' il PPR, sono i `pdoffset`

I 24 u32 della tabella 0x21 sono un byte per core (`0x0202` sul d6220,
`0x020202` su agcombo a tre catene -- il vecchio commento leggeva male il
terzo byte). Rimisurata sui 139 segmenti dei quattro sweep -- d6220 freddo e
caldo, agcombo freddo e caldo, tutti la scrivono -- assume **quattro** payload
e non altri. Le voci 1, 5 e 6 valgono 2 su tutte le board fino a UNII-2, e
valgono **3 sul d6220 da ch149 in su**, sui 17 segmenti di UNII-3 che il set
precedente non aveva: e' il nibble del sub-band 3 di `pdoffset40ma = 0x3222`,
che risultava non catturato e ora lo e'. La voce 10 vale 1 su agcombo da ch100
in su a ogni larghezza, 0 altrove e 0 sempre sul d6220. Agcombo e' la
sola board con `pdoffset80ma{0,1,2} = 0x0100`: l'1 sta nel nibble del
sub-band 2, che e' ch100-140 nella partizione pa5g. Le tre board portano
`pdoffset40ma = 0x3222`: 2 sui sub-band 0-2 e 3 sul sub-band 3, ora osservati
tutti. Quindi la tabella e' l'offset del rilevatore di potenza per gruppo
di rate: `pdoffset40ma[core]` nibble del sub-band sulle voci dei gruppi a 40
(1, 5, 6), `pdoffset80ma[core]` sulla voce del gruppo a 80 (10), il resto zero
(la rev 11 non ha un campo a 20 e `pdoffsetcckma` e' zero dove dichiarato).
Il legame a 80 e' misurato su due board e tre sub-band; quello a 40 e' per
stessa codifica, non discriminato dai dati. `patches/0001` estrae anche
`pdoffset80ma`; su agcombo il port perde 6 valori sbagliati per segmento.

Aperti su questo registro: il `-2` a caldo, il `-4` della seconda occorrenza a
freddo sotto i 5250 (vedi sotto), e la regola con cui i campi sub-band si
sommano alle righe, che serve solo a una board dove non sono zero.

### Il campo bw80 non decide questo registro

Le sei osservazioni a 80 MHz, coi nibble di `bw80` contro quelli di `bw20`:

| board | ch | wl | con bw80 | con bw20 |
| --- | --- | --- | --- | --- |
| agcombo | 36, 52, 100 | 68, 68, 76 | 68, 68, 76 | 68, 68, 76 |
| d6220 | 36 | 66 | 62 | **66** |
| d6220 | 52 | 64 | 60 | 62 |
| d6220 | 100 | 76 | **76** | 80 |

Quattro su sei per ognuna delle due scelte, e ch52 sbagliata in entrambe. E il
motivo per cui le sei osservazioni non discriminano e' che **su due delle tre
board le tre larghezze hanno la stessa word**:

| board | 5gl | 5gm | 5gh | bw20 = bw40 = bw80? |
| --- | --- | --- | --- | --- |
| d6220 | 0x20000000 / 0x21000000 / 0x32222222 | ... | ... | **no** |
| agcombo | 0x88644220 su tutte e tre | idem | 0xcca88440 su tutte e tre | si |
| dsl3580l | 0xeca86420 su tutte e tre | 0xcca86420 | 0xcca86420 | si |

Solo la d6220 ha `bw80` diversa, quindi l'unica evidenza utile sono le sue tre
osservazioni a 80 MHz -- e quelle fanno 1 su 3 coi nibble di `bw80` (ch100) e
1 su 3 con quelli di `bw20` (ch36), con ch52 sbagliata in entrambe. Il campo
esiste in NVRAM su tutte e tre le board ed e' letto, ma **non e' lui a decidere
questo registro**, e con questi dati non lo si puo' nemmeno mettere alla prova.

Il ramo a 80 MHz resta comunque quello di `bw80`, e la ragione e' la direzione
dell'errore, non il punteggio: con `bw80` la d6220 esce **sotto** il vendor (62
contro 66), con `bw20` esce **sopra** (80 contro 76). Uscire sopra spinge il PA
oltre quello per cui la board e' caratterizzata; uscire sotto costa portata. A
parita' di 4/6 si sceglie il lato sicuro.

## Nota di metodo: la serie patches/ e' parte dell'albero, non un allegato

Ho passato un turno a scrivere una patch bcma per estrarre `mcsbw{20,40,80}5g*po`
dalla SROM, dopo aver controllato `drivers/bcma/sprom.c` e
`include/linux/ssb/ssb.h` **di mainline** e avere concluso che quei campi non
esistessero. Esistono: li aggiunge `patches/0001`, che e' esattamente "ssb:
bcma: add SPROM revision 11 extraction" e porta
`bcma_sprom_extract_r11()` con tutti e nove gli offset, piu' quelli a 160 MHz,
`maxp5ga[4]`, `pa5ga[12]`, i quattro `rxgains_*`, il blocco FEM/PA,
`subband5gver` e `pdoffset40ma`.

Il che rende falso anche l'audit che avevo scritto qui: non c'erano venti campi
scoperti, era coperto tutto tranne la forma.

Una cosa utile e' rimasta. Gli offset che avevo ricavato cercando le word note
dentro `wl1_srom_raw.txt` delle due board -- `0x0160`, `0x0164`, `0x0168`, poi
`0x0170`/`0x0174`/`0x0178` e `0x0180`/`0x0184`/`0x0188` -- **coincidono uno per
uno** con quelli che la 0001 prende dalla tabella del parser bcmdhd. Due strade
indipendenti, stesso risultato: il metodo "cerca la word nota nel dump e
verifica su due board" e' valido, e la tabella della 0001 e' confermata dai dump
in `router-data/`.

E una divergenza c'era per davvero, nella forma: lo stub dichiarava un
`struct ssb_sprom_mcsbw_po mcsbw5g_po[3]` inventato qui, mentre la 0001 aggiunge
nove campi piatti. Il PHY leggeva `sprom->mcsbw5g_po[band].bw40`, che contro la
struct vera non compila. Ora c'e' `b43_phy_ac_mcsbw5g_po(sprom, band, width)`
che seleziona il campo piatto, e lo stub porta gli stessi nove nomi della 0001.

Regola che ne segue: prima di dire che un campo, un valore o un offset manca,
guardare in `patches/`. Il perimetro del port e' `src/` **piu'** la serie, e
mainline da sola non e' il riferimento.

## Le tre scritture di 0x0646 sono tre giri, e il -4 del secondo ha un ingresso

Misurato su `cold17` (ch36 bw40), dove il pattern e' visibile:

```
#400437   0x0646 = 0x42        ->  TBL 0x40 len=128   (LUT est_pwr)
#400704                            TBL 0x21 len=24    (ppr)
#401111   0x0646 = 0x3e  (-4)  ->  TBL 0x40 len=128
#404880                            --- calibrazione, tono 0x0380 ---
#424205   0x0646 = 0x42        ->  TBL 0x40 len=128
```

Tre giri distinti di `txpwrctrl_setup`, ognuno con la propria LUT. I giri 1 e 2
sono **entrambi prima** della calibrazione, a 569 op di distanza, e in mezzo c'e'
il caricamento di `ppr`. Non e' quindi un bump su `cal_cycles`: e' un
abbassamento temporaneo per il secondo giro, ripristinato al terzo.

### La LUT est_pwr non dipende dal target

I tre payload di `TBL 0x40 off=0 len=128` sono **identici byte per byte** in
tutti e tre i giri, nonostante il target differisca di 4. Quindi la funzione di
trasferimento e' della sola caratterizzazione del PA, non del target -- e questo
semplifica il lavoro sulla LUT: un payload per sotto-banda di `pa5ga`, senza
dipendenza dal punto di lavoro. Il primo campione e' `5d 5d 5d 5d ...`, min 21,
max 93.

### E il -4 e' 2 * l'offset ppr

Il payload di `ppr` (`TBL 0x21 len=24`) e' `0 514 0 0 0 514 514 0 ...`, identico
su **tutti** i segmenti e tutte le larghezze -- che conferma quanto il commento
di `txpwr_target()` gia' diceva, e cioe' che `ppr` non porta informazione di
larghezza. `514` e' `0x0202`, cioe' due byte da 2, uno per core.

Quel 2 e' in mezzi dB, il registro e' in quarti: `2 * 2 = 4`, lo stesso fattore
che il modello usa gia' per i nibble. Quindi il secondo giro scrive
`base - 2*max(ppr)`, cioe' il target del rate **peggiore** invece del massimo,
mentre il primo e il terzo scrivono il massimo. Non e' una costante scelta: e'
il contenuto della tabella caricata fra i due giri.

### Dove si applica, e dove no

| configurazione | tre giri |
| --- | --- |
| freddo, ch36/ch44, bw40 e bw80 | `base`, **`base-4`**, `base` |
| freddo, ch52 e oltre, bw40 e bw80 | tutti uguali |
| freddo, bw20 | tutti uguali (il clamp a 56 li schiaccia) |
| caldo, ogni configurazione | tutti uguali |

Il che chiude la differenza bw20/bw40 che sembrava un fenomeno a se': non lo
era, era lo stesso pattern mascherato dal clamp. Sopra i 5250 non compare, e la'
le calibrazioni non girano; ma a caldo le calibrazioni girano e il -4 non
compare, quindi non e' "quando la calibrazione gira" -- e' il percorso a freddo
sotto i 5250.

### Cosa resta da capire

Perche' il secondo giro prenda il rate peggiore invece del massimo, e perche'
solo la'. E la correzione `-2` del primo blocco, che a caldo si applica e a
freddo no. Sono le due ultime differenze fra il modello e le catture su questo
registro: il resto e' esatto, 26/26 sul caldo.

## Il `4` su `0x0646` a ch149-165, e un tentativo circolare da non rifare

Lo sweep a 43 segmenti aggiunge otto configurazioni UNII-3 -- ch149-165 a 20
MHz, ch149/157 a 40, ch149 a 80 -- e su tutte e otto il vendor scrive **`0x04`**
sul registro target, dove il port scrive `0x50`. E' la prima divergenza di sette
di quegli otto segmenti una volta chiusa la maschera parziale.

`0x04` sono **1 dBm**. Su ogni altra configurazione dello sweep quel registro
porta un numero fra 56 e 80, cioe' una potenza plausibile. Prima di modellarlo
come un target va deciso se lo sia: una banda che il locale non permette e un
marcatore scritto da un altro percorso del vendor spiegherebbero il numero
altrettanto bene, e le catture non contengono il locale (vedi "Non viene da
nessuna lettura, rifatto sugli 87 segmenti").

### Il tentativo, e perche' e' circolare

Provato a farlo uscire dalla catena esistente. Servono tre cose insieme: un
tetto di 8 dBm per quei canali in `AC_MAX_POWER_MAP`, la rimozione del floor a
8 dBm (con cui nessun tetto porta il target sotto 32) e la rimozione del
disable TSSI-visible (che senza il floor azzera tutto). Con le tre il port
scrive `0x04` a tutte e tre le larghezze, e i segmenti perdono 6 valori
sbagliati l'uno.

**Annullato.** Il tetto non e' misurato, e' scelto per far uscire il `4`: e'
l'unica delle cinque voci della mappa che non venga da una configurazione dove
il modello SROM e lo scritto si separino da soli. Da li' in poi la prova che
floor e disable siano di troppo e' che stanno in mezzo a quel `4` -- cioe' il
fit che si chiude su se' stesso. Che i due termini siano fittizi puo' anche
essere vero, ma quei `−6` per segmento non lo dimostrano, e sono termini di
comportamento su hardware, non di punteggio: il floor tiene il target sopra il
minimo utile e il disable spegne i rate che l'anello chiuso non sa inseguire.

L'argomento della forma intera -- `4 + 6` di margine `+ 22` di antenna gain fa
8 dBm EIRP, come 21/22/24/26 degli altri quattro tetti -- e' una consistenza,
non una misura: la forma e' additiva e produce un intero per costruzione ogni
volta che lo scritto e' congruo a 2 modulo 4, che qui vale.

Cosa lo chiuderebbe davvero, in ordine di costo: una cattura del DSL o
dell'agcombo su ch149 (se tutte e tre le board scrivono `4` con `maxp5ga`
diversi, non e' un target); il valore di `0x0846` sul secondo core, che un
target segue e un marcatore no; e i segmenti a caldo su UNII-3, dove il locale
e' quello dello userspace e il numero dovrebbe cambiare.

## La maschera parziale `0x05d6`/`0x05d8` non e' la guardia radar, e' la banda

Seconda divergenza dello sweep per numero di segmenti, otto: ch144-165 a 20
MHz e ch149/ch157 a 40. Il port ci scrive la coppia parziale, il vendor
coremask.

Il conto su tutti e 43 i segmenti, quattro siti per segmento, dice che il
vendor porta una coppia diversa da coremask **solo** su ch36-48 a 20 MHz
(`1`/`1`) e su ch36/ch44 a 40 (`1`/`3`, e `3`/`3` al sito txpwr). Dappertutto
altrove, guardia radar o no, sono quattro `3`.

Il predicato nel codice era `b43_phy_ac_may_calibrate_tx()`, cioe' la guardia
radar. Sui 26 segmenti vecchi i due candidati danno lo stesso risultato ovunque
-- il commento della funzione lo diceva gia', "non si distingue qui da un
semplice sotto i 5250" -- perche' quello sweep non conteneva **nessun canale
senza guardia sopra i 5250**. ch144-165 sono quei canali, e separano i due:
niente guardia, e coremask. Il predicato e' la sotto-banda, gruppo 0 di
`b43_phy_ac_pa5g_group()`.

Il termine "primo bring-up" **non** resta, ed e' l'ultimo pezzo del predicato
che va tolto: sui 44 `up` del set corrente la coppia parziale c'e' -- gli stessi
ch36-48 a 20 MHz e ch36/ch44 a 40 dei freddi, sei segmenti -- mentre sui 52 `up`
del set vecchio era coremask ovunque, ed era quell'assenza a giustificarlo. A
parita' di canale e larghezza i due sweep si comportano uguale, quindi il
predicato e' la sola sotto-banda.

Misurato, e si muovono solo gli otto:

```
cold20 ch144 bw20   98.04% -> 98.09%   valori 284 -> 276
cold21-25 ch149-165 bw20                       -8 ciascuno
cold36-37 ch149/157 bw40                       -3 ciascuno
```

I due numeri sono la forma della divergenza e non un punteggio: a 20 MHz le
celle sbagliate erano due per sito su quattro siti, a 40 una sola, perche' la'
`0x05d8` portava gia' `3`. Totale sui 43 segmenti 10143 -> 10089, nessun altro
segmento mosso, gate periodico a `MATCH`.

### Nota di metodo: i predicati co-varianti dello sweep vecchio

E' il secondo di questa famiglia, dopo quello dentro `may_calibrate_tx()`
stessa (`center_freq <= 5250` invece della guardia radar, vedi
`test/unit/README.md`), e i due vanno in direzione opposta: la' la guardia era
giusta e la frequenza sbagliata, qui il contrario. Non c'e' una regola che dica
quale scegliere; c'e' che sullo sweep a 26 segmenti **nessuno dei due casi era
osservabile**, e ogni punto del driver che oggi guarda la guardia radar o i
5250 MHz va rimisurato sui 43. Il modo di trovarli e' quello usato qui: contare
il valore del sito su tutti i segmenti e guardare come si partiziona, non
leggere il codice.

## L'attesa del CAC: il timer e' un orologio del driver, e le calibrazioni stanno dentro la fase

Ventuno segmenti dello sweep stavano fra il 55% e il 71%, ed erano il blocco di
massa piu' grosso del confronto. Tutti e ventuno hanno la guardia radar e un
check di disponibilita' che si chiude dentro la cattura.

### Cosa NON era

**Non era una fase mancante.** Il primo sospetto -- che il port non emettesse i
giri d'attesa, perche' `cac_wait_ticks` resta zero e nessuno lo produce -- e'
falso, e lo esclude un conteggio di due minuti: le teste di giro
(`PHY.RD 0x07af`) sono **24/24 su cold01, 21/21 su cold14, 124/124 su cold05 e
122/122 su cold09**. I giri li emette gia' la fase probe, e su un canale con la
guardia radar la fase probe *e'* l'attesa. Accendere `b43_phy_ac_cac_wait()`
li raddoppia, ed e' esattamente quello che si misura: 11000 op di troppo per
sbloccarne 12744 di mancanti, con le due che quasi si annullano.

Il conteggio andava fatto **prima** di scrivere un tool per contare i giri
dalla cattura, non dopo.

### Cos'era

Le op mancanti di cold05 sono 13375 e **12744 stanno in una regione sola**, che
comincia tre operazioni dopo il ripristino della riga AMT:

```
#32625  t=594.548   AMT.WR idx=0x3f a3=0x8008    il check si chiude
#33194  t=594.551   PHY.WR 0x140 = 0x0df4        parte il blocco
```

E' la catena di calibrazione intera, che il port non esegue: il gate
`b43_phy_ac_may_calibrate_tx()` legge `!cac_pending`, e `cac_pending` veniva
azzerato in un solo posto, la coda di `cac_wait()`, che non gira. Il contatore
lo conferma: `PHY.MOD 0x02ed` sta 13 volte nel vendor di cold05 e 3 nel port,
mentre su cold01 e cold14 e' 15/15 e 3/3.

### Il timer, e la sua durata

Il check finisce quando il suo timer scade, e nell'harness l'unico orologio
sono i giri del watchdog, che la cattura mette a 1.004 s l'uno dall'altro.
`b43_phy_ac_cac_tick()` fa scadere il check al giro giusto; su hardware quella
transizione e' l'evento `CAC_FINISHED` di mac80211 e la funzione non serve.

La durata e' 60 secondi, 600 nella sotto-banda del radar meteo 5600-5650 MHz, e
il confronto va fatto sul **blocco occupato** e non sul canale primario, perche'
a bloccare e' la banda che si occupa. La regola non e' tarata: scritta cosi', i
segmenti il cui check non si chiude entro la cattura escono esattamente ch120,
ch124 e ch128 a 20 MHz piu' ch116 a 40, ch124 a 40 e ch116 a 80, e nessun altro
dei 43. ch116 a 20 MHz sta sotto i 5600 e si chiude in sessanta secondi; lo
stesso ch116 a 40 e a 80 copre 5600 e non si chiude. Quei sei sono catture
incomplete, non una configurazione diversa, e su questa fase non vanno
misurati.

### Le calibrazioni stanno dentro la fase, non dopo

`b43_phy_ac_calibration_block()` e' estratto dall'orchestratore perche' ha due
chiamanti: `post_switch_calibrations()` quando il canale e' disponibile
all'ingresso, e **il giro della fase probe che chiude il check**, che e' dove
la cattura le mette -- tre operazioni dopo il ripristino della riga AMT, in
mezzo ai giri. La fase riprende dal giro seguente.

Che la posizione contasse lo ha detto la misura, non un'ipotesi. Messo il
blocco *dopo* tutta la fase invece che al giro giusto:

```
                    prima    dopo la fase    al giro giusto
cold05 ch52  bw20   70.66%      59.35%           95.49%
cold09 ch100 bw20   70.81%      59.06%           94.37%
cold28 ch52  bw40   66.27%      79.72%           95.40%
cold39 ch52  bw80   55.06%      74.23%           86.67%
```

A 20 MHz peggiorava di undici punti e a 40/80 migliorava di nove-diciannove:
non era rumore, era lo sfasamento. Dove i giri dopo la chiusura sono tanti
(~66 a 20 MHz) il costo posizionale supera il guadagno, dove sono pochi no. La
divisione per larghezza era il segnale che la posizione contava, ed e' il
motivo per cui quel passo intermedio va misurato e non saltato.

### La chiusura ha un marcatore, e non e' il timer da solo

Le finestre misurate fra l'arm e il ripristino della riga AMT, con i giri a
1.004 s su tutti i segmenti: 62.9 s su diciotto, 67.8 su `cold18`, 67.6 su
`cold42`, 76.4 su `cold34`. Il passo fra i giri e' lo stesso ovunque, quindi a
variare e' la durata del check e non l'orologio, e i 60 s del timer spiegano
diciotto casi su ventuno.

Gli altri tre li chiude un marcatore che il vendor lascia. I giri che portano
un measure block stanno su una griglia regolare, ogni dieci a partire dal nono;
in ogni segmento con la guardia radar c'e' **una voce fuori griglia**, e cade
sul giro in cui il check si chiude:

```
cold05  ..,49,59,[60],69,..   chiude al 60      cold18  ..,59,[63],69,..  al 63
cold34  ..,49,59,69,[70],79   chiude al 70      cold31  [60] .. [78]      al 60
```

Ventuno segmenti su ventuno, ed e' la sola voce fuori griglia che ognuno abbia
prima della chiusura. Il vendor emette un measure block extra quando il check
finisce: quello e' il momento, il timer resta il modello.

Cosa allunghi il check di tre-dieci secondi non sta nelle catture. Un
azzeramento del contatore e' **escluso**: rifarebbe sessanta secondi, non
cinque. Resta un'attesa che si estende -- medium occupato, o un rilevamento che
non porta a cambiare canale -- e nessuna delle due ha una prova. **SALAME** su
quale sia.

### Il giro di anticipo, che valeva piu' dei tre casi anomali

La condizione `turn + 1 >= 60` chiude al giro **59**, non al 60. Un giro di
anticipo su tutta la famiglia, e il conto di quanto e' costato vale piu' della
correzione dei tre segmenti anomali:

```
cold34 ch132 bw40   50.56% -> 97.97%     cold42 ch132 bw80   83.66% -> 97.44%
cold39 ch52  bw80   86.95% -> 99.08%     cold18 ch136 bw20   93.20% -> 97.51%
cold28 ch52  bw40   95.76% -> 99.03%     cold29 ch60  bw40   95.71% -> 98.96%
```

Con questo cade una conclusione scritta qui un giro prima: `cold39` e `cold42`
sembravano bloccati su un nodo di sostanza -- 638 regioni divergenti dentro il
blocco di calibrazione, lette come "il port fa due passate di misura dove il
vendor ne fa sei". Erano lo stesso blocco sfasato di un giro. **Un blocco
grosso fuori posto produce centinaia di regioni che sembrano un problema di
contenuto e non lo sono**, ed e' la seconda volta in questa zona: prima col
dump da trenta secondi, poi qui.

### Il dump da trenta secondi cade sul tick, non sulle teste

Contando le teste `PHY.RD 0x07af` il dump risulta ai giri 27, 57, 87 e 117, e
una regola scritta su quel conteggio lo mette due giri in anticipo: i tick 1 e
2 della fase non emettono la testa, ma il giro c'e' e il secondo passa. Sul
tick la regola e' `(tick + 1) % 30 == 0`, cioe' ogni trentesimo giro, senza
termini di scarto.

Vale la pena sapere quanto costa sbagliarlo: col dump due giri prima, cold05
emette 256 operazioni di cui **sette** combaciano, e il grezzo scende dal
95.50% al 95.03%. Un blocco giusto nel posto sbagliato costa piu' di un blocco
assente.

### Risultato sui 43 segmenti

Tutti e ventuno i segmenti con il check che si chiude salgono, da +14 a +31
punti; i ventidue restanti non si muovono di un'op.

```
op di wl mancanti, totale     278870 -> 27610
valori sbagliati, totale        9645 -> 16196
```

I valori sbagliati **salgono**, e non e' una regressione: dodicimila operazioni
per segmento che prima non venivano emesse adesso ci sono, e una parte porta
numeri che il port non sa ancora derivare. E' il debito che il gate chiuso
nascondeva, ed e' la frontiera nuova. Gate periodico a `MATCH`, cold01
invariato a 95.82%.

### Quel che viene dopo, e adesso e' alla portata

La prima divergenza di quei dieci segmenti resta l'`AMT.WR 0x3f` con i suoi due
`OBJ.BULKR`/`OBJ.BULKW`: la riga dell'address match che il core azzera quando
il check parte e ripristina quando si chiude, tre op per sito e due siti. Prima
non c'era dove metterle; adesso i due confini del check sono modellati -- l'arm
e il giro in cui `cac_tick()` fa scadere il timer -- e sono esattamente quelli.
Restano del core, quindi doppioni in `main.c` accanto a
`emit_core_amt_cac_suspend()`, che di quelle tre occorrenze ne copre gia' una.

### La riga AMT ai due confini, fatta

`b43_ac_cac_match_gate()` e' dichiarata in `phy_ac.h` e definita da chi guida il
flusso, come `b43_ac_beacon_reload()`: l'operazione e' del core -- la riga 0x3f
dell'address match, che il BSS non deve far rispondere finche' il canale non e'
disponibile -- e i due momenti li conosce questa fase. `cac_arm()` la sospende
fra il maskset di `0x02e4` e il primo poll, `cac_tick()` la ripristina quando
il timer scade. Sei operazioni per segmento, e la prima divergenza dei dieci
segmenti che l'avevano si sposta.

### Il dump da 128 byte e' un periodico da 30 secondi

La prima divergenza che viene fuori dopo, su cold05 a `@16915`, e' un
`OBJ.BULKR addr=0x00e0 len=128` seguito dalle 64 letture di `0x00e0-0x015e`:
65 operazioni, che il port non emette affatto. Cade fra il cambio di modo e la
testa della spazzata, cioe' fra il pezzo 5 e il pezzo 1 dell'alfabeto del giro.

Cade ai **giri 27, 57, 87, 117** su ogni segmento che ci arriva. Col battito di
1.004 s e il primo giro a +3.6 s dall'ingresso della fase, sono i 30, 60, 90 e
120 secondi: un periodico da trenta secondi contato da li'. La regola

```
dump = giri > 27 ? (giri - 27) / 30 + 1 : 0
```

torna su **tutti e 43 i segmenti**, senza eccezioni: 4 sui dodici a 20 MHz che
hanno 122-125 giri, 2 su quelli a 80 giri, zero sui ventuno che restano sotto
i 28.

Una cosa che la regola **non** distingue: se il periodico sia di ogni canale o
solo di quelli con la guardia radar. I segmenti che arrivano a 28 giri sono
tutti e soli quelli con la guardia -- gli altri stanno a 21-24 -- quindi le due
ipotesi danno lo stesso conto su tutto lo sweep e questa cattura non le separa.
Il conteggio dei giri e' la piu' semplice delle due e non ha un termine in piu',
quindi e' quella da scrivere, ma la cosa va annotata: un segmento lungo su un
canale senza guardia la metterebbe alla prova, e non c'e'.

Da fare: emetterlo fra il pezzo 5 e il pezzo 1 di `b43_phy_ac_wd_turn()`. Il
dump e' il marcatore bulk piu' le 64 parole, che e' la forma con cui la cattura
traccia una lettura di regione, non 64 letture separate.

### La fase del dump era il giro d'ingresso contato due volte — chiuso

Emesso il dump, su `cold01` ne usciva uno che il vendor non ha: 64 op di
troppo, che erano **tutto** il debito di op di troppo del segmento e la prima
divergenza posizionale, a `@28213`.

Il conto dice dov'era. Misurando su tutti e 43 i segmenti l'indice di giro del
measure block e del dump, 42 danno `meas=[9, 19, 29, …]` e `dump=[29, 59, 89,
119]`; `cold01` da' `meas=[10, 20]` e nessun dump. E' l'unico sfasato di uno, e
l'unico in cui il giro sintetico che `timeline.py` inserisce prima del primo
cambio di modo dista 1 ms invece di 1.0-1.3 s: li' la testa e il cambio di modo
sono **una callback sola del vendor** -- il giro d'ingresso -- che il driver
spezza su due eventi dell'harness. Da quel punto `wd_turns` correva un giro
avanti a quello del vendor. `AC_WD_PHASE=9` rimetteva a posto il measure block,
che e' mod 10, e non il dump, che e' mod 30; e `timeline.py`, che il residuo lo
cercava fra `phase`, `phase+10` e `phase+20`, con 27 giri non ne trovava
nessuno che tenesse il dump fuori e usciva dal ciclo lasciando quello che ce lo
metteva.

La fase, del resto, veniva dalla posizione del measure block, che l'evento in
piu' aveva gia' spostato: anche il predicato che sceglieva il giro d'ingresso,
`wd_turns % 10 == 9`, era quindi circolare, e sui segmenti a caldo scattava a
caso -- su `05-up` e `10-up` (fase 19) si', su `01-up` (fase 21) no.

Chiuso cosi': il giro d'ingresso e' mezza callback e **non incrementa
`wd_turns`**, e a sceglierlo e' `@wd_entry_turn`, che viene dalla cattura come
`probe_ticks` e le ricariche del beacon. Il testimone e' l'ordine delle quattro
celle di testa -- `0x010e 0x010c 0x0158 0x015e` d'ingresso contro `0x010e
0x0158 0x010c 0x015e` a regime -- e `timeline.py` lo legge e lo passa come
`AC_WD_ENTRY_TURN`; la fase la calcola un giro piu' indietro dove c'e'.

Il censimento dice che non e' un caso isolato, e correla con quando arriva il
primo callback:

| | giro d'ingresso | spazzata sola | distanza dalla coda del bring-up |
| --- | --- | --- | --- |
| 44 `up` a caldo | 44 | 0 | 0.000-0.001 s |
| 43 a freddo | 1 (cold01) | 42 | 0.001 s / 1.001-1.334 s |

Sugli 87 segmenti non c'e' una eccezione: forma piena se e solo se il callback
cade entro il millisecondo. Che sia perche' a caldo il timer da un secondo gira
gia' dal ciclo precedente, mentre a freddo parte col driver e `cold01` e'
l'unico dove il tick era gia' scaduto, e' la lettura naturale ma non e'
dimostrata.

Risultato. `cold01` da 99.68% a **99.89%**, 64 op di troppo a zero e
`compare.py` senza divergenze sul prefisso -- le due tracce differiscono solo
per le tre op in coda. I tre gate a caldo salgono tutti, perche' li' il
predicato vecchio sbagliava: `01-up` 91.77 -> **93.55**, `05-up` 93.51 ->
**95.60**, `10-up` 91.77 -> **94.12**. Gli altri 42 segmenti a freddo non si
muovono di un'op -- con `AC_WD_ENTRY_TURN=0` il percorso e' lo stesso e la fase
non cambia -- e il gate periodico non cambia.

Trappola da non ripetere: il primo censimento dell'ordine delle celle di testa
sui segmenti a caldo diceva 44 su 44 a regime, cioe' l'opposto. Era lo script
usa-e-getta, che non aveva il fallback di `gates.sh` per quando
`strip_other_core.py` esce con errore e quindi rileggeva il file del segmento
precedente. Chi rifa' quel conto a mano copi il fallback.

## L'header BSS `0x00cc`-`0x00d0`: cosa si sa, su cinque catture

Il blocco che le contiene e' un header di configurazione per-BSS che il driver
stagia in shared memory e che la ucode travasa in un colpo solo. Dodici
operazioni contigue, sempre nello stesso ordine:

```
OBJ.BULKW 0x0020 len=4 ; 0x0020=0 ; 0x0022=0     puntatore template MAC/BSSID
OBJ.WR 0x0012 = 0x0003
OBJ.RD 0x00cc -> due scritture, la seconda alza il bit 0
OBJ.WR 0x00ce = <offset di potenza del beacon>
OBJ.WR 0x00d0 = 0
OBJ.WR 0x001c ; 0x001e
```

Subito dopo vengono il template del beacon, la sua lunghezza e quello della
probe response: tutto quel che l'header mette in scena appartiene al percorso
beacon.

### `0x00cc`: i bit 6-8 sono la maschera delle catene

Misurato su tutte e cinque le catture del repo -- d6220 freddo e caldo,
agcombo freddo e caldo, dsl3580l -- correlando ogni scrittura con quel che il
blocco chain-mask aveva appena messo su `0x05d6`/`0x05d8`:

```
maschera in forza     campo bit 6-8
      1 / 1                 1
      3 / 3                 3
      1 / 5                 1
      7 / 7                 7
```

Coincide sempre, e **segue la maschera parziale** dove quella e' in forza: su
cold01 il campo vale 1 mentre `coremask` vale 3. Quindi il valore da mettere
non e' `coremask` ma l'ultima maschera che `b43_phy_ac_chainmask_block()` ha
programmato, che oggi il driver non ricorda. Non e' un dato di board:
l'agcombo arriva a `7` perche' ha tre catene, non perche' sia un'altra board.

Restano opachi il bit 0, alzato da una seconda scrittura dopo il payload, e il
bit 2, alzato su ogni cattura.

### `0x00d0`: campo vivo, mai esercitato

**849 scritture su tre board, freddo e caldo: sempre zero.** La ucode lo legge
e lo affetta a bitfield, quindi non e' padding; ma nessuna delle condizioni che
queste catture coprono lo accende. E' l'unico zero dei tre, ed e' cosi' che il
sorgente lo dichiara in `b43_phy_ac_down()`: se si trova una condizione che lo
accende, il sito va rivisto.

### `0x00ce`: chiuso, e' la potenza del beacon sulla riga a 20 MHz

Q-format con LSB 1/16 dB, ed e' la stessa forma del campo per-rate `+0x0e`:
distanza del rate del beacon -- il 6 Mbit, che sta su mcs0 -- dal massimo della
tabella PPR, presa pero' **sempre** sulla riga a 20 MHz e non su quella della
larghezza operante. Nel driver e' `b43_phy_ac_beacon_pwr_offset()`, che il
`wlc_beacon_phytxctl()` del vendor rispecchia.

La misura, sui due sweep a freddo, confrontando la cella col campo per-rate del
6 Mbit dello stesso segmento:

| sweep | coincide | non coincide |
| --- | --- | --- |
| d6220, 43 segmenti | 37 | ch52/40, ch36/80, ch52/80, ch100/80, ch116/80, ch149/80 |
| agcombo, 26 segmenti | 24 | ch36/80, ch52/80 |

Le otto eccezioni sono tutte e sole configurazioni a 40 e 80 MHz, cioe' quelle
dove la riga a 20 e quella operante divergono: e' la previsione della formula,
non un residuo.

Quel che le scansioni non trovavano si spiega cosi': candidavano celle `OBJ`,
`TPL`, `PHY` e `RAD` osservate nella traccia, e questo valore viene dalla
tabella PPR, che il driver calcola dalla SROM e non scrive in nessuna cella
tracciata. Cade con la formula anche la dipendenza dalla board -- a parita' di
canale e larghezza due schede hanno PPR diverse -- quindi la misura sulla
dsl3580l non serve piu' a decidere niente e non e' rimasta come debito.

Due cose misurate qui che vanno corrette rispetto a com'erano scritte:

- **il passo fine non esiste**. Su tutte e cinque le catture ogni valore
  scritto e' multiplo di `0x08`, cioe' di mezzo dB; `0x68` sull'agcombo e' 6.5
  dB, non 4.25;
- **la variazione fra le passate di uno stesso segmento non e' di questa
  cella**. Dove la cella cambia da una passata all'altra -- ch36/40 e ch44/40 e
  ch36/80 sul d6220, ch36/40, ch44/40, ch108/40, ch132/40 e ch36/80
  sull'agcombo -- cambia nella stessa passata anche il campo per-rate del 6
  Mbit. Sul d6220 dello stesso scarto, sull'agcombo no, quindi fra una passata
  e l'altra non si muove solo il massimo della tabella.

Resta aperto un solo pezzo: il campo mascherato `0x700` che
`wlc_beacon_phytxctl()` sovrappone al valore. Non si e' mai visto acceso --
nessuna delle cinque catture scrive un valore `>= 0x100` -- quindi non si sa
cosa sia e il driver non lo emette.

## Il campo per-rate `+0x0e`: la riga della larghezza, e il tetto su maxp

Misurato sullo sweep a freddo intero, 43 segmenti, prima passata di
`prb_rsp_rate_po` su ognuno. Su 40 dei 43 le tre passate scrivono gli stessi
valori; su ch36/40, ch44/40 e ch36/80 la seconda passata differisce, e
differisce su tutti e dodici i rate -- `-0x10` sui quattro CCK, `+0x18` sugli
OFDM a 40 MHz e `-0x18` a 80. E' la passata in cui il registro target `0x0646`
scende di 4, vedi la sezione sui tre giri di `0x0646`, quindi la prima passata
e' la base giusta su cui misurare il campo. I quattro rate CCK combaciano
ovunque; divergevano i dodici campi OFDM su venti segmenti, ed era la prima
divergenza su undici di essi.

Il campo e' `(massimo - ppr[rate]) * 4`. Due termini erano sbagliati.

**La riga.** I rate legacy stanno sulla riga della **larghezza operante**, non
sulla riga a 20 MHz. Un OFDM legacy su un canale aggregato va in aria
duplicato su tutto il blocco, quindi spende il budget della larghezza intera:
la ragione non viene dai dati, e i dati la confermano. Contro la riga a 20 MHz
il modello e' esatto su 24 segmenti su 43, con la riga della larghezza su 28, e
non e' mai peggiore -- i quattro in piu' sono ch52, ch100, ch132 e ch149 a 80
MHz.

**Il tetto.** Il tetto regolatorio entra su `maxp`, prima che si sottraggano
gli offset, non sulle righe finite. Tagliare le righe finite satura fra loro i
rate che starebbero sopra il tetto, e su ch100 a 20 MHz questo perde il gradino
di testa degli offset che il vendor scrive. **Lo decide quella configurazione e
basta**: ch100/20 e' l'unico punto dello sweep dove un tetto lega su una riga i
cui nibble non siano tutti uguali -- su ch36-48 lega su `mcsbw205glpo =
0x20000000`, che e' piatta, e un clamp su una riga piatta non si vede. Una
configurazione, una board.

Il `maxp` restituito dal caricatore resta quello **non** tagliato, perche' il
riporto per core del chiamante e' una distanza dal `maxp5ga` di quel core.

Fatto senza stato in piu': nessuna seconda tabella, il target e gli offset
escono dalla stessa e quindi sono coerenti per costruzione. Misura, prima e
dopo, su tutti e 43 i segmenti:

| segmento | valori sbagliati |
| --- | --- |
| cold09 ch100 bw20 | 242 -> 233 |
| cold28 ch52 bw40 | 116 -> 92 |
| cold30 ch100 bw40 | 151 -> 148 |
| cold39 ch52 bw80 | 262 -> 238 |
| cold40 ch100 bw80 | 277 -> 253 |
| cold42 ch132 bw80 | 395 -> 380 |
| cold43 ch149 bw80 | 620 -> 611 |

Nessun altro segmento si muove, gate periodico a `MATCH`, cold01 invariato.

### Nota di metodo: il primo modello era un fit su due punti

La prima versione di questa correzione prendeva la riga a 20 MHz e come
riferimento il massimo di quella sola riga, giustificato da ch52 a 40 e a 80
MHz -- i due soli punti delle tre SROM dove le righe di larghezza differiscono.
Riproduceva quei due e lasciava quattro segmenti a 80 MHz che spiegava con una
saturazione. La riga della larghezza riproduce gli stessi due **e** quei
quattro, senza aggiungere niente. Due punti bastano a inventare una regola e
non a sceglierla: la prova che serviva era confrontare i due candidati su tutti
e 43 i segmenti insieme, che e' costato dieci minuti e non e' stato fatto
prima.

### Quel che resta: una saturazione, e non si sa cosa la fissi

Su 13 dei 15 segmenti che ancora divergono il vendor scrive
`max(distanza, K)` con K costante fra i rate del segmento:

| K | segmenti |
| --- | --- |
| 1 dB | ch104-144 bw20, ch60 bw40 |
| 2 dB | ch116 bw80 |
| 3 dB | ch36 bw80 |

La forma e' un tetto a `massimo - K`, ma non quello regolatorio, e non una
funzione della larghezza ne' della banda: ch100, ch104-144 e ch149-165 a 20 MHz
condividono parola `mcsbw205ghpo` e sotto-banda `maxp5ga`, e i K sono 0, 1 dB e
0. Cosa distingua ch104-144 dai suoi due vicini non sta in nessuna grandezza
che il driver abbia: **SALAME** su ogni spiegazione.

I due che non prendono nemmeno quella forma sono **ch100 a 40 MHz**
(`0x10 0x10 0x10 0x18 0x28`) e **ch100 a 80**: a 40 MHz i due gradini di coda
valgono 1.5 e 2.5 dB dove ogni altra configurazione ha multipli di mezzo dB da
un nibble.

Sbagliare questo campo di un dB non impedisce nessuna fase dell'attach: vale
per il punteggio e per il TX power reale, che e' post-MVP.

## Punti aperti

> **Le voci di questa sezione sono state rifatte sul set corrente, tranne
> l'inversione del PPR.** Quei conteggi -- `10/52`, `52/52`, `39/52`,
> `41/52` -- sono sulle 52 configurazioni del set precedente, 26 per board;
> oggi il d6220 ne ha 43. Rifarli vuol dire pilotare `ppr_invert.py` su ogni
> configurazione nuova, e non c'e' in repo un'impalcatura che lo faccia: i
> numeri di allora vengono da un giro a mano. La conclusione -- il massimo
> preso fuori dal gruppo OFDM legacy -- non dipende dal conteggio, la sua
> forza si'.

- **I LED sono del core, e il gpio 10 b43 non lo conosce.** Il blocco
  `GPIO.CTL`/`OUT`/`OE` a maschera `0x407` che il vendor intercala nel
  preambolo freddo, i due toggle `0x0004`/`0x0400` al bss-up e al down e il
  rilascio al rmmod sono, nel blob `wlD6220.o`, `wlc_bmac_hw_up()` (con
  `si_gpioled()`, cioe' la scrittura di `gpiotimeroutmask` a chipcommon
  `0x8c`, che il confronto scartava come ombra), `wlc_bmac_led()` e
  `wlc_bmac_led_hw_deinit()` via `wlc_led_deinit()`. La maschera e' dato di
  board: SROM `ledbh0-3` a `0xff` danno i default di wl su gpio 0-2, NVRAM
  `ledbh10=0x88` da' il gpio 10 active-low. Sul PHY zero effetto, quindi il
  port non li emette piu' e stanno nel `PERIMETER` di `compare.py`; il port
  li emetteva come `frontend_gpio_setup`, che ora si chiama
  `cold_mac_preamble` e tiene solo MHF, MACCTL e la richiesta PMU.

  Il debito era del core e sta in `patches/0016-0017`: `leds.c` copriva
  `gpio0-3` dalla SROM per la sua via (MMIO `GPIO_CONTROL` del MAC, non
  `si_gpioout` sul chipcommon) con uno slot per ruolo, e un LED su gpio 10 da
  variabile NVRAM non aveva ne' campo SPROM ne' slot. La 0016 porta
  `ledbh4..15` in `struct ssb_sprom` (`gpio_ext[]`): li legge
  `bcm47xx_sprom` con il prefisso del device, e **bcma li aggiunge sopra la
  SROM letta dalla scheda** -- sul D6220 il 43b3 ha la sua SROM rev 11 e il
  fallback NVRAM non gira mai, quindi leggerli solo la' non serviva a
  niente. La 0017 passa gli slot a un pool per pin -- sul D6220 gpio 0 e gpio 10 sono entrambi
  activity, e uno slot per ruolo ne accendeva uno solo -- e fa instradare al
  MAC da `b43_gpio_init()` i pin dei LED registrati. Compila e gira in
  `test/integration`, che da allora compila anche `leds.c` e `rfkill.c` con il
  profilo D6220 (`ledbh0-3=0xff`, `ledbh10=0x88`): `GPIO_MASK` e `GPIOCTL` a
  `0x40f`, le scritture di `GPIO_CONTROL` in init ed exit. Su hardware non
  provata.

  Nella coda del rmmod resta un'op che nessuno dei due emette:
  `SI.COREREG core=0 off=0x80 val=4` e' `pcie_watchdog_reset()` da
  `si_detach()` -> `pcicore_deinit()` sui core PCIe Gen2 (`0x83c`): arma il
  watchdog del chipcommon a 4 tick, aspetta 100 ms, ripristina la config
  PCIe. E' un reset del chip al detach, del layer bus, e bcma non ha un
  equivalente.

- **Il poll delle statistiche ha due forme; il parametro c'e', il chiamante
  no.** Su `cold01` i 23 poll che iniziano con `0x010e` si dividono cosi': i
  primi tre -- `#12961`, `#13481`, `#13540`, dentro il blocco di config MAC del
  core -- hanno **18** letture in `0x0768-0x078a`, cioe' la sola spazzata
  piatta; i venti dalla fase probe in avanti ne hanno 54, cioe' la spazzata piu'
  le due passate `hi/lo/hi` sui sei contatori a 32 bit. Uno ne ha 57.

  `b43_phy_ac_wd_stats_poll_opt(dev, ctr32_pass)` prende il parametro, e
  `b43_phy_ac_wd_stats_poll()` resta come involucro che passa `true` -- che e'
  la forma giusta per il tick a regime, e il gate periodico lo conferma
  (`MATCH`). I tre poll ridotti cadono nel blocco di config MAC, che l'harness
  non modella, quindi oggi nessun chiamante passa `false`: il parametro e'
  pronto e serve quando quel blocco verra' modellato.

  Il senso e' plausibile e non provato: le due passate leggono i contatori come
  valori a 32 bit stabili, e prima che il MAC abbia contato qualcosa non c'e'
  niente da leggere in quel modo. La spazzata piatta resta perche' fa parte del
  latch della finestra.

  Il poll con 57 letture, tre in piu' della forma piena, era un contatore in
  piu' letto una volta: e' `0x077c`, letto `hi/lo/hi` da solo dopo il latch di
  chiusura e fuori da ogni spazzata, fra le due letture di `UCODESTAT` che sono
  del core. Lo emette ora `b43_phy_ac_rxiqcal_finalize()`, e su `cold01` i
  conteggi di `0x077c` e `0x077e` combaciano op per op (66 e 109). La stessa
  tripletta c'e' identica sul segmento a caldo, a `#28587-#28591`.

- **Il punteggio ora penalizza le op in piu', e il quadro cambia.** Il
  denominatore e' l'unione dei due flussi, non le sole op di wl: fa 100% solo
  se coincidono. Prima le inserzioni del port erano gratis, il che rendeva
  invisibile l'unico tipo di progresso che si stava facendo -- togliere ~6000
  op spurie non muoveva il numero di un'unita'.

  Il conteggio e' in tre voci, non due, perche' due difetti diversi si
  confondevano in uno. Un'op emessa sul registro giusto col valore sbagliato
  compare due volte nel diff -- manca la versione di wl e sopravanza quella del
  port -- e contarla come "una mancante piu' una di troppo" gonfia il difetto e
  lo chiama col nome sbagliato: il registro e' quello giusto, il numero no.
  `classify()` in `cmp_skip.py` le accoppia per identita' dentro la stessa
  regione sostituita, e l'identita' non e' sempre l'indirizzo: alcune op
  generano altre op tracciate, e alcune classi scrivono su **porte**, che hanno
  sempre lo stesso indirizzo.

  Le porte sono `PHY 0x000d`/`0x000e` (indirizzo delle tabelle) e
  `0x000f`/`0x0010` (dati, word bassa e alta). Una `TBL.WR` e' un marcatore: i
  dati escono come una corsa sulla porta, una coppia per voce. Due scritture su
  `0x000f` non sono la stessa op solo perche' l'indirizzo coincide -- coincide
  sempre -- e possono appartenere a tabelle diverse, quindi la loro chiave
  include il marcatore `TBL` che le precede.

  Relazioni controllate e per cui non serve regola, perche' le classi generate
  non compaiono nelle catture del d6220: `TPL.RAMW` -> `TPL.PTRW`/`TPL.DATW`
  (zero occorrenze delle seconde) e `OBJ.BULKW` -> `OBJ.WR` (la coppia bulk non
  e' agganciata la'). Una che c'e' ma non e' un pericolo per l'accoppiamento:
  `MAC.MHF` scrive la cella `HOSTF` in cinque casi su undici, quindi un MHF
  mancante porta con se' una `OBJ.WR` mancante -- due op tracciate che contano
  due, ma che non si accoppiano fra loro perche' le classi sono diverse.

  | segmento | grezzo | valore sbagliato | mancanti | di troppo |
  | --- | --- | --- | --- | --- |
  | cold01 ch36 bw20 | 98.75% | 3 | 355 | 0 |
  | cold05 ch52 bw20 | 85.03% | 48 | 749 | **1804** |

  Su cold01 le op di troppo sono ormai zero: erano l'ombra dei tre poll di up
  e un bracket MAC duplicato, entrambi chiusi sotto. Dei valori sbagliati, il payload TX IQ/LO
  resta il debito dominante. Sulla forma corta invece le op di troppo sono
  reali e sono la voce dominante.

  **Le 93 di cold01 erano l'ombra di tre poll mancanti nel path di up, e sono
  chiuse.** Escono tutte da `b43_phy_ac_rxiqcal_finalize` (attribuzione con
  `AC_FN_MARKERS=1`), in un blocco contiguo di 91 op piu' due coppie isolate,
  e il blocco e' riga per riga un ciclo intero di
  `b43_phy_ac_wd_stats_poll_opt` in forma piena.

  Il censimento dei 23 poll della cattura, per forma:

  | poll | thread | forma |
  | --- | --- | --- |
  | 3 (`#12961`, `#13481`, `#13540`) | cpu1, 754.75-754.78 | sola spazzata: 18 letture `0x0768-0x078a` |
  | 20 (`#30919`-`#35059`) | cpu0, 762.3-781.0 | forma piena: 54 letture, coda, latch, clear |
  | 1 (`#35321`) | cpu0, 782.3 | piu' lunga: 57 letture, 32 hi/lo, clear assente |
  | 1 frammento (`#12944`) | cpu1 | sola coda, senza testa |

  I tre a spazzata sola non sono misure del PHY: stanno su `cpu1` in una
  raffica di 24 ms nel path di `up`, fra le scritture di config MAC che li
  circondano -- parametri WME `0x05d4-0x05da`, limiti di retry
  `0x01f0-0x01fc`, `MAC.MHF`, `TPL.RAMW` -- mentre i venti pieni stanno su
  `cpu0` a un secondo di distanza l'uno dall'altro. Le celle lette sono i
  contatori macstat, non le celle TSSI `0x0725`/`0x0925`.

  Il port non li emetteva, e `b43_phy_ac_wd_stats_poll_opt(dev, false)` -- la
  forma corta, che il sorgente implementa da sempre -- **non aveva chiamanti**:
  ogni chiamata passava da `b43_phy_ac_wd_stats_poll()`, che passa `true`. Il
  deficit che ne seguiva era per indirizzo e non un solo scalare (3 su testa e
  `0x0780-0x078a`, 4 sui `lo` dei contatori, 5 sugli `hi`, 7 su `0x077e`, 1-2
  sulle celle di coda), e l'oracolo delle letture -- FIFO per indirizzo -- da
  quel punto serviva al port il valore dell'occorrenza sbagliata: i valori
  divergenti rompevano la corsa di uguaglianza e producevano le 93 alla
  cucitura, con 58 op vendor cancellate nella regione adiacente.

  Che quelle op le emetta `src/` e' legittimo: non le fa il core b43, nessuno
  ne consuma il valore, e per la posizione nel flusso appartengono a
  `set_channel`. Gli ancoraggi sono op che entrambi i lati emettono, e sono
  senza ambiguita':

  | poll | dopo | prima di |
  | --- | --- | --- |
  | `#12961` | `mhf_maskset(dev, 0, ~0x4000, 0)` | il peek di `TBL_WRITE_GATE` (#39179) |
  | `#13481` | `b43_mac_enable()` (#39191) | `mhf_maskset(dev, 4, ~0x8000, ...)` (#39192) |
  | `#13540` | `b43_mac_suspend()` (#39199) | `maccontrol_set(~0x10000000u, 0x10000000)` (#39200) |

  Incondizionate, non sotto `FIRST_BRINGUP`: i segmenti `up` dello sweep a
  caldo hanno anch'essi 3, 3 e 4 poll a spazzata sola, quindi il vendor li
  emette anche sui bring-up successivi.

  Il bracket MAC va aperto una volta sola:
  `b43_phy_ac_rxiqcal_measure_block()` chiudeva il blocco con
  `mac_enable` + `mac_suspend` -- dichiarati "arm del probe cycle successivo"
  -- mentre entrambi i chiamanti lo circondavano gia' con
  `mac_suspend`/`mac_enable`. Il port emetteva quindi `enable, suspend, enable`
  dove il vendor ha un solo `enable`, due volte per segmento: sul tick di
  misura e sul tick di chiusura. Il wrapper senza quelle due op non aggiungeva
  nulla a `b43_phy_ac_measure_block()`, quindi e' stato rimosso e i due
  chiamanti chiamano il blocco direttamente. Il tick periodico non passava da
  quel wrapper, e infatti resta a `MATCH`.

  Nota per chi tornera' qui: `PERIMETER` e' la leva sbagliata per questa
  famiglia di celle. Il port emette `0x0768-0x078a` nei suoi poll, quindi
  scartarle dal lato vendor lascerebbe venti cicli senza controparte -- il caso
  che `test/unit/README.md` avverte di non creare.

  **Il numero di poll a spazzata sola non spiega niente, perche' non varia**, e
  il censimento, rifatto su tutti e 43 i segmenti a freddo, lo chiude: la
  sequenza delle forme e' sempre `18, 18, 18`, poi i poll da 54 letture, poi
  **uno** da 57. Tre corti e un lungo su ognuno dei 43, senza forme intermedie e
  senza eccezioni, a ogni canale e a ogni larghezza. Quel che varia e' il numero
  dei poll pieni, e varia molto piu' di prima -- da 21 a 127 invece che da 18 a
  21 -- perche' i segmenti DFS ripresi col CAC completato durano molto di piu'. Il conteggio si rifa' con

      per ogni poll, che apre su 0x010e: quante RD in 0x0768-0x078a
      prima del poll successivo

  Una versione precedente di questa sezione dava 5 poll corti su cold03, 8 su
  cold04 e 4 su cold05, e ne concludeva che le op di troppo residue di quei
  segmenti fossero poll non emessi. Quei conteggi non si riproducono: sono 3
  ovunque. Le 28 op di troppo di cold03, le 70 di cold04 e le 84-96 dei due
  bw40 sono quindi ancora da attribuire, e non e' qui che vanno cercate.

  Resta invece aperto, e ora si sa che e' universale, il poll da 57 letture:
  tre in piu' della forma piena, cioe' la lunghezza di una lettura `hi/lo/hi`,
  quindi un contatore in piu' letto una volta. Uno per segmento su tutti e 43.

  `SOLO_PORT` in `compare.py` e' la lista delle op del port che l'oracolo non
  puo' contenere. Ci sta una voce, `AMT.*`: il port scrive la address match
  table per via di `patches/0011`, ricavata dalla cattura a freddo del
  DSL-3580L, e le catture del d6220 non la hanno perche' l'hook su
  `wlc_bmac_write_amt` e' stato aggiunto dopo. **Non e' un'op di troppo: e'
  un'op giusta senza oracolo**, e ci resta finche' non c'e' un retrace del
  d6220 con quell'hook -- quel giorno la voce va togliata e il confronto
  diventa piu' severo.

  I casi legittimi per quella lista sono due e vanno distinti: un'op che b43
  deve fare per la sua struttura dove wl ne fa una diversa, che e' permanente,
  e un'op giusta la cui controparte esiste ma non e' stata catturata, che e'
  temporanea per definizione. Tutto il resto e' il port che fa qualcosa di
  troppo.

  Verificato che sia l'unica: delle op che il port emette per via del DSL,
  `0x078c` (il MAC in shared memory, `patches/0010`) e le dieci celle di
  `patches/0012` hanno tutte controparte nelle catture d6220 -- e' la' che sono
  state trovate. Le AMT sono le sole senza.

  `INVISIBILI`, che esisteva solo per le AMT e le scartava da entrambi i lati,
  e' stata rimossa: diceva "nessuna cattura puo' contenerle", che e' falso --
  una cattura con l'hook le contiene.

- **Il muro a `@11375` e' un confine di responsabilita', non un'op mancante.**
  Da cold01 #13593 il vendor emette configurazione BSS dentro il channel setup:
  `0x0160-0x0166` contiene l'SSID (`test-ap` in ASCII), `0x0018` e `0x004a` sono
  le lunghezze dei template, con le `TPL.RAMW` di `0x0200` e `0x0700` accanto.

  **Non vanno dichiarate in `CORE_SHM`.** Il criterio non e' "concettualmente
  del core" ma "il core le emette, e nel punto giusto", e qui:

  - `0x0018` e' `B43_SHM_SH_BTL0` e `0x004a` e' `B43_SHM_SH_PRTLEN` in `b43.h`,
    e b43 le scrive -- ma dal percorso dei template, quando mac80211 aggiorna il
    beacon, non dentro il channel setup;
  - l'SSID a `0x0160` non ha simbolo in `b43.h`: b43 non lo scrive in shared
    memory affatto, in AP mode sta nel template beacon.

  Escluderle nasconderebbe uno sfasamento reale e non farebbe avanzare il
  posizionale di un'op, perche' b43 le emette in un altro momento. Quindi da
  qui in avanti il posizionale non chiede altre op del PHY: chiede che l'ordine
  del core combaci -- la stessa classe dei tre poll di up in cima a questo
  documento.

  **E sono dentro lo scopo**, benche' siano fasi da AP: il traguardo e' la
  configurazione completa piu' il beacon in aria, quindi beacon, template della
  probe response e SSID vanno emessi. Il vincolo di posizione e' reale e non
  arbitrario: il MAC e' sospeso da #13539 fino all'enable di #13670, e tutta
  questa configurazione cade dentro quella finestra -- l'ucode legge SSID, PLCP
  e lunghezze quando trasmette, quindi devono essere a posto prima che il MAC
  riparta.

  b43 ha la finestra giusta: `b43_op_config()` gira interamente fra
  `b43_mac_suspend()` e `out_mac_enable`, e dopo il ritorno di
  `b43_switch_channel()` fa ancora i limiti di retry e la potenza. Il posto e'
  quello. Ostacolo noto: `b43_update_templates()` e' solo la meta' superiore --
  chiede il beacon a mac80211 e rinvia le scritture all'IRQ, perche' scrivere
  mentre il firmware trasmette manderebbe fuori un beacon invalido. Con il MAC
  sospeso quel rischio non c'e', quindi serve la meta' inferiore sincrona
  (`b43_upload_beacon0/1` piu' l'equivalente per la probe response), con i flag
  `beaconN_uploaded` gestiti in modo esplicito e non azzerati a mano.

  Guadagno collaterale: il `len` provvisorio di `b43_phy_ac_prb_rsp_plcp()`
  viene da `0x004a`, che il core scrive con la lunghezza del template. Su
  hardware si legge da la' invece di essere una costante per larghezza.

- **Il blocco di config MAC dopo l'init radio: aperto per un pezzo.** Sul set
  precedente il muro del posizionale era questo blocco su tutti e 26 i
  segmenti, ma non allo stesso punto, e la differenza diceva quale lavoro
  viene prima:

  | famiglia | segmenti | prima divergenza | vendor | port |
  | --- | --- | --- | --- | --- |
  | A | 7, tutti UNII-1 (ch36/40/44/48, bw20/40/80) | `@10214-10220` (`@12780` a 80 MHz) | `OBJ.WR 0xd4 = 0x88` | `MAC.MHF slot 0 mask 0x4000` |
  | B | 19, da ch52 in su | `@9599-9622` | `OBJ.RD 0x92` | `PHY.RD 0x140` |

  La B era l'ingresso del blocco -- `OBJ.RD 0x0092` e' l'apertura di
  `b43_phy_ac_shm_readback_block()` -- e il port ci arrivava in ritardo perche'
  stava ancora eseguendo le calibrazioni che il vendor la' saltava. Quindi la B
  era un sintomo anticipato del gating mancante, non un muro distinto.

  **Le due famiglie sul set corrente non esistono piu'**, e si legge dalla
  tabella dei punteggi in "Lo sweep a freddo intero": i muri stanno fra `@6574`
  e `@25799` e si raggruppano per banda e larghezza, non piu' in due blocchi --
  `@25619-25799` sui quattro UNII-1 a 20 MHz, `@16729-16935` su ch52-64,
  `@10752-10762` su ch100-140, `@11359-11400` su ch144-165 e sui 40 MHz bassi,
  `@6574-13919` sugli 80. Il gating delle calibrazioni e' stato chiuso nel
  frattempo, quindi la famiglia B e' andata: il ricalcolo delle famiglie va
  rifatto da capo su questi muri prima di attribuirli a un blocco.

  **La A e' il write-through delle host flag, ed e' bloccata sui dati.** Il
  vendor non rilegge mai le cinque celle HOSTF -- **zero** `OBJ.RD` su
  `0x005e`, `0x0060`, `0x0062`, `0x0078`, `0x00d4` in tutta la cattura --
  quindi tiene uno shadow in memoria e ne scrive la cella solo in 5 chiamate su
  38, piu' un flush di tutte e cinque a `#686-#690`. `b43_phy_ac_mhf_maskset()`
  in `src/helpers_phy_ac.c` fa invece read-modify-write sulla cella a ogni
  chiamata: e' una differenza strutturale, non un blocco di scritture
  mancanti. Nell'harness la cosa e' nascosta perche' `helpers_phy_ac.c` non e'
  nel link di test e `wrap.c` modella la funzione, col tappo che ci sta dentro
  dichiarato come tale.

  La condizione **e' stabilita**, e non e' `bands`: l'equivalente GPL in
  brcmsmac (`brcms_b_mhf()`, `brcm80211/brcmsmac/main.c`) scrive la cella solo
  se

  ```c
  if (wlc_hw->clk && (band->mhfs[idx] != save) && (band == wlc_hw->band))
  ```

  cioe' clock su, **shadow cambiato**, e banda modificata uguale alla corrente.
  Il discriminante che mancava e' l'ottimizzazione "solo se cambia". Il modello
  (clock su da una soglia in poi) AND (shadow cambiato) predice **tutte** le
  chiamate di entrambi i testimoni: 38 su 38 su cold01 del d6220 e 56 su 56 sul
  DSL, con la soglia a `#623` e `#424` rispettivamente. Le due catture sono di
  versioni diverse del blob (7.14 e 6.30) e danno la stessa partizione:
  scrivono `(slot0, 0x100)`, `(slot4, 0x08)`, `(slot4, 0x8000)`, `(slot3,
  0x20)` -- piu' `(slot1, 0x20)` sul solo d6220 -- e non scrivono i clear ne'
  le riscritture dello stesso bit. `bands` non serve: in queste catture ogni
  chiamata e' sulla banda corrente.

  **Fatto.** Lo shadow sta in `struct b43_phy_ac` (`mhfs[5]` piu'
  `mhf_writethrough`), `b43_phy_ac_mhf_maskset()` scrive la cella solo quando
  la word cambia e non la rilegge mai, e il flag passa a vero fra la chiamata
  slot 4 e la slot 0 del blocco GPIO frontend, dove le catture mettono la
  transizione. E' stato dichiarato che lo shadow duplica stato che in b43
  sarebbe del core: il vendor ne tiene molto, e se serve al PHY sta nel PHY.
  Nell'harness il tappo di `wrap.c` e' stato rimosso e `helpers_phy_ac.c` e'
  entrato nel link, cosi' la decisione di scrivere e' quella del driver e non
  un modello parallelo; il wrapper emette il solo record `MAC.MHF` e delega a
  `__real_`. Corroborazione: allo scatto del flag lo shadow vale
  `{0x100, 0, 0, 0x40, 0x80}`, gli stessi cinque valori del flush del core.

  **Lo shadow sopravvive a un down/up e si azzera solo col ricaricamento del
  modulo**, e le catture lo provano: il flush di un `up` a caldo scrive i
  valori accumulati dal ciclo precedente -- `0x0060` in HOSTF4 e `0x0088` in
  HOSTF5 (`01-up-ch36-bw20` `#29158-#29162`) -- dove un ciclo a freddo flusha
  `0x0040` e `0x0080`. Siccome `b43_wireless_core_init()` chiama
  `prepare_structs` a ogni `up`, il `memset` di
  `b43_phy_ac_op_prepare_structs()` porta lo shadow attraverso il reset; quello
  che lo azzera e' la `kzalloc` di `op_allocate()`. Nell'harness il freddo e'
  giusto da se': `mount_board()` azzera `g_ac` a ogni processo, cioe' un modulo
  ricaricato.

  **Trappola per il confronto a caldo, da chiudere quando il caldo tornera' un
  target.** Il caldo e' don't care finche' il primo setup a freddo non e'
  confermato; niente da togliere nel frattempo, vedi il README. Ogni
  invocazione dell'harness e' un processo nuovo, quindi lo shadow parte sempre
  a zero: un flusso `switch_channel` o `up` confrontato con un segmento dello
  sweep a caldo emetterebbe i write-through del blocco GPIO frontend, che la
  cattura non ha perche' la' lo shadow era gia' accumulato. Serve un seme per i
  run non-primo-bring-up, sulla stessa leva di `AC_FIRST_INIT`, col valore in
  cui un ciclo a freddo termina.

  **Fatto anche RFATT**: `OBJ.WR 0x0064 = 0x0480`, subito dopo il
  write-through di HOSTF5. Rimisurato sul set corrente: `0x0480` una volta per
  segmento su tutti e 43 i freddi e tutti e 44 gli `up`, e identica sul DSL
  nella stessa posizione. Trascritta: a
  cosa serva su un AC-PHY non e' noto.

  **Fatti i due azzeramenti.** Nel blob sono caricamenti di tabella da rodata,
  e le tabelle sono tutte zeri: un ciclo le sostituisce senza perdere niente,
  quindi non c'e' nessuna tabella da trascrivere.

  | blocco | word | episodi cold01 |
  | --- | --- | --- |
  | `0x10f4-0x14b2` | 480 | `#12311-#12790` |
  | `0x05e0-0x0666` | 68 | `#12791-#12858` |

  Contigui fra loro, una volta per attach, subito dopo RFATT, e identici su
  ogni segmento controllato a ogni canale e larghezza. Le prime dieci word del
  secondo cadono nel `KEYIDXBLOCK` che `b43.h` dichiara del core, ma il vendor
  le scrive in quella corsa e non altrove -- ogni cella compare una volta sola
  nella cattura -- quindi la corsa e' riprodotta per intero e la voce
  `CORE_SHM` e' stata ristretta a `0x05d4-0x05de`, come la regola di
  `test/unit/README.md` prescrive quando il port impara a scrivere una cella.

  **Il muro ora e' `0x0020 = 0x0800` seguito da `0x08ec-0x0a8e`**, e cambia
  natura: quei blocchi portano valori, non zeri. Decorrelati con
  `reverse-tools/decorrelate_channels.py`, le 72 del blocco si dividono cosi'.

  **La categoria `dinamico` oggi non e' rilevabile su nessuno dei due sweep.**
  Rilanciato sul set corrente: sui 43 segmenti a freddo escono 1782 chiavi --
  1480 invarianti, 151 centro-freq, 103 solo-larghezza, 12 solo-canale, 36
  stesso-insieme -- e sui 44 `up` 1770, con la stessa forma. **Dinamiche: zero
  in entrambi.** Un valore dinamico si rileva confrontando due segmenti con la
  stessa etichetta `(canale, larghezza)`, e nel set corrente ogni etichetta
  compare una volta sola per condizione: era lo sweep a caldo precedente, 26
  configurazioni per due cicli, l'unico dataset che lo permetteva, e non c'e'
  piu'. Finche' non si ricattura una configurazione ripetuta, "funzione di
  canale e larghezza" e "risultato di calibrazione" non si distinguono con
  questo strumento.

  | classe | chiavi | cosa serve |
  | --- | --- | --- |
  | invariante | 45 | letterali, fra cui `0x0020`, `0x08ec`, `0x0910` |
  | solo-larghezza | 15 | una funzione della larghezza |
  | centro-freq | 12 | una funzione della frequenza centrale |

  Le solo-larghezza stanno a passo `0x14` (`0x0994`, `0x09a8`, `0x09bc`, ...) e
  fanno `0x238b -> 0x23ab -> 0x23cb` per 20/40/80, cioe' `+0x20` per passo sul
  byte alto; le celle adiacenti (`0x0998`, `0x09ac`, ...) cambiano solo a 80
  MHz, `0x01a4 -> 0x01a8`.

  Le celle a passo `0x1c` sono il **gruppo B** gia' aperto sopra, e il muro
  posizionale si ferma la', a `OBJ.RD 0x0a3a` seguita dalla sua riscrittura.
  Non sono le soglie CRS di `crs-min-power.md`, che stanno sui registri PHY
  `0x0321-0x0336` e sul banco `0x0910`: un altro spazio di indirizzi.

  **E' una funzione di canale e larghezza, non una misura.** Lo dimostrava lo
  sweep a caldo precedente, 26 configurazioni per due cicli: era l'unico dataset
  in cui la categoria `dinamico` fosse rilevabile -- ne trovava 123 su 1737
  chiavi -- e queste dodici celle ne restavano fuori, con i due cicli della
  stessa configurazione a dare lo stesso valore. Sul set corrente restano
  `centro-freq` in entrambe le condizioni, ma la prova negativa non si puo'
  rifare: vedi il punto qui sopra.

  I valori sono campi su bit `[7:3]`, cioe' interi a 5 bit con segno. Per
  `0x0a3a` (le altre tre della famiglia sono sempre identiche):

  | bw | ch36 | ch40 | ch44 | ch48 | ch52 | ch56-64 | ch100+ |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | 20 | -1 | -1 | -2 | -2 | -3 | -3 | -1 |
  | 40 | -2 | | -1 | | -3 | -2 (ch60) | -1 |
  | 80 | -1 | | | | -3 | | -1 |

  Due cose che la forma **non** e': non e' monotona nella frequenza centrale
  (5190 da -2 mentre 5180 e 5200 danno -1; 5210 da -1 e 5220 da -2), quindi
  l'etichetta `centro-freq` dice solo "dipende dalla combinazione"; e non e' per
  sottobanda, perche' dentro UNII-1 a 20 MHz cambia a ch44 e `subband5gver=0x4`
  non ha quel confine. La famiglia a passo `0x14` ha valori positivi (0, 1, 2,
  6) e le sue ultime due celle divergono dalle altre sei sui canali alti.

  **Origine trovata: sono i blocchi per-rate della shared memory, e le tre
  voci che questo documento teneva separate -- le quindici solo-larghezza, le
  dodici centro-freq e il gruppo A del read-back -- sono una cosa sola.**

  L'indirizzo non e' una costante. Ogni accesso e' preceduto dalla lettura di
  una voce della direct-map table, che e' un puntatore:

      blocco = 2 * shm_read(DIRMAP + indice * 2)

  cioe' `brcms_b_rate_shm_offset()` di brcmsmac. `M_RT_DIRMAP_A` sta a `0x01c0`
  e `M_RT_DIRMAP_B` a `0x0200`. L'indice non e' il numero del rate: e' il
  nibble basso del campo SIGNAL del PLCP, per cui 6, 9, 12, 18, 24, 36, 48 e 54
  Mbit/s stanno agli indici 11, 15, 10, 14, 9, 13, 8 e 12 -- ed e' per questo
  che i puntatori sembravano letti fuori sequenza. I passi `0x14` e `0x1c` che
  sembravano struttura sono la spaziatura dei blocchi su questa board.

  Offset toccati dentro il blocco, su cold01:

  | offset | op | volte | cosa e' |
  | --- | --- | --- | --- |
  | +8, +10, +12 | solo WR | 80 ciascuno | tre word, i sei byte del PLCP |
  | +14 | RD+WR | 36 | 12 rate per 3 passate |
  | +18 | RD+WR | 16 | 8 rate OFDM per 2 passate |

  I nomi `M_RT_*` di brcmsmac **non** si possono trasferire: quello scrive il
  PLCP a +10 e +12, due word, il blob tre word a +8. Il layout del blocco AC e'
  spostato, quindi chiamare +18 `OFDM_PCTL1` perche' la' sta a 18 sarebbe una
  deduzione dal nome e non dai dati.

  **Fatto: +14 per gli otto rate OFDM.** Il valore e' il nibble di `mcsbw*po`
  del MCS su cui il rate legacy ricade -- 6, 9, 12, 18 su mcs0; 24 su mcs1; 36
  su mcs2; 48 su mcs3; 54 su mcs4 -- moltiplicato per 8, con l'indice di banda
  che `b43_phy_ac_po_band()` gia' calcolava. Esatto su 12 delle 26
  configurazioni a freddo; le altre 14 differiscono per un termine additivo per
  canale, non nella forma, e quel termine non e' identificato.

  **La forma della regola e' sbagliata, e phy_n la da'.** La catena e'
  quella del PPR: massimo per rate dalla SROM, tetti regolatori, tetti per
  gruppo (`b43_ppr_apply_max_group`, 68 quarti di dB a una catena e 56 a due,
  i 12 di differenza essendo `10*log10(2)`), pavimento
  `b43_ppr_apply_min(..., INT_TO_Q52(8))`, e infine
  `tx_power_offset[i] = max(ppr) - ppr[i]`. Il valore e' una **distanza dal
  massimo**, non un nibble, e i dodici blocchi scritti dal vendor sono i primi
  due gruppi del PPR -- `cck[4]` e `ofdm[8]` -- con la partizione che combacia
  cella per cella. Il nibble diretto coincide con la differenza solo dove i
  nibble sono quasi tutti zero, che e' esattamente l'insieme delle 12
  configurazioni in cui la scorciatoia passa.

  Ne segue che il "pavimento" e il "termine additivo" cercati qui sotto non
  sono termini nuovi: sono l'effetto di `apply_max_group` e di `apply_min` su
  una differenza. E i quattro CCK sono un gruppo a se' nel PPR, senza campo
  SROM per 5 GHz nella rev 11, quindi il loro valore e' determinato da tetto e
  pavimento e da nient'altro -- che e' la board-independence misurata su tre
  schede.

  **La catena e' verificata sui dati AC, invertendola.** Con
  `ppr[i] = maxp5ga[sb] - 2*nib[i]` e campo `(max(ppr) - ppr[i]) / 2` -- il
  grezzo essendo quello per 8:

  | | maxp | ppr per rate | tetto | max | campo atteso | osservato |
  | --- | --- | --- | --- | --- | --- | --- |
  | d6220 ch100 bw20 | 86 | 86,86,86,86,86,82,78,74 | nessuno | 86 | 0,0,0,0,0,2,4,6 | idem |
  | agcombo ch100 bw20 | 82 | 82,82,82,82,74,74,66,66 | 74 | 82 | 4,4,4,4,4,4,8,8 | idem |

  Da cui tre risultati. L'unita' e' mezzi quarti di dB. La catena e' quella del
  PPR e non il nibble. E **il tetto non e' il 68 di phy_n**: sul d6220 nessun
  tetto minore o uguale a 86 puo' mordere, altrimenti ch100 uscirebbe piatto,
  mentre sull'agcombo il tetto e' 74 e morde. Due board, due tetti, nessuno dei
  due ereditabile come costante -- il regolatorio del vendor non e' uniforme
  fra le combinazioni.

  Nota sul massimo: sull'agcombo `max = 82` non e' raggiunto dal gruppo OFDM
  legacy dopo il tetto, quindi `max(ppr)` e' preso su **tutti** i gruppi, MCS
  compresi, come `b43_ppr_get_max()` sull'intera struttura da 44 rate.

  **`max` non e' vincolabile al massimo del solo gruppo OFDM legacy**: cosi'
  tornano 10 punti su 52, con 27 incoerenti e 15 degeneri -- degenere vuol dire
  che il tetto appiattisce tutti i rate e i dati fissano solo `max - tetto`.
  L'inverter va usato con `max` libero, e `reverse-tools/ppr_invert.py` enumera
  i tetti compatibili distinguendo i tre casi invece di restituirne uno: un
  inverter che ricavi `max` da un'assunzione ha due parametri liberi per otto
  punti e da' una coerenza vacua.

  **E il confronto fra le due versioni e' il risultato che serviva**: con `max`
  libero tornano 52 punti su 52, col massimo del gruppo ne tornano 10. Quindi i
  dati *richiedono* che il massimo venga preso fuori dal gruppo -- non e' piu'
  un'osservazione su un caso singolo, e' una separazione su 42 configurazioni.
  `b43_ppr_get_max()` su tutti e sette i gruppi non e' un'opzione di
  modellazione. **Ma costruirli non basta**, e provarlo e' stato utile: in tutti
  e sei i campi `po` delle due board il nibble minimo e' portato da `mcs0` o il
  campo e' uniforme, quindi il gruppo che vince il massimo e' sempre quello
  SISO e il suo massimo e' `maxp - 2*min(nib)`, cioe' quello che si calcolava
  gia'. Il grado di liberta' non era quale gruppo.

  Il blocco e': **due incognite per configurazione, `max` e tetto, e otto
  osservazioni che nei casi appiattiti vincolano solo la differenza.** Aggiungere
  configurazioni non lo risolve, perche' ognuna porta le proprie due incognite:
  serve una misura indipendente di una delle due.

  E c'e': il **target di potenza per core**, registro `0x0646 + stride`, che
  `b43_phy_ac_txpwr_target()` calcola come `lim - 6` con `lim` limitato dal
  tetto regolatorio. E' nelle tracce, canale per canale, ed e' il tetto
  misurato. Da la' `max` non e' piu' incognito e il tetto esce dagli otto campi.
  **Estratto, e separa due tetti che erano confusi in uno.**

  | | reg 0x0646 | lim = reg+6 | max risolto | tetto risolto |
  | --- | --- | --- | --- | --- |
  | d6220 ch36 bw20 | 0x38 | 62 | 72 | nessuno |
  | d6220 ch100 bw20 | 0x4c | 82 | 86 | nessuno |
  | agcombo ch100 bw20 | 0x4c | 82 | 82 | 74 |

  Primo: il `max` dei campi e' il valore SROM **non tagliato**. Sul d6220 a
  ch100 `maxp` e' 86 col nibble minimo a 0, e i campi richiedono 86, mentre il
  registro da' 82, cioe' 86 gia' limitato. E' la conferma indipendente che nella
  catena dei campi `get_max` viene prima dei tagli.

  Secondo: il tetto che agisce sui campi **non e'** quello regolatorio.
  Sull'agcombo a ch100 il registro da' `lim = 82` con `maxp = 82`, quindi il
  regolatorio non morde, mentre i campi richiedono 74. Sono due quantita'
  distinte: il registro porta il limite regolatorio, i campi il **tetto di
  gruppo**, l'`apply_max_group` di phy_n -- 68 la', 74 sull'agcombo. Trattarle
  come una sola e' la ragione per cui i residui non si chiudevano.

  **Con `max` imposto dalla SROM resta una sola incognita, e tornano 45 punti su
  52** (`reverse-tools/ppr_invert.py --model srom`). I tetti di gruppo risolti:

  | | bw20 | bw40 | bw80 |
  | --- | --- | --- | --- |
  | d6220 | ch104-140: 82 | ch60: 66 | ch36: 56, ch52: 62 |
  | agcombo | ch100-140: 74 | ch36/44: 62, ch60: 66 | ch36: 62, ch52: 70, ch100: 66 |

  Tutti pari e spaziati di 4; le larghezze legate hanno tetti sistematicamente
  piu' bassi delle strette, la stessa aritmetica dei 3 dB; e il 56 del d6220 a
  ch36 bw80 e' esattamente il tetto a due catene di phy_n.

  **I 7 residui sono tutti casi in cui `max` deve essere post-taglio.** Su
  agcombo ch36-48 bw20 i campi sono tutti zero, cioe' tutti i rate al massimo,
  mentre `ppr` mette i rate da 24 in su piu' in basso: impossibile con
  `M = 74` imposto, e risolvibile solo con `M = 66`, che e' il valore tagliato.
  Sul d6220 a ch104 invece `M` e' 86, pre-taglio. Quindi l'ordine fra
  `get_max` e `apply_max_group` non e' lo stesso nelle due situazioni.

  **Check del registro fatto su tutte e 52: l'ipotesi dei due massimi non
  regge.** Confrontando il massimo che i campi richiedono con quello della SROM
  e con `lim = reg(0x0646) + 6`: 33 casi in cui i due candidati coincidono e
  quindi non discriminano, 6 che seguono la SROM, 2 che seguono `lim`, e **11
  che non seguono nessuno dei due**. In quegli undici il massimo dei campi e'
  una terza quantita'.

  **Ma il check da' un risultato piu' forte: il tetto regolatorio non agisce
  sui campi SHM.** Sul d6220 a ch36 bw20 il registro da' `lim = 62`, cioe'
  `maxp = 72` limitato dal regolatorio, mentre i campi richiedono `M = 72` e
  nessun taglio. Se il regolatorio entrasse in quel percorso i campi si
  appiattirebbero. I due percorsi divergono **prima** dello stadio regolatorio:
  il registro di target lo applica, i blocchi per-rate no.

  Resta quindi in piedi la sola ipotesi messa da parte: il massimo cade sul
  gruppo che vince fra i sette, con tetti di gruppo diversi. Gli undici casi
  «nessuno» sono compatibili -- su agcombo ch36-48 serve `M = 66`, che e'
  esattamente `min(ppr)` del gruppo, cioe' tutto appiattito al tetto. Per
  distinguerla servono davvero i sette gruppi, ed e' l'unica strada rimasta.

  Ipotesi provate e cassate per i residui, per non rifarle: sottobanda indicizzata
  per frequenza secondo `txpwr_subband()` (39/52, identico); massimo preso dal
  campo `po` a 20 MHz invece che da quello della larghezza operativa (39/52,
  residui diversi).

  **Invertita con `max` libero su tutte e 52 le configurazioni, due board:** I nibble di `mcsbw*po` vanno letti **senza segno**: con la
  conversione a intero con segno `0xcca88440` dell'agcombo darebbe backoff
  negativi e il conto non chiude. Il tetto risolto per configurazione, a 20
  MHz, dove «>=» vuol dire che non morde e si vede solo un limite inferiore:

  | | ch36-48 | ch52-64 | ch100 | ch104-140 |
  | --- | --- | --- | --- | --- |
  | d6220 | >=72 | >=68 | >=86 | **82** |
  | agcombo | **66** | >=74 | **74** | **74** |

  Da cui: il tetto e' **per canale**, non per sottobanda -- sul d6220 a ch100
  non morde e a ch104-140 vale 82, dentro la stessa sottobanda -- ed e'
  dipendente dalla board. I valori sono limiti plausibili in dBm: 66, 74 e 82
  quarti fanno 16,5 / 18,5 / 20,5 dBm, cioe' il tetto regolatorio meno il
  guadagno d'antenna. Non e' una costante da ereditare da phy_n.

  E due fenomeni che questa sezione inseguiva non esistono:

  - la "soppressione" sulla sottobanda bassa dell'agcombo e' il tetto a 66 che
    appiattisce tutti i rate: finiti tutti al tetto, `max - ppr` e' zero per
    tutti;
  - l'"anomalia" del d6220 a ch52-64, con l'osservato sotto il predetto, e'
    `5gmpo = 0x11111111`: nibble tutti uguali, quindi `max` coincide con ogni
    `ppr` e le differenze sono zero.

  **`max` e' il massimo prima dei tetti, non dopo.**
  `max = maxp5ga[sb] - 2*min(nibble)` combacia su 39 delle 52 configurazioni, e
  su tutte quelle a 20 MHz di entrambe le board tranne agcombo ch36-48; la
  variante col tetto applicato, `min(a, tetto)`, ne prende solo 25. Quindi
  l'ordine nell'AC non e' quello di phy_n, dove `b43_ppr_get_max()` viene dopo
  `apply_max_group`: qui il massimo e' quello della SROM prima del taglio. Lo
  conferma il caso che sembrava richiedere un altro gruppo -- sul d6220 a ch104
  `max` vale 86 mentre il gruppo OFDM e' tagliato a 82.

  Dei 13 residui, **11 sono a 40 e 80 MHz**, dove e' incerta la scelta stessa
  del campo `po` e della sottobanda: le due partizioni sono dichiarate
  indipendenti in `src/phy_ac.c` -- `b43_phy_ac_po_band()` per canale e
  `b43_phy_ac_txpwr_subband()` per frequenza e dipendente dalla larghezza --
  mentre `reverse-tools/ppr_invert.py` usa la stessa per entrambe. **Provata:
  non e' quella** -- con `maxp` indicizzato per frequenza secondo
  `b43_phy_ac_txpwr_subband()` il conto resta 39/52, identico.

  Quello che i residui dicono e' un'altra cosa. Le differenze fra risolto e
  predetto sono `+4, +4, +4, +4, +4, +12, -2, -8, -8`: quasi tutte multipli di
  4 quarti di dB, cioe' **1 dB esatto**, e concentrate sulle larghezze legate.
  Un campo `po` sbagliato darebbe scarti irregolari; questo e' un termine
  additivo in dB che entra a 40 e 80 MHz -- la stessa aritmetica dei 3 dB che
  `phy_n` mette fra gruppi a una e due catene. Da cercare li'.

  I due residui a 20 MHz sono invece agcombo ch36-48, dove il risolto e' 66,
  cioe' esattamente il tetto: la' il tetto entra anche nel massimo, e questo
  resta senza spiegazione.

  **La regola sembrava incompleta per un pavimento.** Il testimone da usare qui e'
  l'agcombo, wl 7.x, non il DSL, che a 6.30 e' troppo vecchio per validare una
  formula per-rate. E l'agcombo mostra la forma esatta del difetto:

  | | osservato | predetto |
  | --- | --- | --- |
  | agcombo ch36 bw20 | `0,0,0,0,0,0,0,0` | `0,0,0,0,2,2,4,4` |
  | agcombo ch100 bw20 | `4,4,4,4,4,4,8,8` | `0,0,0,0,4,4,8,8` |

  A ch100 l'osservato e' `max(predetto, 4)`, sul d6220 ch100 e' `max(pred, 0)` e
  ch104-140 e' `max(pred, 2)`. Quindi il termine che mancava non e' additivo, e'
  un **pavimento** per canale -- coerente col fatto che un limite regolatorio
  puo' solo alzare il backoff, lo stesso argomento sui segni dei residui che il
  commento di `b43_phy_ac_txpwr_subband()` usa. Sull'agcombo il pavimento e'
  esatto: 0 a ch52, 4 a ch100, ch104 e ch132.

  **Ma c'e' un secondo fenomeno, e non e' un pavimento**: sull'agcombo a ch36 e
  sul d6220 a ch52-64 l'osservato sta *sotto* il predetto ed e' piatto a zero,
  cioe' la variazione per-rate sparisce del tutto. Che non sia il campo `po` lo
  inchioda l'agcombo: `5glpo` e `5gmpo` la' coincidono, `0x88644220`, quindi la
  predizione a ch36 e a ch52 e' identica -- e l'osservato e' piatto a ch36 e
  per-rate a ch52. E non e' `maxp5ga`: l'agcombo ha `{74, 74, 82, 82}`, lo
  stesso 74 nelle due sottobande che si comportano in modo diverso. Quindi la
  soppressione e' keyata sulla sottobanda, e serve un ingresso che non e'
  nessuno dei due campi.

  **Gli undici residui su `max`, risolti per due terzi.** Posto
  `k = (maxp - M) / 2`, in 41 configurazioni su 52 `k` e' esattamente
  `min(nibble)` del campo `po` della larghezza operativa. Gli altri undici si
  dividono in due famiglie di segno opposto:

  - **`M` sopra `maxp`**, cioe' `k` negativo: d6220 ch60 bw40 e ch36 bw80,
    agcombo ch36/44 bw40 e ch36 bw80. I salti sono `+4` e `+8` quarti di dB,
    cioe' 1 e 2 dB, e **solo sulle larghezze legate**. E' il bonus di densita'
    spettrale: allargando la banda il totale ammesso cresce, quindi il tetto di
    partenza sale. Il termine mancava perche' si assumeva che la larghezza
    spingesse solo verso il basso.
  - **`M` sotto `maxp`**: agcombo ch36-48 bw20 con `k = +4`, piu' agcombo
    ch100 bw40 e d6220 ch100 bw40. Qui il tetto morde prima che si prenda il
    massimo. Restano quattro configurazioni con questa forma, ed e' l'unico
    residuo vero.

  **Dove vivono le due riduzioni.** I gruppi a due catene sono popolati nella
  tabella 0x21 -- i 24 `u32` sono 96 slot da un byte, quattro per gruppo, e i
  quattro byte sono i modi STF SISO/CDD/STBC/SDM; leggere ogni `u32` come un
  numero solo li nasconde. Nei gruppi popolati `CDD` e' **uguale** a `SISO`, e
  `STBC`/`SDM` sono zero su entrambe le board.

  Non vuol dire che il backoff per catena non ci sia: **sta nel target per
  core**, non nelle distanze per rate. I target sono identici fra core -- 56/56
  sul d6220, 56/56/56 sull'agcombo -- che e' esattamente cio' che si aspetta se
  ogni catena prende `totale - 10*log10(N)`: la riduzione e' nel valore comune,
  non in una differenza fra core, e sarebbe osservabile solo confrontando una
  configurazione a una catena con una a piu' catene. Non ne abbiamo: il vendor
  configura tutte le catene in ogni cattura, due sul d6220 e tre sull'agcombo,
  e lo dicono i registri `0x0646`/`0x0846`/`0x0a46`.

  Sulla larghezza il target **sale**: ch36 sul d6220 fa 56 a 20 MHz e 66 a 40.
  Il limite a 5 GHz e' una densita' spettrale, quindi raddoppiando la banda il
  totale ammesso cresce di 3 dB -- la potenza per MHz scende, quella per catena
  sale. Le due riduzioni vanno in direzioni opposte, ed e' la ragione per cui i
  conti sulla larghezza non tornavano assumendo il contrario.

  **La tabella 0x21 non e' invariante**, e per vederlo servono entrambe le
  board e tutti e 24 i word del payload. Su tutte e 52 le
  configurazioni la posizione 10 vale `0x0101` su ogni segmento agcombo della
  sottobanda alta -- ch100-140, a ogni larghezza -- e zero su tutto il resto,
  d6220 compreso. Dipende dalla sottobanda e si vede dove i nibble SROM sono
  grandi (`5ghpo = 0xcca88440` sull'agcombo). Quindi le tre costanti hardcodate
  in `src/phy_ac.c` sono giuste per il d6220 e incomplete altrove, e la domanda
  aperta nel commento -- l'asse board -- e' risolta: l'asse esiste.

  **Non e' il ppr**, e nemmeno sull'asse board: sull'agcombo la tabella 0x21
  vale `0x202` in 1, 5 e 6 su ogni canale, identica al d6220. Il commento in
  `src/phy_ac.c` che dice «only the d6220 has a sweep» e' da aggiornare --
  l'agcombo ce l'ha, ed e' quello che chiude la verifica.

  **Il DSL, che per l'OFDM non fa testo,** Sulla sua unica cattura
  completa a ch36 tutti e otto i blocchi OFDM ricevono `0x30`, mentre la sua
  SROM (`mcsbw205glpo = 0xeca86420`, nibble `0, 2, 4, 6, 8, 10, 12, 14`)
  predirebbe cinque valori distinti. Un valore piatto contro cinque. Le
  possibilita' sono due: la regola dipende dalla versione del blob, 7.14 contro
  6.30, o e' sbagliata e la corrispondenza sul d6220 va rivista. Da tenere
  presente che delle configurazioni a freddo di allora solo quelle da ch100 in su
  hanno nibble abbastanza vari da discriminare una mappatura: sulle altre
  qualunque mappa da' zero.

  **Il limite UNII-1 e' escluso in entrambe le forme**, e va escluso perche' e'
  l'ipotesi che viene naturale.

  Come tetto assoluto: se mordesse un tetto regolatorio uguale per le due
  schede -- `ccode` vuoto e regrev 0 su entrambe -- `M` sarebbe assoluto mentre
  `ppr_cck` segue `maxp`, che differisce di 2 quarti, e i campi differirebbero
  di 1. Sono identici, `-6` su entrambe a UNII-1 con `maxp` 72 e 74. Un tetto
  assoluto non da' campi uguali con basi diverse: la riduzione e' **relativa** a
  `maxp`.

  Come limite di densita' (`maxp` totale, limite per MHz): darebbe 3 dB per
  raddoppio, quindi due gradini uguali su 20/40/80. A ch36 sono `-6 -> -1 -> -1`:
  primo gradino 2,5 dB, secondo zero. Non scala come una densita' -- e questo
  cassa anche la lettura data al bonus di banda piu' sopra, che e' la stessa
  idea in positivo.

  Resta dai dati, senza interpretazione: una riduzione relativa a `maxp`, uguale
  fra board a UNII-1 (-6) e UNII-2C (-1), **diversa a UNII-2A** (-3 sul d6220,
  -1 sull'agcombo) -- che e' l'unica sottobanda in cui le due SROM hanno una
  relazione interna diversa, `70 < 72` contro `74 = 74`.

  **Il campo CCK non e' una lettura diretta del massimo locale**, e il test lo
  falsifica: su d6220 ch36, dove `maxp` resta 72 e `M` cambia con la larghezza,
  `ppr_cck = M - 2*campo` fa 84 a 20 MHz, 74 a 40 e 82 a 80. Ne' `ppr_cck` ne'
  `M - maxp` sono fissi.

  Ma il conto da' un fatto piu' netto. **A 40 e 80 MHz il campo e' -1 quasi
  ovunque**, e le deviazioni stanno tutte a **20 MHz sui canali bassi**,
  crescendo scendendo in frequenza: -1 in UNII-2C, -3 in UNII-2A, -6 in UNII-1
  su entrambe le board -- una riduzione extra di 2,5 dB che alle larghezze
  legate scompare. Le anomalie degli OFDM sono nella stessa regione e con lo
  stesso segno. Due osservabili indipendenti che indicano lo stesso posto: a
  UNII-1 e 20 MHz entra una riduzione che il modello non ha, e non e' un
  artefatto di uno dei due conti.

  **I quattro CCK non sono board-independent**, e ch36 e ch100 non lo mostrano
  perche' sono i due punti in cui le due schede concordano. Estratti tutti:

  | | ch36-48 | ch52-64 | ch100+ |
  | --- | --- | --- | --- |
  | d6220 bw20 | 0xd0 | **0xe8** | 0xf8 |
  | agcombo bw20 | 0xd0 | **0xf8** | 0xf8 |

  Divergono su UNII-2A a tutte le larghezze -- anche 0xf0 contro 0xf8 a ch52
  bw40 e bw80. E divergono dove divergono le SROM: `maxp5ga` e' `{72,70,86}`
  sul d6220 e `{74,74,82}` sull'agcombo, e la sottobanda 1 e' la sola in cui il
  d6220 ha un valore piu' basso della 0. Quindi anche i CCK dipendono dalla
  SROM: non sono una costante, sono lo stesso conto senza il campo per-rate.
  Tabellarli per canale e larghezza inchioderebbe un valore del d6220 su sei
  configurazioni dell'agcombo.

  **Sul PLCP il DSL conferma**: il suo campo SIGNAL da' `len = 262` contro i
  284 del d6220, e le durate a +12 seguono. La formula regge su due board.

  **Fatto: +18**, che era il gruppo A. `shm_readback_block()` indirizzava col
  passo fisso `0x14`, che su questa board da' le celle giuste ma non emette le
  letture del puntatore; ora passa da `b43_phy_ac_rate_shm_offset()` come il
  vendor.

  **Fatto: la scansione delle due direct-map**, sedici voci ciascuna in sola
  lettura piu' `0x0056` che la chiude, cold01 #12245-#12309.

  **Cautela sulle conclusioni tratte da un'assenza.** `b43-6362-wip`,
  `docs/gap-inventory.md`, documenta che la object memory si scrive anche con
  `write_objmem` e in blocco con `copyto_objmem`, che **non** passa dalle
  varianti `*16`; e che sul blob del d6220 le `*16` esistono e sono quelle
  agganciate, mentre `copyto_objmem`/`copyfrom_objmem` pure esistono. Quindi
  una scrittura in blocco puo' non comparire nelle catture di questo repo. Il
  `sel=` che si vede solo nella cattura DSL e' l'altra faccia della stessa cosa:
  a 6.30 gli accessor sono `write_objmem` col selettore di spazio.

  Le affermazioni di questa sezione che poggiano su un'assenza vanno quindi
  lette come "non tracciato", non come "non fatto": che i blocchi CCK ricevano
  solo +14 e niente PLCP, e che +8/+10/+12 siano solo degli OFDM. Regge meglio
  lo shadow delle host flag, perche' la' l'assenza di letture e' corroborata da
  evidenza positiva -- il modello predice 38 chiamate su 38 e 56 su 56.

  **Il muro e' ora +14 per i quattro rate CCK**, e quelli non vengono da
  `mcsbw*po`: valgono -6, -3, -2, -1 e -4 in ottavi a freddo e solo -1, -2, -3 a
  caldo, sono identici fra le quattro celle -- quindi per i CCK il campo non e'
  per-rate -- e `cckbw202gpo` e' zero. `maxp5ga0 = {72, 70, 86, 0}` non li
  spiega: la mappa non e' monotona, 70 e' minore di 72 e da' un valore
  maggiore. Su 5 GHz quei rate non trasmettono, ma le op vanno emesse.

  **Fatto: +8, +10 e +12**, e non e' una tabella, e' un conto. Il campo SIGNAL
  sta a +8 e +10, la durata a +12:

      tmp    = len << 5
      plcp   = nibble_rate | (tmp & 0xff), tmp >> 8, tmp >> 16
      durata = 20 + ceil((len * 8 + 22) / NDBPS) * 4 + SIFS

  che e' `brcms_c_compute_ofdm_plcp()` piu' `brcms_c_calc_frame_time()`.
  Verificato al microsecondo su tutti e otto i rate di cold01: 420, 292, 228,
  164, 132, 100, 84 e 80 us con len = 284 e SIFS = 16.

  Quello che resta provvisorio e' `len`, non la formula: sul ferro deve venire
  dal template della probe response, e niente ancora la fornisce. Le catture
  danno 284 byte a 20 MHz, 285 a 40 e 286 a 80, ricavati dal campo SIGNAL. Il
  +1 per larghezza non e' spiegato: a 20 MHz la `TPL.RAMW` misura 280 e 280 + 4
  di FCS torna, ma a 40 MHz la `TPL.RAMW` misura 284 mentre il SIGNAL dice 285.

  **Perimetro.** `0x01c0-0x01de`, `0x0200-0x021e` e `0x0056` sono passate in
  `PHY_ANCHE`: `b43.h` le dichiara tabelle rate, del core, e lo erano finche' il
  port non le leggeva. Attenzione al modo in cui la voce va aggiunta: toglierne
  una parte sola fa scendere il muro posizionale, perche' il vendor legge quei
  puntatori diciotto volte per campi diversi e il perimetro nascondeva le
  letture dei campi non implementati. E' successo -- il muro e' tornato da
  @10854 a @10200 -- e si e' chiuso da se' implementando il blocco intero
  invece di ritoccare il perimetro.

  Il muro apre con quattro accessi piu' due
  gruppi di lettura-riscrittura.

  **Fatto**, in `b43_phy_ac_shm_readback_block()`: le quattro op di apertura,
  trascritte con i loro TODO (`RD 0x0092`, `WR 0x000c = 0xf`, `SLOTT` `0x3ff`
  poi `9`), e il gruppo A -- otto celle a `0x099e` con passo `0x14`, lette e
  **riscritte col valore appena letto**. Quel gruppo non trascrive niente: cio'
  che torna e' cio' che e' uscito, quindi non c'e' un valore che possa essere
  sbagliato su una board non misurata. Che i valori SIANO costanti --
  `44 3c 34 30 2c 2c 28 28` su due canali del d6220 e su un 4360 -- e' come si
  sa che la rilettura e' fedele e non una coincidenza: sono default dell'ucode.

  Perche' il driver stock lo faccia non e' noto. Una read-modify-write la cui
  modifica e' un no-op su questo hardware avrebbe esattamente questa forma, e
  cosi' l'avrebbe un tocco deliberato per far notare le celle all'ucode; le
  catture non le distinguono, e riprodurre gli accessi non costa niente in
  nessuno dei due casi.

  **Correzione alla `patches/0012`**: le catture mettono ENTRAMBE le scritture
  di `SLOTT` qui, non al core init dove quella patch mette il `9`. La patch va
  rivista.

  **Da fare**, il gruppo B: dodici celle di cui quattro passano da `0` a `0xd0`
  (`0x0a3a`, `0x0a56`, `0x0a72`, `0x0a8e`) e otto restano a zero ma vengono
  comunque lette e riscritte. Il valore dipende dal canale -- `0xd0` su ch36,
  `0xf8` su ch100, `0xd0` sull'agcombo a ch36 -- quindi va derivato, non
  trascritto.

- **Sopra i 5250 MHz le calibrazioni che trasmettono non girano — tre fasi
  fatte, il confine esatto no.** Il predicato e'
  `b43_phy_ac_may_calibrate_tx()`, e ci sono dietro le tre fasi di cui
  l'assenza e' **provata su tutti e 43 i segmenti a freddo**:

  | fase | testimone | 37 segmenti | 6 radar-meteo |
  | --- | --- | --- | --- |
  | `rxcal_afe_calibrate` + `finalize_gain_luts` | PHY `0x0380` | 209-912 | **0** |
  | `rxiqcal_run_meas_iters` | PHY `0x0380` | idem | **0** |
  | `loopback_gain_search` | PHY `0x0b22` | 9 | **0** |
  | `rxiqcal_coeff_tables_reset` | tab. `0x42`/`0x62`/`0x82` | 256 ciascuna | **0** |
  | `post_rxiqcal_stage2` | tab. `0x000e` | 8-12 | **0** |

  **La colonna vuota non e' piu' "da ch52 in su".** Sul set precedente le tre
  fasi mancavano su tutti i segmenti sopra i 5250 e la soglia sembrava il
  termine; rifatta la misura sui 43 segmenti a freddo, mancano **solo** sui sei
  radar-meteo, e ci sono su tutti gli altri 37, ch52-165 compresi. E' il
  predicato con la guardia radar e il CAC, non la frequenza. Il testimone di una
  fase e' un registro o una tabella che nel port solo quella funzione tocca --
  il metodo trova le fasi che ne hanno uno, e non quelle che condividono tutto
  con altre.

  Due che NON vanno gatate, controllate e scartate: `idle_tssi_meas` (198
  campionamenti a ch36 contro 192 a ch52: gira su entrambe) e
  `rxiqcal_finalize` (19 e 19).

  Una da guardare: `rxiqcal_meas_post_dds_apply_v2`, il cui testimone PHY
  `0x0270` fa 110-202 accessi sotto i 5250 MHz ed esattamente **1** sopra, su
  ogni segmento. Quasi assente ma non del tutto, e quell'uno va spiegato prima
  di gatare la fase intera.

  Non e' una corsa piu' breve, e' niente, ed e' la maggior parte della
  differenza fra un attach da 36k op e uno da 20k. Il port le eseguiva sempre:
  27420 op a ogni canale contro le 20191 del vendor a ch52.

  La ragione fisica c'e' e non e' solo una linea che combacia: quelle
  calibrazioni **trasmettono** -- pilotano il generatore di tono e attendono il
  risultato -- e una radio che non puo' trasmettere finche' il channel
  availability check non e' finito non puo' eseguirle. Non e' una prova: lo
  stesso confine e' anche "la seconda sottobanda dei 5 GHz".

  **Le cinque fasi sono gatate e i testimoni lo confermano a ch52**, port e
  vendor entrambi a zero su `0x0380`, `0x0b22` e le tabelle `0x42`/`0x62`/
  `0x82`. Non e' li' che sta il residuo.

  **Dove il canale non e' disponibile il driver stock non fa un attach ridotto:
  ne fa uno diverso.** Il conteggio per indirizzo, che non dipende
  dall'allineamento e quindi resta valido oltre il muro posizionale, partiziona
  senza una sola eccezione -- ma la partizione non e' quella scritta qui sotto.
  Rifatto sui 43 segmenti a freddo:

  | | 37 segmenti, ch36-165 | 6 radar-meteo |
  | --- | --- | --- |
  | op totali del vendor | 29692 - 49062 | 16923 - 16960 |
  | `PHY.RD 0x0251` | 0 su ch36-48 e ch144-165, 160-722 sui DFS | 160-161 |
  | `TBL.WR id=0x000c` | 397 - 445 | **8** |
  | `MAC.MCTRL` | 153 - 2071 | 465 - 467 |

  Cioe' la firma che il set precedente attribuiva ai canali sopra i 5250 --
  ventimila op, otto scritture su `0x000c`, nessuna calibrazione -- oggi ce
  l'hanno i soli sei segmenti ripresi senza il CAC completato, e tutti gli
  altri, DFS compresi, fanno l'attach pieno. La tabella originale, sul set di
  allora:

  | | 7 segmenti ch36-48 | 19 segmenti da ch52 |
  | --- | --- | --- |
  | op totali del vendor | 35350 – 50755 | 20175 – 20393 |
  | `PHY.RD 0x0251` | 0 | 134 (135 su ch104/108/60bw40, 108 su ch136) |
  | `PHY.RD 0x0252` | 1 | 135 (idem +1) |
  | `TBL.WR id=0x000c` | 429 – 439 | **8** |
  | `MAC.MCTRL` | 135 – 167 | 353 – 403 |

  **Le nove fasi dietro `may_calibrate_tx()`.** Le quattro aggiunte dopo le
  cinque originali sono `radio_iqcal_config` e `radio_iqcal_teardown`, la
  catena di semina del tono (`rxiqcal_dds_seed` e le due varianti tone) e
  `rxiqcal_meas_post_dds_apply_v2`, cioe' tutto quello che pilota il generatore
  di tono. Le prove per ognuna sono nel commento al predicato in
  `src/phy_ac.c` e nel messaggio del commit; il caso piu' netto sono i dodici
  registri radio di `radio_iqcal_config`, tutti e dodici a zero sopra la
  soglia contro 3-21 accessi ciascuno sotto, senza un indirizzo condiviso.

  **Un testimone esclusivo a zero prova assente il registro, non la fase.** La
  §6 di `test/unit/README.md` dice come si trova un testimone e non dice questo, che
  e' il passo dopo. Il controesempio e' `idle_tssi_meas`: RAD `0x004e`, PHY
  `0x0012` e `0x0845` sono solo suoi e sono tutti a zero sopra i 5250 MHz,
  mentre la fase la' gira. Percio' il criterio applicato e' **ogni** indirizzo
  della fase, con `phase_absent.py`, e per le fasi che lavorano attraverso una
  porta dati l'identita' e' la tabella e non l'indirizzo -- ogni scrittura di
  tabella si presenta come una scrittura su PHY `0x000f`. Scartate proprio
  cosi', dopo che il solo testimone le dava candidate:
  `post_cal_finalize_iter3` (2 indirizzi a zero su 10, e la sua finestra
  statistiche `0x0308-0x0312` fa 38 accessi sopra contro 40 sotto),
  `rxiqcal_apply_second_stage` (1 su 12) e `rxiqcal_apply_tx_bbmult_kick`, che ha
  i suoi tre registri PHY a zero ma le sue quattro scritture sulla tabella
  `0x000c` presenti una volta -- sono quattro delle otto superstiti.


  Tre fatti distinti, e vanno tenuti distinti:

  1. **`0x0251`/`0x0252` sono un blocco che il vendor esegue solo sopra i 5250
     MHz e che il port non ha affatto** -- 134 e 135 letture, zero nel port a
     qualunque canale. Non e' una fase da gatare: e' ~269 op di debito, e sta
     dal lato delle mancanti, non delle di troppo.
  2. **La tabella `0x000c` scende da ~431 scritture a esattamente 8.** Il port
     ne emette 188 piu' 42 letture. Le tre cifre otto su diciannove segmenti
     dicono che quel programming a ch52 e' un residuo minimo e non una versione
     ridotta, quindi va trovato quale sito emette quelle otto e gatato tutto il
     resto.
  3. **Il vendor emette 2,7 volte le `MAC.MCTRL`** -- ~399 contro ~147 -- e il
     port ne emette 115 a ogni canale. Un bracket suspend/enable per fase
     spiegherebbe la scala, ma quale fase le porti non e' stabilito.

  Al netto, a ch52 il port emette 19144 op contro le 15848 del vendor nella
  finestra; a ch36 ne emette 606 in meno e la sola classe in eccesso e'
  `AMT.WR`, che e' in `SOLO_PORT`. L'eccesso e' quindi tutto della famiglia
  alta.

  **Trappola dei marcatori, da non ripetere.** `AC_FN_MARKERS=1` emette una
  coppia -- `----FN:x----` in entrata e `----/FN:x----` in uscita -- e le
  coppie annidano. Leggere solo l'entrata attribuisce al chiamato tutto cio'
  che il chiamante emette dopo il ritorno, e il sintomo e' plausibile: una
  versione precedente di questa sezione dava `cca_pulse` come emittente di
  1091 op di troppo, mentre quella funzione tocca un registro solo, `BBCFG`,
  con due maskset. Lo stesso errore aveva assegnato la tabella `0x000e` a
  `post_rxiqcal_stage2` come testimone esclusivo: con la pila quella funzione
  ne emette 1 accesso su 8, gli altri 7 sono le tre `dds_seed`.
  `reverse-tools/phase_absent.py` tiene la pila, in entrambi i modi.

  Con l'attribuzione giusta, l'eccesso a ch52 dopo il gate delle fasi del
  tono e' 3450 op e ha una voce dominante:

  | funzione | op | cosa e' |
  | --- | --- | --- |
  | `idle_tssi_meas` | 1557 | forma ridotta, vedi sotto |
  | `rxiqcal_prep_second_iter` | 178 | |
  | `rxgain_perchan_config` | 164 | |
  | `rxiqcal_teardown_apply_defaults` | 163 | |
  | `rxiqcal_apply_body_core` | 158 | |
  | il resto | ~1230 | ventina di fasi sotto le 160 op |

  Nessuna di queste e' candidata al gate: sono forme ridotte. `idle_tssi_meas`
  e' quella misurata: 20 dei suoi indirizzi vanno a zero sopra la soglia -- RAD
  `0x004e`, `0x0166`, `0x024e`, `0x0366`, PHY `0x0460`, `0x0747`, `0x0732`,
  `0x0733` e i mirror -- mentre `0x0393` fa 18 accessi contro 42 e `0x0394` 12
  contro 30, quindi la fase gira ridotta. Il vendor la apre in **tre gruppi di
  sei** a ch52 e sette gruppi di sei a ch36; il port emette otto per chiamata a
  ogni canale. Quale sia il ramo che non gira non e' stabilito, e inventarlo
  significa scrivere una configurazione RF non osservata.

  Cautela sulle attribuzioni: passano dall'LCS, e a ch52 il muro e' a `@9609`,
  quindi da la' in avanti un blocco emesso nel posto sbagliato conta come di
  troppo di qua e mancante di la'. I conteggi per indirizzo della tabella sopra
  non hanno quel problema; l'attribuzione per funzione dice dove guardare, non
  quanto pesa.

  Nota che togliere queste op non muove il punteggio dei sette segmenti bassi:
  stanno oltre il muro a `~@9600` e l'LCS non penalizza le inserzioni. Muove il
  punteggio dei diciannove, dove la voce e' dominante.

- **La forma corta salta la sequenza classctl + clip-hold.** Dove la forma
  lunga ha `PHY.WR 0x0339 = 0x0fff` e poi il blocco di config MAC, la corta va
  diritta al blocco dopo `PHY.WR 0x0170 = 0x7d0`: il port invece emette in
  mezzo il peek di `0x0140`, la classctl, i quattro `adc_hold`
  (`0x02ed`-`0x02f9`), i tre `clip_det` (`0x06d4`/`0x08d4`/`0x0ad4`) e
  `PHY.WR 0x0339 = 0`. E' il bloccante dei 19 segmenti corti a `~@9600`, e non
  e' un valore sbagliato: e' una sequenza in piu'.

  E la posizione del blocco di config MAC differisce fra le due forme: sulla
  lunga il vendor lo mette dopo `PHY.WR 0x0339 = 0x0fff`, che e' dove
  `b43_phy_ac_shm_readback_block()` lo emette; sulla corta subito dopo
  `0x0170`. Quindi non basta togliere la sequenza in piu', va anche spostato il
  punto di emissione del blocco -- o capito da cosa dipende.

- **La misura di idle TSSI armava una volta invece che a ogni passata.**
  `b43_phy_ac_idle_tssi_meas()` leggeva `0x0393`, scriveva `0x0394` e `0x0393`
  UNA volta e poi leggeva le coppie `0x0013`/`0x0012` in un loop. Le catture
  riarmano prima di **ogni** coppia:

      RD 0x0393 -> WR 0x0394 = 0x0110|core -> WR 0x0393 = 0x8000 -> RD 0x0013 -> RD 0x0012

  A 20 e 40 MHz la passata e' una sola e le due forme sono indistinguibili; a
  80 MHz, dove il conteggio e' 256, il port emetteva 256 coppie di letture
  contro 256 passate armate. Spostata l'arma dentro il loop.

  `cold24-ch36-bw80` passa da 67.90% a **81.91%** e il posizionale da `@9863` a
  `@12759`, che e' il salto singolo piu' grande di questo giro. Ed e' anche un
  difetto funzionale, non solo di traccia: senza il riarmo le 256 letture
  campionavano la stessa misura invece di 256 misure, quindi la media
  dell'idle TSSI a 80 MHz era un solo campione ripetuto.

- **Registri che dipendono dalla banda e stavano fissi al valore di 20 MHz.**
  Nove gruppi, trovati confrontando i segmenti bw40 e bw80 contro il port e
  filtrando alle sole SCRITTURE con valore diverso. Quattro fatti, cinque no.

  Fatti, e sono quelli con una legge aritmetica invece di tre numeri:

  | registro | 20 MHz | 40 | 80 | legge |
  | --- | --- | --- | --- | --- |
  | `R2069_AFECAL_CFG` (radio `0x122`) | `0x5830` | `0x5030` | `0x4230` | comune `0x4030` + campo |
  | PHY `0x0381` | `0x7976` | `0x7987` | `0x7998` | `+0x11` per passo |
  | PHY `0x0463` | `0x27` | `0x4f` | `0x9f` | `(x+1)` raddoppia |
  | radio `0x004e`/`0x024e` | `0x8000` | `0x8009` | `0x8012` | `+9` per passo |
  | PHY `0x0738`/`0x0938` campo `[2:0]` | `3` | `4` | `5` | `+1` per passo |

  `b43_phy_ac_bw_step()` da' il passo, 0/1/2. Il commento dell'helper dice la
  cosa che conta: la legge e' fittata su tre punti e niente di piu', ed e' un
  modo compatto di scrivere cio' che e' stato misurato, non una previsione --
  160 MHz sarebbe un quarto punto e non c'e' cattura. `AFECAL_CFG` nel
  sorgente era commentato "fixed CTRL config", e non lo era.

  Fatto anche PHY `0x0140`, che si e' rivelato una regola e non tre numeri: il
  bit `0x0800` e' impostato a 20 MHz e azzerato sopra, e il resto della parola
  non si muove -- `0x0df4`/`0x0df6` a 20, `0x05f4`/`0x05f6` a 40 e 80. Vale su
  tutte le scritture tranne una per segmento; l'eccezione e' la prima di
  `channel_switch_prep()`, che riporta il bit 11 dal suo peek prima che
  `coeff_bank_init()` lo abbia impostato per la larghezza corrente e scrive
  `0x0df4` anche a 40 e 80. Rimisurato sui 139 segmenti dei quattro sweep: 106
  non hanno nessuna eccezione e 33 ne hanno esattamente una, e il registro non
  assume altri valori che quei quattro.

  Restano da fare, e sono tre valori per tre larghezze senza legge:

  | registro | 20 MHz | 40 | 80 | |
  | --- | --- | --- | --- | --- |
  | PHY `0x0646`/`0x0846` | `0x38` x3 | `0x3e` x1 + `0x42` x2 | idem 40 | tre siti con valori diversi sopra i 20, tutti uguali a 20 |
  | PHY `0x0737`/`0x0937` | `0x91` | `0x8d` | `0x8d` | uguale a 40 e 80 |
  | PHY `0x0321`-`0x0336` | `0x34`, `0x3a` | `0x36`, `0x3c` | `0x32`, `0x3d` | due scritture su `[7:0]`, non monotone; `[15:8]` sempre `0x36` |
  | PHY `0x073a`/`0x093a` | campo `[2:0]`=`3`, `[6:5]`=`2` | `[2:0]`=`2`, `[6:5]`=`0` | il bit `0x8` al posto di `[2:0]` | piu' campi cambiano, e a 80 MHz cambia quale |

  Nota su cosa NON e' un problema: nel confronto su bw40/80 la maggior parte
  delle differenze sono `OBJ.RD` e `PHY.RD`, cioe' valori **letti** --
  contatori a `0x0768`-`0x0788`, campioni di guadagno a `0x07ab`, finestra
  statistiche a `0x0308`. Divergono perche' sono a valle delle scritture
  sbagliate, non perche' il port legga male. Vanno riguardati dopo.

- **Le due forme di attach: la seconda `0x2e4`.** Sul set precedente i 26
  segmenti si dividevano in due forme, 7 lunghe (~36k op) e 19 corte (~20k), e
  il bloccante del posizionale su tutte e 19 era la stessa op,
  `PHY.MOD 0x02e4 val=0x0f00 mask=0x3f00`: nella forma corta il vendor la
  scrive **tre** volte e nella lunga una, e il port ne emetteva una.

  Aggiunta in `b43_phy_ac_cold_mac_preamble()`, fra la terza
  `b43_maccontrol_set(0x04000400)` e `b43_phy_ac_pmu_req(dev, false)`. Il
  posizionale dei 19 passava da `@54` a **~@9600**.

  Perche' la condizione e' sul canale: lo sweep e' **a freddo**, `rmmod` piu'
  `insmod` fra un ciclo e l'altro, quindi non esiste stato riportato dal ciclo
  precedente e nessuna regola del tipo "il primo canale di questa larghezza"
  puo' essere quella vera.

  **Quale proprieta' del canale, il set corrente lo decide.** Il conteggio per
  segmento sui 43 a freddo da' 1 su ch36-48 **e su ch144-165**, 3 da ch52 a
  ch140, su entrambe le larghezze e senza eccezioni. Il confine non e' i 5250
  MHz -- ch144-165 stanno sopra e scrivono una volta -- ma il dovere radar,
  la stessa famiglia del poll e delle calibrazioni. Il predicato nel driver e'
  gia' `b43_phy_ac_chan_has_radar_duty()`. Cosa il campo significhi resta
  ignoto, e sui canali con il dovere il vendor lo scrive tre volte contro le
  due del port: la terza sta nella coda del channel setup e nessun sito la
  emette.

  Questo capoverso diceva che "sopra 5250" e "richiede radar detection" erano
  lo stesso insieme e non si distinguevano, perche' lo sweep si fermava a
  ch140. Con ch144-165 in cattura si distinguono, ed e' il secondo: vedi sopra.

- **`MAC.BW` a 40 e 80 MHz — non c'era niente da aggiungere.** Il primo
  tentativo aggiungeva una chiamata a `b43_mac_bw_set()` in
  `b43_phy_ac_write_chanspec()`, col campo di banda del chanspec. Portava il
  posizionale dei tre segmenti a banda larga da `@88` a `@9472`, ed era
  sbagliato: quella funzione non esiste in b43 e il guadagno era comprato con
  del codice che non doveva esserci.

  `b43_mac_bw_set` non scrive un registro. L'equivalente GPL in brcmsmac,
  `brcms_b_bw_set()`, fa `wlc_phy_bw_state_set()` -- che e' `pi->bw = bw` e
  nient'altro -- piu' un reset e un init del PHY; e nelle catture fra il record
  e il prologo radio non c'e' nessuna scrittura di registro, con il prologo
  radio che e' quell'init.

  In b43 la larghezza sta in `phy.chandef`, che `b43_phy_init()` punta prima di
  chiamare `switch_analog()` e `b43_software_rfkill()` -- cioe' prima di dove il
  PHY scrive il chanspec -- e che `b43_op_config()` ripunta a ogni cambio di
  canale. `b43_is_40mhz()` legge da la'. E' gia' impostata, e non c'e' nulla da
  scrivere.

  Vale per `write_chanspec`, che gira da `software_rfkill`, e **non** in
  generale: `switch_analog` ha altri tre siti di chiamata --- `main.c:5650`,
  `4956`, `3402` --- che precedono `b43_phy_init()`, e la' `phy.chandef` e'
  nullo. E' il difetto che la suite di integrazione ha trovato in
  `cold_mac_preamble`; vedi `test/integration/README.md`.

  Cosa e' rimasto: `b43_phy_ac_write_chanspec()` ora legge `dev->phy.chandef`
  invece di frugare in `dev->wl->hw->conf.chandef`, che e' l'idioma di b43 e la
  fonte che il resto del driver usa. E `MAC.BW` e' in `SOLO_VENDOR` di
  `compare.py`, la lista delle op che nessun codice b43 puo' emettere: una per
  segmento a banda larga, e sblocca il posizionale a `@9471`.

- **MAC filter e address match sull'AC — chiuso, `patches/0011`.** Dalla
  cattura a freddo del DSL-3580L (kernel 2.6.30, wl 6.30.102.7):

  `wlc_bmac_set_addrmatch` -- la funzione che scrive la porta MACFILTER
  `0x0420`/`0x0422` -- era fra gli hook armati e **non ha sparato una volta** in
  un bring-up completo. Quella porta non viene scritta.

  Al suo posto la address match table, e nella traccia si vede tutta:
  `wlc_bmac_write_amt` su tutte le 64 voci durante il core init, due passate,
  poi `idx=0x3e a3=0x8002` (BSSID) e `idx=0x3f a3=0x8008` (station address).
  `wlc_bmac_set_rcmta` chiama `write_amt` al suo interno, che e' perche' le due
  classi compaiono interlacciate voce per voce.

  Il formato della voce: routing `B43_SHM_RCMTA`, due word a `idx * 2`, byte
  basso dell'indirizzo per primo, e i flag nella meta' alta della seconda word
  -- `0x8000` = valida. E' lo stesso impacchettamento che `keymac_write()` usa
  da sempre in quello spazio; l'unica differenza e' che la seconda word va
  scritta a 32 bit per portarci i flag. Confermato per costruzione: le tre
  half-word che ne escono per un MAC noto sono le stesse che `wl` scrive nella
  copia in shared memory della voce qui sotto.

  Nell'harness il doppione e' `emit_core_amt()` in `test/unit/main.c`, che emette
  **un record per riga** -- la granularita' del tracer, il cui hook da'
  `AMT.WR idx=` e non le due word sottostanti, che il vendor scrive sulla
  coppia objaddr/objdata per una via che nessun accessor agganciato copre.
  Non passa da `b43_shm_write16` dell'harness: quella ignora il routing e
  indicizza il mirror su `offset/2`, quindi una riga AMT calpesterebbe le celle
  basse della shared memory.

  Le 66 op che ne escono non sono in nessuna cattura del d6220, perche' l'hook
  e' arrivato dopo lo sweep: `compare.py` le scarta da **entrambi** i lati via
  `INVISIBILI`, e lo riporta. Appena una cattura le contiene quella lista si
  svuota e il confronto diventa piu' severo.

  Il gate e' `core_rev >= 42`, ed e' confrontabile direttamente con le
  catture: il valore che il driver mette in `B43_SHM_SH_WLCOREREV` e' quello
  stesso campo, e vale `0x2a` su tutte e tre le board -- d6220, DSL-3580L e
  agcombo. La 41 non l'abbiamo mai vista e sotto e' non testato, quindi resta
  la porta MACFILTER: sbagliare il confine verso il basso lascerebbe il filtro
  RX non programmato, sbagliarlo verso l'alto lascia le cose come sono oggi.

- **L'indirizzo MAC in shared memory a `0x078c-0x0790`.** `wl` scrive tre word
  la', e sono i sei byte del MAC col byte basso di ogni coppia per primo -- lo
  stesso impacchettamento di `b43_macfilter_set()`. I valori combaciano col
  `macaddr` di NVRAM di due board: `00:00:00:00:00:03` sul d6220 da'
  `0x0000/0x0000/0x0300`, `00:c0:02:01:07:24` sull'agcombo da'
  `0xc000/0x0102/0x2407`. Il `sel` e' zero sulla cattura agcombo, che lo porta,
  quindi la destinazione e' shared memory.

  b43 quelle celle non le tocca. `patches/0010` aggiunge
  `b43_shm_macaddr_set()` accanto alla scrittura MACFILTER in
  `b43_upload_card_macaddress()`, gatata su `B43_PHYTYPE_AC`, con
  `B43_SHM_SH_AC_MACADDR` in `b43.h`; il doppione nell'harness e'
  `emit_core_shm_macaddr()` in `test/unit/main.c`, col MAC nel profilo di board. La
  scrittura e' additiva e non disturba la porta MACFILTER.

  Aperto: se all'ucode serva, e in che rapporto stia con la voce sopra. Nella
  traccia cade prima del punto in cui b43 chiama
  `b43_upload_card_macaddress()`.

- **Celle di shared memory che le catture scrivono e b43 no — triate.** Delle
  1835 op `OBJ` che il port non riproduce su `cold01`, l'inventario contro
  `b43.h` e i sorgenti del core:

  | | op |
  | --- | --- |
  | b43 le emette gia' (nome usato nel core): mancano solo perche' l'harness compila `src/` e non `main.c` | 280 |
  | celle che `b43.h` nomina ma il core non usa | 47 |
  | celle che `b43.h` non nomina affatto | 1508 |

  Delle 47 nominate, trenta sono `PRSSID`/`PRSSIDLEN`/`PRTLEN`, cioe' l'offload
  delle probe response, che b43 **disabilita di proposito** con
  `PRMAXTIME = 1`: restano non scritte anche da noi. Le altre sono in
  `patches/0012`: `SLOTT`, `MAXBFRAMES`, `ANTSWAP`, `BTSFOFF`, `DEFAULTIV`,
  `RFATT`, `HOSTF4`, `HOSTF5`, `EDCFQ`, `PSM`.

  Di quelle il solo `SLOTT = 9` e' derivato -- e' lo slot di 802.11a -- e il
  resto e' osservato: stesso valore su due chip, ogni canale e due versioni di
  wl. Si scrivono perche' l'hardware evidentemente se le aspetta: lasciare una
  cella al default dell'ucode che il driver stock sovrascrive sempre e'
  anch'esso un'ipotesi, e la peggiore.

  Dove una cella viene scritta piu' volte durante il bring-up -- `HOSTF4` e
  `HOSTF5` guadagnano bit strada facendo, `BTSFOFF` cambia una volta -- la
  patch mette il valore finale: per l'hardware conta lo stato, e il core init
  di b43 ha una forma diversa da quella del vendor, quindi i punti intermedi
  non hanno un corrispondente.

  Delle 1508 non nominate, 480 sono l'azzeramento di `0x10f4-0x14b2`:
  identico su tutti gli 8 segmenti d6220 provati, e **assente sull'agcombo**.
  Quindi non e' un "azzera questa regione all'init" universale, e scriverlo
  incondizionatamente divergerebbe sull'altra board. Serve capire da cosa
  dipende prima che diventi codice.

  **L'ordine conta piu' del valore.** `MAXBFRAMES`, `ANTSWAP` e `BTSFOFF`
  rispecchiate nell'harness al punto giusto -- prima del chanspec, come la
  traccia -- vengono appaiate: 92.5654% -> 92.5724%. Metterle dopo il chanspec
  non ne appaia nessuna, perche' una sola inversione fa scartare l'op. Il resto
  del gruppo cade fra `#12197` e `#14172`, in un blocco che l'harness non
  modella, e non e' rispecchiato: emetterlo altrove lo metterebbe nel posto
  sbagliato.

  Il blocco di chip init e le host flag sono ora modellati nell'harness --
  `emit_core_shm_chipinit()` e `emit_core_hostflags()` in `test/unit/main.c`, con
  `core_rev` e `mac_hw_cap` nel profilo di board -- e portano da 92.5654% a
  92.6002%. Appaiate: `MAXBFRAMES`, `ANTSWAP`, `WLCOREREV`, `MACHW_L`/`H`,
  `BTSFOFF`, `SFFBLIM`, `LFFBLIM`, `HOSTF1`/`2`/`3`.

  Non appaiate, e si sa perche':

  - `MAC.MCTRL 0x40020000/0x40060000` a `#651` e `BTL0 = 7` a `#655`. La stima
    di 280 op "che b43 fa gia'" era ottenuta cercando il nome nel sorgente, che
    non e' la stessa cosa che scriverle in quel punto: `BTL0` b43 la usa solo
    in `b43_write_beacon_template`, non al core init. La stima e' quindi alta.
  - `HOSTF4 = 0x40` e `HOSTF5 = 0x80`. `patches/0012` scrive il valore finale,
    `0x0060` e `0x8088`, e quelle due op non combaceranno mai: e' la
    conseguenza voluta di quella scelta, non un difetto da inseguire.

- **Le host flag: la cella si scrive a volte e a volte no, e manca l'argomento
  che lo spiega.** `wlc_bmac_mhf(hw, idx, mask, val, bands)` ha un quinto
  argomento che il tracer non catturava. Su `cold01` le chiamate che impostano
  bit sono seguite dalla scrittura della cella HOSTF solo in cinque casi su
  undici: `#623`, `#12242`, `#13525`, `#13530`, `#13535` si', mentre `#469`,
  `#470`, `#593`, `#594`, `#620` e `#13409` no -- e i loro valori ricompaiono
  al flush di `#689-#690`.

  I valori che si vedono sono accumuli coerenti: slot 4 va `0x80` -> `0x88` ->
  `0x8088`, slot 3 va `0x40` -> `0x60`. Quindi la funzione tiene un valore in
  cache e scrive la cella solo in certe condizioni; l'ipotesi e' che scriva
  quando `bands` combacia con la banda corrente. **Non e' stabilito**, e senza
  quell'argomento non si distingue.

  L'hook ora lo cattura via `nargx`, come per `si_corereg`. La prossima cattura
  lo dice, e da la' si sa dove b43 deve mettere le sue scritture di HOSTF4 e
  HOSTF5 invece di metterle tutte al core init.

- **Il confine fra core e PHY nel port e' in un punto sbagliato.** Il vendor
  emette op del core mentre sta facendo il PHY, e dove il port ha messo quelle
  op dentro una funzione del PHY il confronto si blocca: le op che mancano
  cadono in mezzo a quelle che la funzione emette in blocco, e l'harness puo'
  inserire solo ai confini fra chiamate del flow.

  Uno risolto: `b43_phy_ac_cold_mac_preamble()` chiudeva con un
  `b43_maccontrol_set(~0x40060000, 0x40020000)`, cioe' `INFRA | DISCPMQ` con
  `AP` azzerato -- il modo operativo del **core**, che `b43_adjust_opmode()`
  imposta. Il PHY non doveva scriverlo, e le catture concordano: il vendor lo
  emette dopo che il core ha scritto le sue celle di chip init (`cold01` `#645`
  per la GPIO.CTL, `#651` per questo, con due scritture in mezzo). Togliato da
  `src/` e spostato nel modello del core init.

  Ne resta almeno uno: `OBJ.WR 0x5e = 0x100` a `#624` cade dentro la sequenza
  MHF del preambolo. Era il bloccante del posizionale, e nei primi 2000 op era
  **l'unica** non appaiata: una sola op mancante sfasa di uno tutto quello che
  segue. Emessa dal wrapper MHF dell'harness con la condizione minima
  `slot == 0 && val == 0x100`, il posizionale passa da `@52` a `@10196`.

  Quella condizione e' un tappo e nel codice e' marcata come tale: e' cucita
  sul caso singolo, non e' un modello, e va sostituita dalla condizione vera
  appena una cattura porta `bands`. Se poi si scoprisse che la scrittura e' una
  `hf_write` separata del driver, non va nel wrapper affatto ma in `src/`.

  **L'ordine conta piu' del valore, e va rispettato quello della traccia.** Tre
  volte in questo giro un gruppo di op giuste non si e' appaiato per una sola
  inversione: `MAXBFRAMES` messa dopo il chanspec invece che prima, le host
  flag messe prima del MAC in shared memory invece che dopo, l'AMT dopo il
  blocco di chip init invece che prima. Su cold01 l'ordine e' AMT `#443`, chip
  init `#649-#658`, MAC `#661-#663`, host flag `#686-#690`, chanspec `#691`.

  Nota che b43 il suo core init lo fa in un ordine diverso -- mette le host
  flag fra `WLCOREREV` e `MACHW`, il vendor molto piu' tardi -- quindi
  l'harness segue il vendor e la riconciliazione dei due ordini resta aperta.

- **Config MAC/ucode del core b43 all'attach — 1250 op, il pezzo piu' grosso
  che resta.** L'obiettivo e' che b43 emetta una-per-una tutte le op di `wl`,
  quindi queste non sono fuori perimetro: sono debito, e vanno scritte nel core
  (`main.c`, `xmit.c`) come patch della serie, non nel PHY.

  Sul segmento `cold01` dello sweep a freddo, misurato senza perimetro:

  | episodio | op | contenuto |
  |---|---|---|
  | `#12243` | 583 | `OBJ.WR` x550: init della shared memory del MAC |
  | `#12860` | 129 | tabelle rate lette, key index block, `0x8ec-0x94a`, 12 RMW a `0x99a-0xa8e` |
  | `#13540` | 94 | continuazione della config MAC |
  | `#14085` | 73 | idem |
  | `#14170` | 68 | idem |
  | `#30735` | 123 | `OBJ.WR` x97 + 4 `TPL.RAMW`, prima della fase probe |

  **Buona parte di `b43_wireless_core_init` la fa gia'.** Su `cold01` le op
  `#649-#658` sono `MAXBFRAMES`, `ANTSWAP`, `WLCOREREV`, `MACHW_L`/`MACHW_H`,
  `BTL0`, `BTSFOFF`, `SFFBLIM=3`, `LFFBLIM=2`, e b43 le emette tutte in
  `b43_wireless_core_init` con gli stessi valori. Mancano dal confronto solo
  perche' l'harness compila `src/` e non `main.c`. Prima di scrivere codice
  nuovo per questo blocco va guardato op per op quanto e' gia' coperto: la
  stima di 1250 op di debito e' con ogni probabilita' gonfiata.

  Il blocco `#12860` e' configurazione, non raccolta di statistiche: con un
  tick del watchdog condivide solo le celle dei contatori, una passata invece
  di due, e per il resto scrive 16 word a `0x92c-0x94a` con valori tipo
  `0x3475`/`0x217c`. E' identico sull'agcombo, quindi non e' taratura di board.

  Perche' e' portabile. Il port carica il firmware 784.2, lo stesso di `wl` e
  preso da `wl`, quindi la mappa della shared memory sopra `0x0700` e' quella
  che quei valori assumono. Il firmware open non regge l'AC, e adattarlo e' un
  lavoro a se'.

  Attenzione a un tranello di nomenclatura: `OBJ.WR 0x0910` qui e' shared
  memory, **non** il registro PHY `0x0910` del banco CRS di questo stesso
  documento. Spazi di indirizzamento diversi, stesso numero.

- **OTP e SROMCTL all'attach — verificare contro `patches/0001`, poi SKIP.**
  Tre op su `cold01` (`SROMCTL.RD`, `OTP.RDW`, `OTP.INIT` a `#548-#551`), in
  mezzo alla sequenza analogica. Le emette il codice srom di bcma, che
  `patches/0001` tocca per aggiungere l'estrazione rev 11. Da verificare che
  siano esattamente quelle che la patch produce; se lo sono lo skip e'
  definitivo e la finestra del confronto va fatta partire dopo, altrimenti e'
  un buco della patch.

  Meno urgenti, stessa natura: `MAC.MHF.RD` a `#13537` (`b43_hf_read()`, che nel core c'e' gia'), le 19 `TPL.RAMW` e la `CAL.INIT`.

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
- **Max index TX su 0x0646/0x0846 -- chiuso come tetto regolatorio.** Lo
  `0x38` del primo bring-up e' il tetto del locale di default che lega su
  ch36-48 a 20 MHz, non una costante di fase; il DSL lo emette anche sul
  down->up perche' la' il country non viene mai impostato. Vedi la sezione sul
  registro `0x0646`.
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

- **Soglie CRS e banco 0x0910: il valore e' chiuso, il momento no.** Regola,
  verifica e i 12 segmenti a freddo dove il blocco E cade in un punto diverso
  da quello del vendor in [`crs-min-power.md`](crs-min-power.md).
- **Coefficienti RXIQ — chiuso.** `b43_phy_ac_iq_solve` e' collegato ai tre
  call site e la formula e' quella verificata sul blob (§8 di
  `rxiq-cal-analysis.md`). Bit-exact in attach; resta un residuo di 1 LSB sul
  warm della catena 0, dichiarato e non inseguibile dalle catture.
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
- **accessor vendor — parzialmente chiuso.** L'accessor che questo punto
  cercava sotto il nome `phy_reg_write_list` esiste e si chiama
  `phy_reg_write_array`: e' `GLOBAL` in entrambi i blob ed e' ora agganciato,
  insieme a `phy_reg_read_wide`/`phy_reg_write_wide` (accesso PHY a 32 bit),
  `wlc_bmac_write_ihr` (gli Indirect Hardware Registers del core d11) e
  `wlc_bmac_set_shm` (scrittura mascherata in shared memory). Nessuno dei
  cinque era coperto, quindi finora le catture erano cieche su quelle op.

  Le firme sono lette dai prologhi dell'oggetto 6.30 con
  `reverse-tools/mipsdis.py --prologo`, e quattro su sei non erano quello che il
  nome suggeriva -- `phy_reg_write_array` prende un puntatore e un conteggio,
  non un indirizzo; i due `_wide` non hanno indirizzo affatto. Tabella in
  `reverse-tools/wl-diag/README.md`.

  Restano da cercare, e non sono in questi due blob con questi nomi:
  `wlc_phy_write_regs*` e varianti `write_radio_reg_*`. Da verificare col
  `/proc/kallsyms` del DSL riflashato, che elenca anche cio' che il blob non
  chiama per nome.

  **Uno accertato mancante: `wlc_bmac_set_addrmatch`.** Programma i registri
  RXE di match dell'indirizzo con una write diretta sulla finestra del core
  d11 (`rcm_ctl`, `rcm_mat_data`, cioe' `0x0420`/`0x0422`), quindi non passa
  ne' da objmem ne' da `si_corereg` e nessun hook attuale la vede. Firma come
  `wlc_bmac_mctrl`: `(hw, int match_reg_offset, const u8 *addr)`, offset in
  a1. Serve a decidere il punto aperto sull'indirizzo MAC in shared memory
  qui sopra -- se `wl` scriva una strada, l'altra o entrambe.

  **Confine.** Si intercettano gli accessor di **I/O hardware**, non la logica
  interna del driver: la traccia deve restare un'osservazione del dispositivo.
  Strumentare funzioni interne farebbe cadere quella distinzione. Leggere
  `.rodata`/`.data` dai blob e' invece lettura di dati, ammissibile.


## Doppia programmazione dell'analogico durante l'attach

Osservata sull'agcombo e assente sul d6220 **nel preambolo**, e il discriminante
non e' stabilito.

Il conteggio per segmento va tenuto separato da quello del preambolo, perche'
l'unita' analogica compare anche altrove e per un po' non la si e' contata: il
testimone `PHY.WR 0x1725 = 0x1fff` sta **tre volte in tutti e 43 i segmenti a
freddo** -- due nel preambolo, la coppia di cui parla questa sezione, e una nel
bss-up in coda -- e **una sola volta in tutti e 44 i segmenti `up` a caldo**,
quella del bss-up. Rimisurato sul set corrente, senza eccezioni. Quindi "assente sul d6220" vale per la seconda entrata del preambolo,
non per l'attach: anche il d6220 rientra nell'arm analogico, venti secondi
dopo, ed e' l'occorrenza che stava sepolta in `rxiqcal_finalize()` e che ora e'
in `b43_phy_ac_down()` con le altre due dietro `b43_phy_ac_afe_arm()`.

Il gate in `b43_phy_ac_op_switch_analog` e' su `chip_id` perche' e' l'unica
variabile che distingue i due testimoni, non perche' sia dimostrato.

Cosa si sa:

- il codice del driver stock e' **identico** fra le due versioni coinvolte. I
  siti di chiamata a `wlc_phy_switch_radio` (via puntatore, dalla tabella ops)
  sono 7 in 7.14.89.14 e 7.14.43.21, funzione per funzione: `phy_init` 1,
  `phy_attach` 1, `coredisable` 1, `bmac_set_chanspec` 1, `bmac_radio_hw` 2,
  `bmac_init` 1. Quindi la differenza e' a **runtime**, non nel codice;
- contare i siti da' un limite superiore, non le esecuzioni: un sito in un ciclo
  esegue N volte, due siti in rami esclusivi ne eseguono uno;
- fra le due entrate il driver stock riscrive il PLL con gli **stessi** valori e
  le dieci letture di save ricominciano da capo: e' una seconda invocazione
  completa, non un ciclo interno.

Evidenza dalle catture:

| board | unita AFE | altro |
|---|---|---|
| agcombo | `@17`, `@37`, `@71`, `@91` -> due coppie | `PMU.PLL 0xc31/0x100e` riscritti a `#123`/`#131`, **fra** le due coppie; `PHY.WR 0x01ec = 0x2` dopo la MOD `0x02e4` di ciascuna coppia |
| d6220 | `@17`, `@36` -> una coppia | nessuna seconda scrittura del PLL, nessuna rientrata |

Tutte e quattro le unita' scrivono il banco AFE_ON (`0x1725=0x1fff`,
`0x1721=0xffff`, `0x1720=0x03ff`): non c'e' nessuno spegnimento in mezzo, sono
due accensioni. Se il PLL e' la causa lo e' l'atto (re-lock), non un cambio di
frequenza.

Le op di bus fra le due entrate (`SI.COREREG`, `PMU.PLL`, `MAC.MCTRL`) non sono
riprodotte: appartengono a bcma e al core b43, non al PHY.

- **TODO(4360 con 7.14.89):** serve un 4360 con la versione del d6220 per
  separare chip da versione. Il VR400 ha 7.14.89 e lo stesso SDK, ma monta un
  4352.
- **TODO(verificare su una board nuova):** l'evidenza e' un solo 4360. Serve un
  secondo esemplare per distinguere "proprieta' del chip" da "proprieta' di
  questa board", e per capire se la rientrata segua la riscrittura del PLL o se
  entrambe seguano un terzo fattore nella sequenza di attach.

## Prima mappatura su agcombo (4360)

Cattura `agcombo-wl1-4360-rescan-to-bss-ch36` (6453 RETVAL, la sola agcombo con
i valori letti), flow `full`, finestra da `#47` -- l'analogo del `#50` del
d6220, trovato agganciando le prime op del port. **77.40% di op in comune su
30322, prima divergenza a @45.**

Il numero **non e' confrontabile** col 99.94% del d6220: quello misura una
configurazione validata op-per-op, questo la prima esposizione a una board
nuova, dove di 1615 regioni solo la prima e' interpretabile perche' il resto e'
a valle di un disallineamento.

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

Questo conferma in modo indipendente la nota della versione precedente del
decorrelatore, limitata a 4 trace d6220, che diceva "la variabile
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

`b43_phy_ac_op_switch_channel` ora rifiuta tutto cio' che non e' **ch36 BW20 su chip
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

### `femctrl`

`femctrl` seleziona lo schema di controllo del FEM -- linee di enable del PA e
stato dello switch T/R -- e vale 6 su tutte e tre le board, verificato in NVRAM.
`b43_phy_ac_set_regtbl_on_femctrl` lo legge e si ferma se non e' 6: la tabella
portata e' quella di femctrl 6, e pilotare un FEM diverso con quello schema
significa PA abilitato quando non dovrebbe, o switch T/R nello stato sbagliato
durante il TX. Meglio un bring-up che fallisce.

**Offset fissato e patch aggiornata.** `bcmsrom_tbl.h` (GPL, in
asuswrt-merlin e bcmdhd -- gli URL erano gia' in `sprom-rev11/cross_check.md`)
da' `femctrl` a `SROM11_FEM_CFG1` con maschera `0xf800`, bit 15:11. Con i valori
NVRAM la word attesa e' `0x30A1`, e nei dump raw sta al byte **0x0AA**, con
`FEM_CFG2` = `0x00A1` a `0x0AC`: dodici campi su dodici, su due board.
`patches/0001` ora definisce i due offset, i dodici campi in `struct ssb_sprom`
e la loro estrazione in `bcma_sprom_extract_r11` (`git apply --check` passa su
torvalds/linux master). L'harness non porta piu' `femctrl` come valore cotto: il
profilo board porta le due word raw e le decodifica con le stesse maschere.

Perche' il value-matching qui non basta, ed e' utile saperlo per i campi che
restano: il valore e' identico su entrambe le board di riferimento, quindi da'
un vincolo solo e 17 word candidate. Serve la fonte canonica, non un terzo dump.

**Un valore di potenza trascritto non si
scrive mai senza confrontarlo con cio' che l'SROM dichiara.** L'indice massimo
di potenza TX non e' piu' trascritto: viene da `txpwr_target()`, che parte da
`maxp5ga` e applica tetto regolatorio e margine, anche al primo bring-up.

### Impalcatura: i siti che scrivono valori trascritti e toccano RF

Non c'e' un marcatore greppabile: i tag `SCAFFOLD(...)` e `TODO(formula)` di
cui parlavano le versioni precedenti di questo file non sono piu' nel sorgente.
I siti si trovano con `grep -n -i scaffold src/*.c`, che oggi ne da' due, e il
commento di ognuno dice cosa e' trascritto e da dove:

- la tabella di controllo FEM, che e' quella di `femctrl=6`, il solo valore
  osservato (`b43_phy_ac_set_regtbl_on_femctrl`);
- il filtro delle configurazioni validate in `op_switch_channel()`, che e'
  impalcatura per costruzione, non un limite del chip.

Ne sono usciti, per ragioni diverse: il blocco `0x60`/`0x64`, che ora scrive
`afe_res[]`/`afe_res_cal[]`, risultati riletti dalla cal AFE; l'ampiezza del
tono, misurata invariante su 16 canali e tre larghezze; l'indice massimo di
potenza TX, derivato da SROM, tetto regolatorio e margine (sezione sul registro
`0x0646`). Un valore trascritto esce derivandolo **oppure** dimostrando che
l'invarianza copre il dominio.

La regola operativa: un `SCAFFOLD` si rimuove solo derivando il valore, oppure
allargando il filtro dei canali con una cattura a supporto. Non si rimuove
perche' il gate passa.

Vale anche il verso opposto, che il gate non protegge: **un valore derivato non
si sostituisce con la costante a cui si riduce sulla board di riferimento.** Il
confronto op-per-op non lo vede, perche' le due forme emettono la stessa op qui
-- come `tssifloor5g[grp] & 0x3ff`, che su queste tre board vale esattamente il
`0x03ff` che era cablato prima. Un refactor va quindi verificato con un diff del
sorgente a commenti spogliati, non solo con la trace.

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
  scritta insieme e azzerata insieme: arm/disarm del generatore di tono.

  **E non sono nemmeno per canale.** Questa riga diceva "serve una mappa
  canale -> valore, e i punti noti sono due: `0x0152` e' ch44, ch36 vale
  `0x0154`". Falso, e l'errore era gia' smentito dai dati in repo. Rimisurato
  sugli 87 segmenti del set corrente: i valori scritti su `0x0736` sono sempre e
  solo `0x0154`, `0x0152` e `0x022a`, piu' lo zero che disarma, e la tripletta
  compare nello stesso ordine ovunque:

      0x0154, 0, 0x0152, 0, 0x022a, 0, 0, 0x0154, 0

  Quel che cambia fra segmenti e' **quante volte** si ripete il solo `0x0154`,
  non quale valore prende un canale. Le uniche eccezioni sono i sei radar-meteo
  a freddo e i sette `up` corrispondenti, dove compare il solo `0x0154` perche'
  il blocco che scrive gli altri due non gira: e' la stessa condizione di
  disponibilita' del canale descritta in testa al documento, non una mappa.

  I tre valori distinguono i **siti di chiamata**, non i canali: `0x0154` in
  `rx_gain_regs_program`, `0x0152` nel banco `b2j_ops`, `0x022a` in
  `rxgain_perchan_config`. Il port aveva le tre costanti giuste per sito da
  subito; l'errore era solo nella descrizione. Chi ha scritto quella riga ha
  confrontato due siti diversi di due catture diverse e ha attribuito la
  differenza al canale.

  Morale, perche' ricorre: prima di dichiarare una dipendenza da canale o
  larghezza, si classifica la chiave con
  `reverse-tools/decorrelate_channels.py` sui segmenti del set corrente. Per
  `0x0736` la classe e' `dinamico`, non `solo-canale` -- e "dinamico" qui
  significa solo che il numero di scritture varia fra i segmenti, non il valore.
  Il `full-sweep.zip/decorrelazione-52-segmenti.csv` che questa riga citava
  **non e' in repo**: era un'uscita del tool tenuta a mano, e si rigenera invece
  di essere citata.

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
`reverse-tools/srom.py literals`
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
## Fuori perimetro: la regione probe-response della shared memory

Ogni segmento dello sweep d6220 contiene sette scritture in SHM che il port non
emette -- due su `0x0180`, `0x0182` e `0x0186`, una su `0x0184` -- con valori
costanti su tutti e 87 i segmenti del set corrente, a freddo e a caldo, e tutte
e tre le larghezze:

| offset | valore | decimale |
|---|---|---|
| `0x0180` | `0x0527` | 1319 |
| `0x0182` | `0x01f4` | 500 |
| `0x0184` | `0x0000` | 0 |
| `0x0186` | `0x0032` | 50 |

`0x0184` viene scritto a zero e **riletto** 29 episodi dopo, sempre zero, una
volta per segmento: e' un handshake col firmware, non configurazione depositata.

Cadono in una regione che `b43.h` mappa parzialmente, fra `B43_SHM_SH_PRSSID`
(`0x0160`, SSID della probe response, 32 byte) e `B43_SHM_SH_PRPHYCTL`
(`0x0188`, il suo control word PHY). Nessuna funzione del core b43 scrive
`0x0180`-`0x0187`.

**Non vanno portate**, e la ragione sta nel core, non in un'ipotesi. Le probe
request sono il meccanismo della scansione attiva; il lato AP deve risponderci
entro un tempo stretto, e ci sono due strategie: risponde il firmware, che per
farlo ha bisogno del template e delle temporizzazioni in shared memory, oppure
risponde il software. `wl` scegle la prima; b43 la seconda, e la disattiva
esplicitamente in `main.c`:

    /* Disable sending probe responses from firmware.
     * Setting the MaxTime to one usec will always trigger
     * a timeout, so we never send any probe resp. */
    b43_shm_write16(dev, B43_SHM_SHARED, B43_SHM_SH_PRMAXTIME, 1);

Quindi quelle quattro scritture configurano un offload che b43 tiene spento per
scelta architetturale: sarebbero senza effetto, e romperebbero i due gate.

Il gate `if (dev->phy.type == B43_PHYTYPE_AC)` nel core sarebbe disponibile —
quattro patch della serie lo usano gia' — quindi il problema non era mai il
rischio per gli altri PHY: e' che la funzionalita' non esiste in questo stack.

Nota sulle catture. Le due usate dai gate — `att.merged.txt` e
`dtb.merged.txt` — **non tracciano affatto la classe `OBJ`**: zero letture e
zero scritture, contro 1084 e 900 in un segmento dello sweep. L'hook non
esisteva quando sono state prese, quindi l'assenza di una `OBJ.WR` in quelle
due non dice nulla.

Le due catture di attach che hanno la classe completa sono
`wl-diag-wl1-attach-ch36-bw20-con-preambolo.txt` (3630 op `OBJ`) e
`wl-diag-wl1-attach-ch36-bw20-tabelle-complete.txt` (3328), e **entrambe
contengono queste sette scritture**. Quindi non sono legate all'operativita' da
AP: l'argomento resta il `PRMAXTIME=1` del core, non la loro assenza.

Classi assenti per intero dalle catture dei gate, da trattare come non
informative su quelle due: `OBJ.RD`, `OBJ.WR`, `TPL.RAMW`, `MAC.BW`,
`CAL.INIT`. Un'assenza vale come prova solo se la classe e' tracciata.

## `b43_amt_write()`: scrittura di riga, non di due word

`patches/0011` scrive una riga della address match table come due
`b43_shm_write32()` su `(index * 2) + 0` e `+ 1`, e rilegge la word alta solo
quando il chiamante passa `B43_AMT_KEEP_FLAGS`.

La cattura dice un'altra cosa. Su `cold01`, per ognuna delle 64 righe del ciclo
di azzeramento:

```
#307  AMT.WR    idx=0x0000
#308  OBJ.BULKR addr=0x0000 len=8 a5=0x00040000
#310  OBJ.BULKW addr=0x0000 len=8 a5=0x00040000
```

`a5=0x00040000` e' il routing RCMTA e `addr = idx * 8` e' la riga da 8 byte:
una lettura e una riscrittura della riga intera, su **tutte** le righe, comprese
le 64 azzerate dove non c'e' niente da preservare. Non due accessi da 4 byte.

Questi hook -- `OBJ.BULKR` e `OBJ.BULKW` -- sono fra quelli aggiunti con la
ricattura, e prima non c'erano: e' il motivo per cui il commento di
`emit_core_amt()` diceva che le due word non erano confrontabili.

Da fare: portare `b43_amt_write()` alla forma della cattura, il che assorbe
anche il ramo `KEEP_FLAGS`, visto che la riga viene letta comunque. Non e'
stato fatto qui perche' l'harness non compila il core e la modifica non sarebbe
verificabile in questo albero; `b43_test_emit_amt()` intanto emette la forma
del vendor, quindi finche' la patch non segue i due sono disallineati.

Resta fuori anche dove vanno le due righe in cima. L'harness le mette in coda
all'azzeramento; nella cattura stanno a `#13302` e `#13943`, dentro il bss-up,
tirate da `ADDRM.SET` -- 60 op di una classe che il port non emette affatto --
e in mezzo il vendor rifa un azzeramento parziale di 56 righe (`idx 0x00-0x37`).
E' la prima divergenza che resta su `cold01`, a `@255`.

## La sequenza CAC, per intero

Ricavata da `cold05-ch52-bw20` dello sweep ricatturato, che e' il primo
segmento a freddo in cui il check si chiude dentro la finestra di cattura.

### Dove sta

Il vendor si ferma **fra le passate conf_tx e `op_channel_calibrate`**: il
blocco d'attesa e' inserito esattamente dove `cold01` ha l'entrata di quella
funzione, e tutto quello che segue viene nello stesso ordine -- il 92.7% della
sequenza di `cold01` si ritrova in ordine dentro `cold05`. Non e' un attach
diverso: e' lo stesso attach con dentro un'attesa.

```
op    12477   PHY.MOD 0x02e4                l'arm del check
op    12482   1 poll                        il giro che va con l'arm
op    12616   1 poll
op 12692-12712  6 poll                      cac_poll_pre
op 12716..22841  58 giri di watchdog        l'attesa, ~127 op per giro
op    25338   PHY.WR 0x0380                 il gate si apre: tone generator
op ..47861    65 giri + 3 poll di coda      la fase probe come sempre
```

706 poll in tutto: 8 prima della prima testa di tick, 695 nei giri, 3 in coda.

### Cos'e' un giro d'attesa

Un corpo di watchdog senza measure block, cioe' `wd_body(dev, 0, false, true)`,
piu' i poll che vanno con quel giro. Op per op, dal capture:

```
PHY.RD 0x07af 0x07b3 0x07ab 0x07b1        wd_sample_phase
PHY.RD 0x09af 0x09b3 0x09ab 0x09b1
PHY.RD 0x0523 0x0529 0x0528 0x0527
MAC suspend; OBJ.WR 0x0308..0x0312 = 0; MAC enable     latch e clear
PHY.MOD 0x0520 mask=0x000c                             il mode change
OBJ.RD 0x010e 0x0158 0x010c 0x015e                     head sweep
OBJ.RD 0x0768..0x078a
OBJ.RD 0x0768..0x077e  x2                              le due passate ctr32
OBJ.RD 0x07e2..0x07da
OBJ.RD 0x015a 0x014e
OBJ.RD 0x008c 0x0308..0x0314                           la coda statistiche
[ MAC suspend; PHY.RD 0x0251; PHY.RD 0x0252; MAC enable ] x N   i poll
```

I poll stanno **in fondo al giro**, dopo la coda statistiche, non fra il corpo
e il mode change: `PHY.MOD 0x0520` e' all'offset 20 del giro e i poll agli
offset 103-127 su 131. Il commento di `b43_phy_ac_cac_poll()` descrive l'altra
posizione, ed e' da correggere -- vale per i giri della fase probe come per
quelli dell'attesa, verificati entrambi.

### I conteggi per giro

Attesa, 58 giri: `7,6,6, 7,6,7,7,6,7,7,6,...` -- un andamento regolare 6/7, che
e' il battito da 151.5 ms della routine di attesa contro il secondo del tick.

Probe, 65 giri: `7,1,6,7,1,1,6,7,6,1,1,6,...` -- irregolare, con gli 1 dove il
giro cade corto.

`reverse-tools/cac_polls.py` estrae gia' questa sequenza; quello che non fa e'
separare i giri d'attesa da quelli della fase probe, e sono due fasi distinte
che il flow esegue in due punti diversi.

### Cosa manca per chiudere

1. `cac_poll_tick[]` e' 24 elementi e qui servono 123 (58 + 65).
2. Serve `cac_wait_ticks`, ricavato dalla cattura come gli altri orologi: e' il
   numero di teste di tick fra l'arm e la prima `PHY.WR 0x0380`. Il loop probe
   in `rxiqcal_finalize()` indicizza allora `cac_poll_tick[cac_wait_ticks +
   tick]`. L'indicizzazione attuale e' monotona e con un solo consumatore per
   giro (`wd_turn` per `tick` 0..ticks-1, piu' il giro di chiusura a `ticks`),
   quindi l'offset non cambia semantica alle catture vecchio stile, dove
   `cac_wait_ticks` e' zero.
3. `cac_polls.py` deve emettere le due liste separate.
4. Il flow deve eseguire i giri d'attesa fra `emit_core_conf_tx_passes()` e
   `channel_calibrate()`, e poi azzerare `cac_pending`.

Il punto aperto e' il 4, e non e' tecnico. Il loop d'attesa **non e' codice del
driver**: su hardware quei giri sono il periodic work che ticchetta mentre
mac80211 tiene sospeso il bring-up dell'AP, e il poll del rivelatore e' codice
che b43 non ha ancora -- `b43_phy_ac_op_pwork_15sec()` chiama
`b43_phy_ac_watchdog(dev, true)` e non polla niente. Quindi o il giro d'attesa
sta nell'harness accanto a `emit_core_bss_config()` e alle passate conf_tx, che
e' dove stanno le altre cose che l'harness fa al posto dello stack, e allora
`b43_phy_ac_cac_poll()` va reso raggiungibile da la'; oppure si mette in `src/`
una entrata senza chiamanti su hardware, che e' quello che questo albero ha
evitato di fare finora.

## `PHY.RDW`: la tabella 0x20 si legge a 16 bit invece che intera

Classificata, e non e' un'ombra dell'accessore come l'intestazione bulk: sono
38 letture che il port non fa.

Le 19 letture larghe della tabella 0x20 hanno tutte la stessa forma:

```
PHY.WR  addr=0x000d val=0x0020      TABLE_ID
PHY.WR  addr=0x000e val=<off>       TABLE_OFFSET
PHY.RD  addr=0x0011 val=0x0035      una word della cella
PHY.RDW val=0x00001300
PHY.RDW val=0x0000f32f
```

Il port si ferma dopo la `PHY.RD 0x0011` e passa oltre. Che le due `PHY.RDW`
non siano una resa diversa delle letture sulla porta dati lo dicono i
conteggi: `PHY.RD 0x000f` sta a 343 su entrambi i lati e `0x0010` a zero su
entrambi, quindi non e' che il vendor renda come `RDW` letture che il port
rende come `PHY.RD`. Sono accessi in piu'.

Valori visti nelle 38: `0x7f00` (15), `0xf3ff` (11), `0xf32f` (6), `0x1300`
(4), `0xf34f` (2).

Da fare: capire cos'e' la tabella 0x20 e con che larghezza va letta, e
correggere la larghezza in `b43_actab_read` sul sito che la interroga. Le word
che il port non legge sono word che non sta usando, quindi non e' solo un
difetto di traccia.

## `ADDRM.SET`: l'azzeramento delle righe MAC delle chiavi

La regione di divergenza piu' grossa che resta su `cold01`, 224 op in un blocco
solo. Il vendor, per ognuna delle 56 righe da `idx 0x00` a `0x37`, emette:

```
ADDRM.SET idx=0x0000
AMT.WR    idx=0x0000
OBJ.BULKR addr=0x0000 len=8 a5=0x00040000
OBJ.BULKW addr=0x0000 len=8 a5=0x00040000
```

Il port non emette niente di tutto questo. Le 56 righe sono quelle sotto le
otto in cima -- `B43_AMT_WIDE_ENTRIES` meno `B43_NR_GROUP_KEYS * 2` -- cioe' le
righe MAC delle chiavi pairwise, e l'azzeramento e' `b43_clear_keys()` del core
che chiama `keymac_write(dev, i, NULL)` su ognuna. Con `B43_AMT_KEEP_FLAGS` la
riga viene riletta, che e' esattamente la `OBJ.BULKR` che la cattura mostra:
i due livelli di hook del vendor, `ADDRM.SET` sopra e `AMT.WR` sotto,
corrispondono a `keymac_write` sopra e `b43_amt_write` sotto.

Il punto di inserimento non e' ambiguo. Nella cattura il blocco sta fra
l'azzeramento di `OBJ.WR 0x0658..0x0666` e la `MAC.MHF 0x0000 mask=0x4000` che
segue, e il port emette quelle due cose nello stesso ordine, adiacenti:
`/tmp/p1/full` righe 11019, 11020.

Non l'ho aggiunto, perche' quel punto cade **dentro**
`b43_phy_ac_shm_readback_block()`, che il suo stesso commento dichiara codice
del core parcheggiato nel PHY: "These are cells of the MAC, not of the PHY, so
this belongs in the core. It sits here because the captures put it between the
PHY write of 0x0339 and the host flag that follows, and the core has no hook at
that point". Metterci dentro anche l'azzeramento delle chiavi raddoppia quel
difetto invece di risolverlo.

E' lo stesso problema, non uno nuovo: serve un punto di aggancio per il core in
quella posizione del flow. Con quello, ci vanno sia le celle che oggi sono
parcheggiate la' sia le 56 righe, ognuna dal suo doppione `emit_core_*`.

Le due righe in cima (`0x3e` BSSID, `0x3f` indirizzo di stazione) seguono la
stessa strada, ma piu' tardi: `ADDRM.SET idx=0xffffffff` e `0xfffffffe` dentro
il bss-up, due righe ciascuno. `emit_core_amt()` le emetteva in coda
all'azzeramento del core init, dove la cattura non le ha; sono state togliate,
perche' sei op nel punto sbagliato costano piu' che nessuna op.

## Il grappolo di coda: sono tutte ricariche del beacon

Le sei regioni di divergenza fra `@25643` e `@29220` su `cold01`, ~380 op fra
mancanti e di troppo, sono una cosa sola. Ogni regione e' un blocco di ricarica
del beacon, e i blocchi ci sono su entrambi i lati ma **cadono a tick
diversi**: dove il vendor ha una ricarica il port ha una spazzata di contatori,
e viceversa, alternandosi.

Il blocco, dal capture:

```
OBJ.RD   addr=0x00cc                    \
OBJ.WR   addr=0x00cc                     |
OBJ.WR   addr=0x001e val=0x0044          | il beacon
TPL.RAMW addr=0x0480 val=0x012c          |   template alternato
OBJ.WR   addr=0x001a val=0x012b          /   e la sua lunghezza
TPL.RAMW addr=0x0700 val=0x011c         \  la probe response
OBJ.WR   addr=0x004a val=0x0119         /   e la sua lunghezza
OBJ.BULKW addr=0x0160 len=32 + le word     l'SSID
```

Due cause distinte, e vanno separate prima di toccare qualcosa.

**La prima e' deliberata.** Il `TPL.RAMW 0x0700` -- 16 occorrenze nel vendor,
zero nel port -- e la `OBJ.WR 0x004a` che lo segue sono il template della
probe response, che b43 non carica di proposito: `PRMAXTIME=1` in
`b43_wireless_core_init()` e il firmware non risponde ai probe. E' scritto in
`test/unit/README.md` e non e' debito. La sequenza degli indirizzi lo mostra
bene: il vendor fa `0x0200 0x0700 0x0480 0x0700 0x0200 0x0700 ...` e il port
`0x0200 0x0480 0x0200 0x0480 ...` -- il template del beacon alterna su entrambi,
quello della probe response c'e' solo da una parte.

**La seconda e' lo sfasamento.** `AC_BEACON_RELOADS=2:2,6,7,13,14,19,21,26,26`
viene applicato a tick che non sono quelli della cattura, e ogni blocco fuori
posto costa due volte: 39 op mancanti nel punto giusto e 39 di troppo in quello
sbagliato. Da guardare in `beacon_reloads.py`: l'estrazione ha smesso di essere
cieca solo di recente -- prima il campo `sel=` la azzerava -- quindi i tick che
produce non sono mai stati verificati contro un allineamento riuscito.

## Il blocco CRS di finalize sta un giro troppo presto

Dopo la correzione dell'attribuzione delle ricariche, la prima divergenza che
resta su `cold01` fuori dalle voci gia' note e' a `@25639`, e sono tre regioni
che sono una cosa sola: 26 op di troppo nel port, 68 di troppo subito dopo, 26
mancanti novantacinque op piu' avanti. Il blocco da 26 e' identico op per op
sui due lati -- la coda delle statistiche (`0x8c`, `0x308..0x314`), il suspend,
i sedici `PHY.MOD` su `0x321-0x336` e `0x910-0x913`, l'enable -- cioe' la
scrittura CRS in coda a `rxiqcal_finalize()` piu' `prog_bank_0910()`.

Il criterio che lo inchioda e' il conteggio dei corpi di giro che la precedono,
contati sul cambio di modo `PHY.MOD 0x0520 mask=0x000c`:

```
vendor:  1 corpo prima della scrittura CRS,  26 in tutto
port  :  0 corpi prima,                      26 in tutto
```

Un giro, non un'op. Il port scrive il CRS prima di entrare nel ciclo del probe;
la cattura ne esegue un giro e poi scrive. Da qui vengono tutte e tre le
regioni, ed e' anche la spiegazione della spazzata di contatori da 68 op che il
port sembra avere in piu': non ce l'ha in piu', ce l'ha nel posto dove il
vendor non ce l'ha ancora, e viceversa novantacinque op dopo.

### Provato a spostarlo, e non e' un ramo: e' un orologio

Il ramo che c'era, `if (may_calibrate_tx()) stats_latch_and_crs()` prima della
fase e il blocco E dietro il giro 1 sopra i 5250, veniva dalle distanze misurate
dalla cella a `0xffff`: `+1/+10` sul vecchio cold01 e `+198/+207` su cold05.
Sul nuovo cold01 sono `+96/+105`, cioe' un corpo di giro in mezzo, e sembrava
che il caso "sotto i 5250" fosse solo mal misurato.

Spostato il blocco dietro il giro 0 nel caso non-radar, il risultato e' un
pareggio: cold01 da 92.74% a 92.88%, ma cold02 da 97.70% a 97.53% e cold03 da
97.79% a 97.62%. Modifica annullata.

Il motivo lo dicono le distanze su tutta la famiglia:

```
cold01-ch36-bw20     +96 / +105     un corpo di giro in mezzo
cold02-ch40-bw20      +1 /  +10     niente in mezzo
cold03-ch44-bw20      +1 /  +10     niente
cold26-ch36-bw40      +1 /  +10     niente
cold38-ch36-bw80     +73 /  +82     qualcosa in mezzo
cold14-ch120-bw20    +198 / --      DFS, due giri, blocco E dentro il giro
```

Non e' una proprieta' del canale ne' della larghezza: `ch36` a 20 e a 80 MHz ha
un giro in mezzo e a 40 no, `ch40` e `ch44` no. E' **se un tick del periodico
e' caduto in quel varco**, che cambia da corsa a corsa.

### E non basta nemmeno un contatore di giri

Il passo dopo sembrava una leva intera: quanti corpi di giro cadono nel varco,
0, 1 o 2, contati sul cambio di modo `PHY.MOD 0x0520 mask=0x000c`, con il
blocco E dietro il giro N-1. Misurata, non regge neanche quella:

```
segmento              op nel varco   corpi   cosa c'e' dentro
cold02-ch40-bw20            0          0     niente
cold03-ch44-bw20            0          0     niente
cold04-ch48-bw20            0          0     niente
cold26-ch36-bw40            0          0     niente
cold38-ch36-bw80           72          0     una spazzata statistiche nuda
cold01-ch36-bw20           95          1     un corpo intero
cold14-ch120-bw20         197          1     un corpo piu' una spazzata
```

Il varco di `cold38` sono 72 op tutte `OBJ.RD`: `0x010e 0x0158 0x010c 0x015e`,
poi `0x0768..`, poi `0x015a 0x014e`. E' la spazzata dei contatori da sola,
senza `MAC.MCTRL`, senza il `sample_phase` su `0x07af` e senza il cambio di
modo. Quello di `cold01` sono le stesse 72 piu' il suspend, i dodici
`sample_phase`, la `PHY.WR 0x554` e il cambio di modo: un corpo completo.

Quindi le forme sono quattro e crescono per pezzi di giro, non per giri:
niente, una spazzata, un corpo, un corpo piu' una spazzata. Il varco non
contiene un numero intero di giri perche' la cella a `0xffff` cade **dentro**
un giro, in un punto che cambia da corsa a corsa.

Modellarlo fedelmente vuol dire dire all'harness la fase del periodico a quel
punto, con risoluzione sotto il giro -- non "quanti giri" ma "a che punto del
giro" -- e questo tocca la scomposizione di `wd_body()`, non solo il suo
chiamante. E' l'unica strada che copre tutti e sei i segmenti, ed e' la
prossima cosa da fare qui, ma va progettata: una leva intera risolverebbe
`cold01` e `cold14` e lascerebbe `cold38` come sta.

Fino a quel momento il ramo attuale e' la migliore fra le scelte sbagliate
disponibili: indovina su quattro segmenti su sei di questa famiglia, e sono i
quattro con il varco vuoto.

## L'SSID e' cambiato di un carattere, e con lui cinque letterali

Tutta la famiglia di valori "uno sotto" ha una causa sola, e non e' nel
driver: la cattura vecchia ha SSID `test-ap`, sette caratteri, quella nuova
`test-ap5`, otto.

```
gold/merged   OBJ.WR 0x0160 ...  ->  "test-ap"
q1/merged     OBJ.WR 0x0160 ...  ->  "test-ap5"
```

Un byte in piu' nel probe response sposta la sua lunghezza, e da quella
derivano i PLCP degli otto rate. `b43_phy_ac_prb_rsp_len()` ha una tabella di
letterali, 284/285/286 per 20/40/80 MHz, tarata sui sette caratteri. Il conto
torna esatto:

```
len=284 -> tmp = 284<<5 = 0x2380 -> plcp01 = 0x2380 | dirmap = 0x238b   il port
len=285 -> tmp = 0x23a0          -> 0x23ab                              wl, bw20
len=286 -> tmp = 0x23c0          -> 0x23cb                              wl, bw40
```

cioe' il valore che il port scrive su bw40 e' quello che il vendor scrive su
bw20, che e' esattamente la firma osservata. Togliendo l'SSID: la base e'
277/278/279 piu' la lunghezza dell'SSID, e con 7 da' 284/285/286 e con 8 da'
285/286/287. Verificato su entrambe le catture.

E dalla stessa lunghezza dipendono altri quattro letterali, tutti tarati su
`test-ap`:

```
emit_core_bss_ssid()   0x001e = 0x0043      la cattura nuova ha 0x0044
                       beacon_tpl_len()     0x18/0x1a: 0x12b contro 0x12c
emit_core_bss_config() 0x0048 PRSSIDLEN
                       0x004a PRTLEN
b43_phy_ac_prb_rsp_plcp()  la durata 20 + nsym*4 + 16: 0x1a4 contro 0x1a8,
                       un simbolo in piu', che e' lo stesso byte
```

Resa: 48 op su cold01 e 126 su cold26, piu' le celle di lunghezza, ed e' la
famiglia di valori sbagliati piu' grossa dopo i contatori servita
dall'oracolo.

### Fatto

La lunghezza dell'SSID e' ora un ingresso solo, `AC_SSID_LEN` (default 8), e da
lei discendono `b43_phy_ac_prb_rsp_len()` -- parte fissa 277/278/279 piu'
l'SSID -- `beacon_tpl_len()` e la cella `0x001e`. Il campo `ssid_len` sta in
`struct b43_phy_ac` con il perche' scritto accanto.

```
segmento             prima     dopo     regioni
cold01-ch36-bw20     92.74%   94.64%    251 -> 158
cold02-ch40-bw20     97.70%   98.47%    201 ->  85
cold03-ch44-bw20     97.79%   98.63%    190 ->  63
cold14-ch120-bw20    97.17%   97.97%    111 ->  43
cold26-ch36-bw40     95.69%   97.97%    477 -> 126
cold38-ch36-bw80     96.53%   97.06%    245 -> 149
```

La prova che sia una parametrizzazione e non un +1 travestito e' la cattura
vecchia, che ha sette caratteri: con `AC_SSID_LEN=7` da' 99.19% e 92 regioni,
con 8 da' 98.58% e 181. Funziona in entrambi i versi.

Resta da fare che il valore lo ricavi un tool invece che un default: e'
leggibile dalla cattura, PRSSIDLEN sta a `0x0048` e l'SSID a `0x0160-0x017e`,
quindi va accanto a `watchdog_turns.py` e `gates.sh` lo passa come gli altri.
Finche' e' un default, un segmento con un SSID diverso sbaglia in silenzio.

Nota di lettura: il commento di `emit_core_bss_ssid()` diceva "l'SSID e' quello
della cattura, `test-ap`". Era vero.

### E una conseguenza sulle catture vecchie

Lo sweep vecchio ora prende 99.19% contro il 99.90% che prendeva all'inizio
della sessione, e non e' l'SSID: e' `PHY.FGC`. Il port ne emette 76 e quelle
catture non tracciano quella classe, quindi la' sono op di troppo. E' proprio
il caso per cui esiste `SOLO_PORT_SENZA_CLASSE` in `compare.py`, dove ci sta
gia' `AMT.*`: `PHY\.FGC` va aggiunto la', e si spegnera' da se' contro ogni
cattura che lo traccia.

## Fase d'attesa CAC: plumbing dentro, allineamento no

Il loop d'attesa e' in `src/`, come deciso: `b43_phy_ac_cac_wait()` esegue
`cac_wait_ticks` giri -- corpo senza measure block, piu' i poll che gli
spettano dalla stessa lista -- e poi azzera `cac_pending`. Il commento dice che
su hardware non ha chiamanti e perche'. `cac_poll_tick[]` e' passato da 24 a
160 elementi, e il loop della fase probe indicizza ora
`cac_poll_tick[cac_wait_ticks + tick]`, cosi' i due gruppi si consumano in due
punti diversi senza sovrapporsi. La leva e' `AC_CAC_WAIT_TICKS`, default 0, e
con zero tutto si comporta come prima -- verificato: cold01 94.64%, cold14
97.97%, cold05 62.15%, identici.

Con la leva accesa su cold05, che ne vuole 58:

```
AC_CAC_WAIT_TICKS=0    29775/47912 = 62.15%   192 regioni
                 =55   31923/63214 = 50.50%   649
                 =58   32541/62853 = 51.77%   571
                 =60   33123/62443 = 53.05%   615
```

Le op che combaciano salgono di ~2800, ma il denominatore di ~15000, quindi il
grezzo scende. I 15000 sono le calibrazioni: azzerando `cac_pending` il gate si
apre e il port le emette, il che e' il punto dell'esercizio -- prima non le
emetteva affatto e quelle 13000 op del vendor erano tutte mancanti. Adesso
vengono emesse e non si allineano.

Quindi la fase e' nel posto giusto e la forma del giro d'attesa e'
approssimativamente giusta (~56 op su ~130 combaciano per giro), ma dopo di
essa c'e' una divergenza strutturale che prima era invisibile perche' il port
non arrivava mai la'. Il prossimo passo e' guardare la prima divergenza dopo
la fine della fase d'attesa, con la leva accesa, che e' un terreno che nessuno
ha ancora visto.

Da controllare come prima cosa, perche' sono le candidate ovvie:

- i due bool di `b43_phy_ac_wd_sample_phase_opt()`, che nel giro d'attesa ho
  messo a `(true, false)` senza sapere cosa siano: nel loop della fase probe
  sono `(tick != 1 && tick != 2, tick == 0)`;
- se il giro d'attesa debba latchare la finestra (`tail`): l'ho messo a `true`
  perche' il giro nella cattura ha l'azzeramento `OBJ.WR 0x0308..0x0312`, ma
  non ho verificato quale dei due lo emette;
- la posizione dell'arm: `b43_phy_ac_cac_arm()` sta in `post_bringup_tail()`,
  cioe' dopo le calibrazioni, e nella cattura DFS sta a `#12477`, prima
  dell'attesa. Sulle catture DFS vecchie la posizione attuale funziona (cold14
  sta a 97.97%), quindi spostarla va misurato su entrambe le famiglie.

### La prossima divergenza con l'attesa accesa: il measure block

Con `AC_CAC_WAIT_TICKS=58` su cold05, tolte le voci gia' note (OTP a `@0`,
`ADDRM.SET` a `@10455`), la prima divergenza nuova e' a `@11937` e sono **614
op che il port non emette affatto**: `PHY.MOD 0x019e` sui mask 0x40/0x80/0x100,
poi le letture su `0x0720-0x073e` con 64 `RAD.RD` e 64 `RAD.WR`, e in coda
l'azzeramento della finestra e il cambio di modo. E' un measure block piu' il
corpo che lo porta.

Contando i measure block sul marcatore `PHY.RD 0x073c` e i corpi sul cambio di
modo:

```
cold01 (26 corpi)    measure a corpi  10, 20
cold05 (126 corpi)   measure a corpi  9, 19, 29, 39, 49, 59, [60], 69, 79,
                                      89, 99, 109, 119
```

(i dieci a "corpo 0" su cold01 e il gruppo a "60" su cold05 sono letture di
`0x073c` fatte dalla calibrazione, non measure block: il marcatore e' pulito
solo dopo il primo cambio di modo.)

Quindi **il measure block e' periodico, ogni dieci giri**, con offset 10 su
cold01 e 9 su cold05 -- uno di scarto, che e' la fase d'attesa che sposta il
conteggio. Non e' una lista arbitraria: e' un periodo, e l'offset dipende da
dove comincia a contare.

`watchdog_turns.py` pero' ne riporta due: `AC_WATCHDOG_TICKS=9,19` su cold05,
dove il vendor ne fa dodici fino al corpo 119. Su cold01 riporta `10,20` ed e'
giusto, perche' la' i corpi sono 26 e dopo il 20 non ce n'e' piu'. Il difetto
si vede solo su un segmento lungo, e i segmenti lunghi sono arrivati con la
ricattura DFS.

Da fare, ed e' lo stesso intervento fatto su `beacon_reloads.py`: contare i
corpi invece di dedurre la fase, riportare tutti i giri con measure block, e in
`b43_phy_ac_wd_turn()` consultare `b43_phy_ac_watchdog_on_tick()` con l'indice
spostato di `cac_wait_ticks`, esattamente come si fa ora per i poll. Il giro
d'attesa di `b43_phy_ac_cac_wait()` deve passare `watchdog_on_tick(ac, turn)`
invece del `false` fisso che ha adesso.

### Fatto: la lista dei measure block era troncata a due

In `probe_schedule.py` c'era `watchdog[:2]`, e l'attribuzione del giro passava
per i gruppi su `PHY.RD 0x07af`. Corretto entrambi: i giri si contano sul
cambio di modo `PHY.MOD 0x0520 mask=0x000c`, uno per corpo, e si riportano
tutti. Le voci duplicate sullo stesso corpo si scartano, perche' dentro la
regione delle calibrazioni `0x073c` viene letto anche da loro e un measure
block due volte sullo stesso giro non esiste.

`probe_watchdog_tick[]` passa da due slot a 32 con un contatore, e l'indice e'
quello globale come per `cac_poll_tick[]`: `b43_phy_ac_wd_turn()` consulta
`watchdog_on_tick(ac, cac_wait_ticks + tick)` e il giro d'attesa
`watchdog_on_tick(ac, turn)` invece del `false` fisso che aveva.

```
                          prima      dopo
cold01-ch36-bw20          94.64%    94.64%    invariato
cold02-ch40-bw20          98.47%    98.47%    invariato
cold14-ch120-bw20         97.97%    97.97%    invariato
cold38-ch36-bw80          97.06%    98.22%
cold05, attesa spenta     62.15%    64.44%
cold05, attesa a 58       51.77%    67.37%
```

E' la prima volta che la fase d'attesa **conviene**: 67.37% con i 58 giri
contro 64.44% senza. Le op che combaciano passano da 32541 a 40141.

Resta da ricavare `cac_wait_ticks` dalla cattura invece di passarlo a mano --
e' il numero di corpi fra l'arm e la prima `PHY.WR 0x0380` -- e poi `gates.sh`
lo passa come gli altri. Finche' e' a mano, i 27 segmenti DFS restano fuori
dal conteggio.

## Finire cold01: cosa resta, e il varco non si chiude col giro d'attesa

Stato a 94.64%: 59 regioni, 364 op mancanti, 158 di troppo, 160 valori.

```
@48, @29844      11 op    il LED. Fuori da src/, patches/0016-0017.
@11028           242 op   ADDRM.SET, le 56 righe chiavi + le due coppie
@25639-@25734    126 op   il varco del latch: blocco CRS e spazzata dislocati
@26317-@29305     76 op   quattro gruppi 8+8+6: latch e clear dentro il giro
valori           125      i contatori 0x768/0x780/0x782, serviti dall'oracolo
valori            11      la cella 0x001e
```

I 125 valori dei contatori sono sintomo del varco, non difetti a se': l'oracolo
serve la lettura sbagliata da quando i due flussi si sfasano. Quindi il varco
vale ~325 delle ~690 anomalie e `ADDRM.SET` 242: chiusi quei due, cold01 e'
finito a meno del LED e degli 11 valori su `0x001e`.

### Terzo tentativo sul varco, e terzo fallimento

Con `b43_phy_ac_cac_wait()` in piedi sembrava naturale generalizzarla: non "la
fase d'attesa del CAC" ma "N giri prima delle calibrazioni", con N=1 su cold01,
0 su ch40/44/48 e 58 su cold05. Provato:

```
cold01, wait=0   94.64%
cold01, wait=1   93.87%      e 92.83% con l'ultimo giro senza tail
cold01, wait=2   90.71%
```

Non e' questione di parametro: **il giro che il vendor mette nel varco e' 95 op
e un corpo intero ne fa ~170**. Il varco contiene `OBJ.RD 0x010e 0x010c 0x0158
0x015e`, il suspend, i dodici `sample_phase`, la `PHY.WR 0x0554`, la spazzata,
e chiude su `OBJ.RD 0x07de 0x07d6 0x07d8 0x07da 0x015a 0x014e` -- cioe' un
corpo **potato**, non un corpo. Le quattro forme misurate ieri (niente / una
spazzata nuda / un corpo / un corpo piu' una spazzata) sono pezzi di giro, e
nessun conteggio di giri interi le produce.

La guardia su `cac_pending` e' stata rimessa: la generalizzazione e' annullata,
cold01 torna a 94.64% e cold05 con la leva resta a 67.37%.

Il varco vuole la scomposizione di `wd_body()` in stadi e una leva che dica da
quale stadio si entra -- la cosa che avevo detto ieri e che resta vera dopo tre
tentativi di aggirarla. Finche' non c'e', cold01 si ferma dove e' adesso piu'
quello che dara' `ADDRM.SET`.

### Il varco, op per op: e' un'interlacciatura, non un giro mancante

Allineamento completo dei due lati dopo la cella `OBJ.WR 0x0026 val=0xffff` su
cold01. Le graffe sono le op che combaciano.

```
vendor                                   port
------------------------------------     ------------------------------------
{OBJ.WR 0x0026 val=0xffff}               {OBJ.WR 0x0026 val=0xffff}
                                         OBJ.RD 0x008c 0x0308..0x0314    latch
                                         MAC susp; 16x PHY.MOD; MAC en   blocco E
{OBJ.RD 0x010e 0x010c 0x0158 0x015e}     {OBJ.RD 0x010e 0x0158 0x010c 0x015e}
                                         OBJ.RD 0x0768..0x07da 0x015a 0x014e
{MAC susp; 12x sample_phase;             {MAC susp; 12x sample_phase;
 PHY.WR 0x0554 0x0555; MAC en}            PHY.WR 0x0554 0x0555; MAC en}
                                         OBJ.WR 0x0308..0x0312          clear
{MAC susp; PHY.MOD 0x0520; MAC en}       {MAC susp; PHY.MOD 0x0520; MAC en}
{OBJ.RD 0x0768..0x07da 0x015a 0x014e}    {OBJ.RD 0x0768..0x07da ...}
OBJ.RD 0x008c 0x0308..0x0314    latch
MAC susp; 16x PHY.MOD; MAC en   blocco E
{OBJ.WR 0x0308..0x0312}   clear          {OBJ.WR 0x0308..0x0312}
```

Non manca un giro al port: **la testa della spazzata e il suo corpo, sul primo
giro della fase, nel vendor sono separati da `sample_phase` e dal cambio di
modo**, e il latch con il blocco E stanno dopo il corpo invece che prima della
testa. Il port emette le stesse op nello stesso giro, in un ordine diverso.

Che sia solo il primo giro lo dice il confronto: dal giro dopo l'ordine e'
`[cambio modo][testa][corpo]` su entrambi i lati e combacia per 557 op di
fila. E l'inversione `0x010c`/`0x0158` dentro la testa e' un'altra cosa
ancora, due op, e c'e' solo su questo giro.

Quindi la leva che ho provato tre volte -- quanti giri prima del latch --
non c'era modo che funzionasse: il numero di giri e' lo stesso, e' l'ordine
dentro il primo a essere diverso. La correzione sta in come `wd_body()` e
`wd_sample_phase_opt()` si intrecciano all'ingresso della fase, ed e' un
riordino di chiamate in `b43_phy_ac_post_bringup_tail()`, non un ingresso
dalla cattura.

Le quattro forme del varco (niente su ch40/44/48 e bw40, 72 op su bw80, 95 su
cold01, 197 sui DFS) sono quanta parte di quell'intreccio cade prima del
latch, e con l'ordine giusto dovrebbero venire da se'. Da verificare: se
cambiando l'ordine i quattro casi tornano tutti, allora non serve nessuna leva
nuova -- che sarebbe il primo pezzo di questa zona a chiudersi senza.

### Il riordino non e' un riordino di chiamate: `wd_stats_poll()` va spezzata

Andato a fare lo scambio in `b43_phy_ac_post_bringup_tail()` e non e'
esprimibile con le chiamate che ci sono. Con i marcatori di funzione, il port
all'ingresso della fase emette:

```
stats_latch_and_crs()      ->  OBJ.RD 0x008c ...        il latch
  crs_regs_write()         ->  16x PHY.MOD              il blocco E
  prog_bank_0910()
wd_turn(0):
  wd_stats_poll_opt()      ->  testa 0x010e.. + corpo 0x0768..
  wd_sample_phase_opt()    ->  PHY.RD 0x07af ...
```

Il vendor all'ingresso vuole invece:

```
testa 0x010e 0x010c 0x0158 0x015e        <- prima meta' di wd_stats_poll()
sample_phase + PHY.WR 0x0554 0x0555
cambio di modo 0x0520
corpo 0x0768..0x07da 0x015a 0x014e       <- seconda meta'
latch + blocco E
clear
```

cioe' `sample_phase` e il cambio di modo cadono **dentro** la spazzata, fra la
testa e il corpo. Con `wd_stats_poll_opt()` che emette testa e corpo in un
colpo, nessuna permutazione delle chiamate esistenti lo produce: serve
spezzarla in due, testa e corpo, e all'ingresso della fase chiamarle con
`sample_phase` e il cambio di modo in mezzo.

Per riferimento, il giro a regime -- che combacia per 557 op di fila subito
dopo -- ha l'ordine `[clear][cambio modo][testa][corpo][latch]`. Quindi la
spezzatura serve solo all'ingresso e il giro normale resta una chiamata sola,
che e' la ragione per cui non se ne era mai visto il bisogno.

Questa e' la quarta caratterizzazione del varco e la prima che arriva al
livello delle funzioni. Le tre precedenti -- quante forme, quanti giri, che
ordine -- erano tutte vere e tutte insufficienti a scrivere la correzione.

### La scomposizione che serve, per intero

Lette le due funzioni, il varco vuole cinque pezzi e non due.

`b43_phy_ac_wd_stats_poll_opt()` tiene sotto lo stesso `if (head_sweep)` due
cose che all'ingresso della fase sono separate:

```c
if (head_sweep) {
        for (i = 0; i < ARRAY_SIZE(head); i++)     /* 1: le quattro sparse */
                b43_shm_read16(dev, B43_SHM_SHARED, head[i]);
        for (off = 0x0768; off <= 0x078a; off += 2) /* 2: la spazzata piatta */
                b43_shm_read16(dev, B43_SHM_SHARED, off);
}
```

e `b43_phy_ac_wd_sample_phase_opt()` ne tiene tre:

```
3: suspend, le dodici letture di fase, 0x0554/0x0555 se arm_tone, enable
4: b43_phy_ac_wd_stats_clear()
5: suspend, PHY.MOD 0x0520 (il cambio di modo), enable
```

Ordine a regime, che combacia per 557 op:   `4 5 1 2 [latch]`
Ordine all'ingresso, dalla cattura:         `1 3 5 2 [latch] [blocco E] 4`

Quindi all'ingresso il pezzo 4 (il clear) scivola in fondo, dietro il blocco E,
e diventa l'apertura del primo giro del ciclo; e il pezzo 3 si infila fra 1 e
5. Non e' una permutazione delle chiamate: sono cinque pezzi dentro due
funzioni, e vanno estratti.

C'e' anche un dettaglio che avevo messo da parte come "due op a se'": la
tabella e' `head[4] = { 0x010e, 0x0158, 0x010c, 0x015e }`, che e' l'ordine a
regime, mentre all'ingresso la cattura ha `0x010e, 0x010c, 0x0158, 0x015e`.
Non e' un'altra anomalia: e' il pezzo 1 che all'ingresso ha il suo ordine.

Da fare, in questo ordine e misurando fra un passo e l'altro:

1. estrarre i cinque pezzi lasciando invariato il giro a regime, che deve
   restare `4 5 1 2`, e verificare che i punteggi non si muovano di niente;
2. all'ingresso della fase chiamarli `1 3 5 2` piu' il latch, il blocco E e il
   clear, con l'ordine proprio del pezzo 1;
3. misurare i quattro casi del varco -- ch40 vuoto, bw80 72 op, cold01 95, un
   DFS 197 -- e i DFS vecchi, che stanno a 97.97% e non vanno rotti.

Il passo 1 e' a rischio zero per costruzione (stessa sequenza, chiamate
diverse) e va committato da solo: se i numeri si muovono di un'op, l'estrazione
e' sbagliata e si vede subito, invece di scoprirlo mescolato al passo 2.

### Passo 1 fatto: i cinque pezzi sono estratti, e non cambiano niente

`b43_phy_ac_wd_head_words()`, `b43_phy_ac_wd_flat_sweep()`,
`b43_phy_ac_wd_peek()` e `b43_phy_ac_wd_mode_next()` escono da
`wd_stats_poll_opt()` e da `wd_sample_phase_opt()`; il quinto,
`wd_stats_clear()`, c'era gia'. Il giro a regime resta `4 5 1 2`, cioe' le due
funzioni chiamano i pezzi nello stesso ordine di prima.

Verifica, che e' il punto di questo passo:

```
cold01-ch36-bw20   94.88%  139 regioni   invariato
cold02-ch40-bw20   98.53%   77           invariato
cold26-ch36-bw40   98.08%  109           invariato
cold38-ch36-bw80   98.26%  150
cold14-ch120-bw20  97.99%   41
```

cold38 e cold14 si muovono di otto e di due op rispetto alle ultime misure, ma
non per l'estrazione: quelle misure erano di prima della derivazione dell'SSID
nel secondo scrittore di `0x001e`, che su quei due segmenti vale quelle op. I
tre misurati dopo quella correzione sono identici alla cifra.

Resta il passo 2, il riordino all'ingresso: `1 3 5 2` piu' latch, blocco E e il
clear in coda, con l'ordine proprio del pezzo 1. Adesso e' scrivibile, perche' i
pezzi ci sono.

### Passo 2 provato e annullato, ma ora il varco ha un alfabeto

Coi pezzi estratti si possono confrontare i due flussi **come sequenze di
pezzi**, che e' l'artefatto che mancava. Dalla cella a `0xffff`, con
1 = quattro celle sparse, 2 = spazzata, 3 = letture di fase, 4 = clear,
5 = cambio di modo, L = latch, E = blocco E:

```
wl     1 3 5 | 1 2 L E 4 | 5 1 2 L 4 | 5 1 2 L 3 4 | 5 1 2 ...
port   L E   | 1 2 3 4   | 5 1 2 4   | 5 1 2 L 4   | 5 1 2 ...
```

Da qui si legge tutto. Il latch e il blocco E **non** stanno prima della fase:
il latch e' la coda del primo giro e il blocco E viene dietro di lui. Quello
che sta prima della fase e' un prefisso di tre pezzi, `1 3 5`. Dal secondo giro
in poi i due flussi sono la stessa sequenza.

Provato: prefisso `1 3 5` prima della fase, giro 0 che latcha, blocco E dietro
il latch del giro 0. Risultato:

```
                   prima     dopo
cold01-ch36-bw20   94.88%   93.92%    ma 139 -> 111 regioni
cold02-ch40-bw20   98.53%   97.71%
cold38-ch36-bw80   98.26%   97.68%
cold14-ch120-bw20  97.99%   97.99%
```

Annullato. Il motivo e' quello di sempre: **il prefisso non e' lo stesso su
tutti i segmenti**. cold01 vuole `1 3 5`, ch40/44/48 e bw40 vogliono il varco
vuoto -- e la' i tre pezzi sono puro sovrappiu' -- bw80 vuole `1 2` (72 op:
quattro celle sparse, la spazzata, `0x015a 0x014e`), i DFS vogliono di piu'.

Ma adesso il problema e' posto bene. Non e' "quanti giri" ne' "che ordine": e'
**quale prefisso di pezzi** cade prima della fase, con l'alfabeto sopra, e va
ricavato dalla cattura come ogni altro orologio del vendor. Il riconoscitore e'
meccanico -- fra la cella a `0xffff` e la prima `OBJ.RD 0x008c` si leggono i
pezzi uno per uno -- quindi e' un tool in `reverse-tools/` accanto a
`watchdog_turns.py`, una stringa tipo `AC_PHASE_PREFIX=1,3,5`, e in
`post_bringup_tail()` un ciclo che emette i pezzi nominati.

Cinque tentativi su questo varco, e questo e' il primo che lascia qualcosa di
riusabile: la tabella dei pezzi. Il prossimo passo e' il riconoscitore, non
un'altra ipotesi sul prefisso.
## Il varco a `@25619` e' uno scambio di blocchi, non un giro mancante

Ricostruito con `cmp_skip.py --verbose`, che elenca le regioni divergenti dopo
le eccezioni. Le op ci sono su entrambi i lati: cambia l'ordine.

Il vendor, per pezzi, subito dopo `OBJ.WR 0x0026 = 0xffff`:

```
25619  head sweep nuda        0x010e 0x010c 0x0158 0x015e          4 op
25623  il tick                MAC.MCTRL, PHY.RD 0x07af 0x07b3 0x07ab 0x07b1,
                              0x09af 0x09b3 0x09ab 0x09b1, 0x0523 0x0529
                              0x0528 0x0527, PHY.WR 0x0554 0x0555,
                              MAC.MCTRL, PHY.MOD 0x0520
25642  il giro di poll        head sweep piena 0x010e 0x0158 0x010c 0x015e,
                              spazzata 0x0768-0x078a, due passate,
                              0x07e0 0x07e4 0x07dc, 0x07d6-0x07da, 0x015a 0x014e
25714  blocco contatori       OBJ.RD 0x008c, 0x0308-0x0314             8 op
25722  blocco AGC             MAC.MCTRL, PHY.MOD 0x0324-0x0913        16 op
25739  clear                  MAC.MCTRL, OBJ.WR 0x0308.. = 0
```

Il port emette **blocco contatori e blocco AGC prima del tick**, dove il vendor
li mette dopo il giro di poll. Le regioni lo dicono senza ambiguita': il
`delete V[25714:25722]` e' lo stesso blocco che compare come `insert` a
`T[25619:25627]`, 95 posizioni prima.

### Cosa NON e'

Tre ipotesi mie, cadute in ordine, lasciate qui perche' costano tempo a
rifarle:

**Non e' un giro di watchdog che la cattura non contiene.** Il conteggio dei
giri per segmento (19-24 a freddo, 7-12 a caldo, zero sui DFS che restano nel
CAC) e' durata di registrazione e non va dedotto da nessuna regola -- questo
resta vero -- ma non spiega questa divergenza, che e' di ordine e non di
numero.

**Non e' lo stato del PHY.** Ogni segmento a freddo e' uno scarico e ricarico
del modulo, quindi lo stato del driver a cold01 e a cold02 e' identico per
costruzione: nessun flag interno puo' distinguerli. La differenza di tempo
all'ingresso della fase (11.60 s su cold01 contro 12.56-12.59 s sugli altri,
con periodo ~1.00 s) e' reale ma riguarda quanti giri stanno nella cattura, non
l'ordine dei blocchi.

**Non sono i LED.** `SI.COREREG off=0x0064` e' gia' ombra di `GPIO.OUT` in
`SHADOW_OFFSETS`; delle otto op GPIO/SI che sopravvivono al fold il perimetro
ne scarta sei. I conteggi grezzi (`GPIO.*` 2 contro 13, `SI.COREREG` 0 contro
41) vanno letti dopo `apply_perimeter()`, non prima.

### L'ultimo giro e' troncato, e costa poco

Lo taglia il down che parte; la coda lunga dopo l'ultima testa e' il down, non
il giro. Allineando le due code op per op su cold01:

```
 14 op   uguali          il giro troncato, emesso da entrambi
  6 op   solo port       OBJ.WR 0x0308 0x030a 0x030c 0x030e ... (clear contatori)
 83 op   uguali
  2 op   solo vendor     OBJ.RD 0x0040
435 op   uguali          il down, identico
  4 op   solo vendor     GPIO.OUT + SI.COREREG, gia' fuori perimetro
 13 op   uguali
```

Costo reale **6 op di troppo e 2 mancanti**, lo 0.03%. Non vale una regione non
contata in piu'.

### Sesto tentativo, annullato: l'orologio del latch non basta

Il codice dichiara l'approssimazione da solo -- "Il latch e il blocco E prima
della fase. Non e' quello che fa il vendor ... ma il prefisso giusto cambia da
segmento a segmento e vuole un ingresso dalla cattura che ancora non c'e'" --
e sembrava che quell'ingresso potesse essere un booleano: se il tick del
periodico e' caduto in mezzo o no.

Provato. `reverse-tools/latch_placement.py` lo legge con tre marcatori che il
port emette identici: la cella `OBJ.WR 0x0026 = 0xffff` come ancora, la testa
di tick (`MAC.MCTRL` + `PHY.RD 0x07af 0x07b3 0x07ab`), e `OBJ.RD 0x008c` che
apre il latch; se il latch viene dopo il tick, il tick e' caduto in mezzo. Il
campo era `latch_in_phase` in `struct b43_phy_ac`, zero in un driver vivo.

**Non funziona, e il tool resta solo come misura.** Il criterio scatta su tre
segmenti e su due dei tre e' sbagliato:

```
             prima     con l'orologio
cold01      95.82%       95.92%     +0.10
cold14      99.48%       98.87%     -0.61
cold32      99.70%       99.09%     -0.61
```

cold32 stava a 99.70% con zero valori sbagliati, cioe' era fra i segmenti
messi meglio dello sweep. Netto negativo, annullato per intero.

### Perche' non basta: il varco ha quattro lunghezze, non due

Il varco e' il corpo del primo giro della fase, `wd_turn(dev, 0)`. Misurate le
op fra la cella a 0xffff e la prima testa di tick, su tutti i segmenti che alla
fase ci arrivano:

```
  4 op    cold01
 98 op    17 segmenti
100 op    i 6 radar-meteo, con MAC.MCTRL:14 e PHY.RD:7 nel profilo
157 op    cold04
```

Un booleano ne distingue due dove ce ne sono quattro, e i 100 op dei
radar-meteo non sono ne' il 4 ne' il 98: sono una forma a se'. Troncare il
corpo a quattro celle li' toglie 532 op che la cattura ha.

Il riconoscitore che serve emette **la lunghezza del varco**, non un bit, e
prima ancora va capito di quali pezzi sono fatte le quattro forme. L'alfabeto
dei pezzi nella sezione precedente e' il punto di partenza; la lettera nuova e'
il troncone da 4 op, che e' head sweep nuda in ordine di indirizzo
(`0x010e 0x010c 0x0158 0x015e`, contro `0x010e 0x0158 0x010c 0x015e` del giro a
regime) e niente altro.

### Il varco di cold01 e' un campione singolo

Decomposto il varco di tutti e 43 i segmenti in pezzi riconoscibili -- testa,
spazzata 0x0768-0x078a, coda contatori, latch (0x008c + 0x0308-0x0314), blocco
E (PHY.MOD 0x0321-0x0336 e 0x0910-0x0913) -- e classificato per due proprieta':
se il corpo del primo giro c'e', e se latch e blocco E stanno davanti.

```
corpo=no  latch-davanti=no    1 segmento    cold01
corpo=si  latch-davanti=no   28 segmenti    i DFS (186 op) e i radar-meteo (100)
corpo=si  latch-davanti=si   14 segmenti    la forma dominante (98 op)
corpo=no  latch-davanti=si    0 segmenti
```

I due bit non sono indipendenti: la quarta combinazione non esiste. E il caso
`corpo=no` e' **uno su 43**.

Vale anche per l'ordine delle quattro celle di testa: 42 segmenti su 43 leggono
`0x010e 0x0158 0x010c 0x015e`, e solo cold01 legge `0x010e 0x010c 0x0158
0x015e`. Il "secondo lettore" inseguito per piu' tentativi esiste in un
segmento su 43.

I 157 op di cold04 non sono una forma a se': sono i 98 con le ricariche del
beacon in mezzo alla spazzata, che `beacon_reload_pre_late` gia' modella.

**Conseguenza.** Il varco di cold01 non va chiuso con una condizione: una
regola tarata su un campione e' una trascrizione con piu' passaggi, ed e'
esattamente come il sesto tentativo ha fallito -- criterio preso da cold01,
applicato ai tre segmenti su cui scattava, sbagliato su due. Se va chiuso, va
chiuso con un ingresso letto dalla cattura che dice cosa emettere, non con un
predicato che finge di spiegarlo.

E prima va deciso se convenga. cold01 e' un segmento su 43 e gli altri 42
stanno fra il 92% e il 99.7%; ottimizzare il segmento di riferimento distorce
la scelta di cosa sistemare, perche' e' quello su cui si guarda il numero.

### Nota di metodo aggiuntiva

Il tentativo e' stato verificato sui segmenti dove l'orologio NON scattava --
dove per costruzione non poteva cambiare niente -- e dichiarato buono. La
regressione e' emersa solo controllando dove scattava. Una modifica
condizionata si misura dove la condizione e' vera, non dove e' falsa.

### Nota di metodo

Sei giri di questa sessione su questo varco, di cui tre di sola misura, e le
misure erano sul quesito sbagliato perche' partivano da conteggi aggregati per
classe invece che dall'allineamento op per op. `cmp_skip.py --verbose` elenca le
regioni divergenti gia' filtrate dalle eccezioni e mostra `delete` e `insert`
appaiati quando un blocco e' spostato: e' lo strumento da aprire per primo su
una divergenza posizionale, prima di contare qualunque cosa.

Vale anche per la scelta di dove guardare: su cold01 le regioni divergenti sono
77 e le prime cinque valgono il 74% del totale, con una sola -- a `@28620`,
1214 op del port a partire da `RAD.WR 0x020e` -- che da sola e' oltre la meta'.

Quel 1214 pero' **non e' un debito di 1214 op**, ed e' un errore di lettura da
non ripetere: misura quanto e' lungo il tratto che l'LCS non riesce ad
appaiare, non quante op sono di troppo. Gli accessi radio combaciano esatti per
fascia di core -- core0 637/637, core1 629/629, core2 282/282, altro 156/156,
delta zero ovunque -- quindi dentro quella regione le op sono le stesse e
cambia l'ordine, come al varco di `@25619`. (Cade con questo anche l'ipotesi
che il `for (core = 0; core < 3; core++)` di rxgain_perchan_tail emetta un core
inesistente: il terzo core il vendor lo tocca, 282 accessi.)

Per sapere quanto vale davvero serve un LCS locale dentro la regione, non il
conteggio globale.

## Il flusso dopo il bring-up e' fatto di eventi, non di tick

La chiusura del CAC "al giro che porta il measure block fuori griglia" era una
lettura sbagliata della cattura, e il modello a tick che la reggeva era
sbagliato per una ragione piu' grossa: quei tick non esistono su hardware.
Cosa dicono i timestamp, su tutti e 21 i segmenti che chiudono il check:

- il ripristino della riga AMT cade 62.84-63.09 s dopo l'arm su diciotto
  segmenti, 67.6-67.8 su cold18 e cold42, 76.35 su cold34, e sempre **fra due
  giri** del watchdog, dopo uno, due o quattro poll del rivelatore. E' un
  evento dello stack (il bss-up dopo il CAC), non un giro;
- il measure block che `probe_schedule.py` leggeva come tick 60/63/70 sta
  subito **dietro** il ripristino, a MAC sospeso, prima delle calibrazioni: e'
  del bss-up, non del watchdog. Poi le calibrazioni, il latch della finestra e
  il blocco E;
- il poll del rivelatore e' un timer da 150 ms: 706 letture su cold05, 680 dei
  705 intervalli a 151 ms, gli altri a 1.3-1.4 s dove il measure block tiene il
  thread. Parte 11 ms dopo l'arm e non si ferma alla chiusura;
- la testa `0x7af` e il cambio di modo `0x520` stanno allo stesso istante e a
  1.004 s dal giro precedente: il giro del vendor e' sample phase poi corpo, un
  callback solo;
- il measure block cade sul decimo giro dopo lo switch e poi ogni dieci (42
  segmenti a freddo su 43); sugli up a caldo cade ovunque (7, 8, 6, 6, 9, 3 sui
  primi sei), e il dump della regione coincide sempre con uno dei suoi giri:
  sono due fasi di un contatore libero del driver, non del canale. La fase
  va letta mod 30: sull'up a caldo di ch52 il dump cade sullo stesso giro del
  measure block (marker 9), su cold05 venti giri dopo (marker 28) --
  `AC_WD_PHASE` viene dal primo dump, e dal measure block dove il segmento
  non ne ha, scegliendo il residuo che non ne mette uno dentro il segmento.

Il modello adesso: il driver ha `b43_phy_ac_watchdog()` (un callback al
secondo, contatori `wd_turns` dal bring-up e `wd_switch_turns` dallo switch),
`b43_phy_ac_radar_poll()` (il timer da 150 ms) e `b43_phy_ac_bss_up()`
(ripristino AMT, measure block, calibrazioni, latch e blocco E). Nessun tick
nel driver. L'harness replaya gli eventi dell'ambiente con `timeline.py`, che li
legge dai timestamp della cattura: `WD`, `POLL`, `TPL`, `BSS_UP`. L'unico stato
d'ingresso e' `AC_WD_PHASE`, la fase del contatore da dieci all'inizio del
segmento, come `probe_mode` e `last_cal_channel` lo sono gia'.

Su hardware manca il cablaggio: `b43_op_config` deve chiamare l'arm quando
`hw->conf.radar_enabled` si alza (mac80211 sintonizza prima del check e
`start_ap` sullo stesso canale non rifa' lo switch, quindi senza `bss_up()` le
calibrazioni rinviate non partirebbero mai), `bss_info_changed(BEACON_ENABLED)`
deve chiamare `bss_up()`, e il core deve dare al PHY un work da 1 s e un timer
da 150 ms; `b43_ac_cac_match_gate` e `b43_ac_beacon_reload` vanno definite nel
core. Oggi `cac_pending` nel kernel non e' mai assegnato.

Cosa costa: le ricariche del template in fase. Il vendor le mette dentro il
corpo del giro, fra le due passate della spazzata dei contatori -- wl carica il
template dal proprio watchdog -- e ora sono eventi fra un callback e l'altro,
perche' in b43 il template lo scrive `bss_info_changed` sotto il mutex e mai
dentro il watchdog del PHY. Ogni ricarica in fase costa 32-57 op di
spostamento nell'LCS; su cold05 sono 23. Il gate dovrebbe trattare il blocco
come "del core, mobile", categoria che `cmp_skip.py` non ha. Restano dentro la
coda del bring-up, come conteggio, le ricariche fra la host-flag clear e la
cella a 0xffff (`AC_BEACON_RELOADS`): stesso debito, stessa soluzione quando la
coda verra' spezzata. Cadono anche i giri `nolatch`: da derivare come regola sul
tempo dall'ultimo latch, ora che gli eventi hanno un istante. cold01 resta il
campione singolo di sempre, col primo giro collassato sul window read; con la
fase 9 il port gli mette un dump al giro 20 che il vendor non ha, 64 op.

Misure, tabella `--table` (metrica `full`) sui 43 segmenti a freddo: HEAD
min/mediana 96.45/97.88, ora 95.42/96.55 -- da -0.4 a -1.7 punti, tutti dalle
ricariche mobili e da cold01. Sugli up a caldo dei canali con la guardia il
segno e' l'opposto e non e' questo modello a farlo: la cattura dice che `wl up`
rifa' il CAC ogni volta (05-up-ch52: arm, 414 poll, bss-up a 60 s), mentre
l'harness metteva `cac_pending` a falso su ogni hot e faceva partire le
calibrazioni allo switch. Ora il default e' la guardia radar, freddo o caldo:
05-up-ch52 37.3% -> 86.0%, 10-up-ch104 36.4% -> 90.2%, 06-up-ch56 91.0%.
Quello che resta li' e' il debito dell'attach a caldo, non del CAC.
