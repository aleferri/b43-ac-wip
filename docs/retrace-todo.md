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

La localizzazione per-funzione nelle catture non e' piu' annotata nei sorgenti:
era una tabella generata, incolonnata a mano nei commenti e mezza vuota
("n/l" su un lato). Si rigenera con `reverse-tools/localize_functions.py`, che
la ricava dalle fingerprint del sorgente corrente invece di fidarsi di un
commento che nessuno riallinea.

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

Oggi sono tre: i coefficienti TX IQ/LO scritti da tabella invece che calcolati,
il residuo di 1 LSB sul coefficiente `b` della RX IQ, e il guard di canale
scavalcato da `CONFIG_B43_PHY_AC_ANY_CHANNEL`, che elenca quali tabelle sono
fittate su ch36 a 20 MHz e non hanno prove altrove.

## Punti aperti

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

  Da guardare: il poll con 57 letture, tre in piu' della forma piena. Tre e' la
  lunghezza di una lettura `hi/lo/hi`, quindi e' un contatore in piu' letto una
  volta -- quale, non lo so.

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
  | cold01 ch36 bw20 | 91.05% | 428 | 1673 | 93 |
  | cold05 ch52 bw20 | 62.35% | 115 | 2668 | **4990** |
  | cold09 ch100 bw20 | 54.76% | 74 | 3749 | **6053** |

  Su cold01 le op di troppo sono 93, non 521: le altre 428 sono valori
  sbagliati, e 257 di quelli sono il payload TX IQ/LO, il debito noto. Sulla
  forma corta invece le op di troppo sono reali e sono la voce dominante -- il
  port esegue fasi di calibrazione che il vendor la' non esegue.

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

- **Il blocco di config MAC dopo l'init radio: aperto per un pezzo.** Il muro
  del posizionale e' lo stesso su tutti e 26 i segmenti -- il percorso PHY e'
  concluso su ogni canale e larghezza -- e apre con quattro accessi piu' due
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
  `b43_phy_ac_may_calibrate_tx()`, `center_freq <= 5250`, e ci sono dietro le
  tre fasi di cui l'assenza e' **provata su tutti e 26 i segmenti**:

  | fase | testimone | ch <= 48 | ch >= 52 |
  | --- | --- | --- | --- |
  | `rxcal_afe_calibrate` + `finalize_gain_luts` | PHY `0x0380` | 313-978 | **0** |
  | `rxiqcal_run_meas_iters` | PHY `0x0380` | idem | **0** |
  | `loopback_gain_search` | PHY `0x0b22` | 9 | **0** |
  | `iqcal_coeff_tables_reset` | tab. `0x42`/`0x62`/`0x82` | 256 ciascuna | **0** |
  | `post_rxiqcal_stage2` | tab. `0x000e` | 8 | **0** |

  Verificate su tutti e 26 i segmenti, non su un campione. Il testimone di una
  fase e' un registro o una tabella che nel port solo quella funzione tocca --
  il metodo trova le fasi che ne hanno uno, e non quelle che condividono tutto
  con altre.

  Due che NON vanno gatate, controllate e scartate: `idle_tssi_meas` (198
  campionamenti a ch36 contro 192 a ch52: gira su entrambe) e
  `rxiqcal_finalize` (19 e 19).

  Una da guardare: `iqcal_meas_post_dds_apply_v2`, il cui testimone PHY
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

  **Da fare: dove cade il confine dentro il resto del blocco.** Dietro il
  predicato ci sono solo le fasi provate assenti; le altre di
  `set_channel_calibrations()` girano su entrambi i lati per quanto e' stato
  controllato, ma il metodo usato -- cercare un registro emesso da una sola
  funzione -- trova solo le fasi che ne hanno uno. Una fase saltata che
  condivide tutti i suoi registri con altre non si vede cosi'. Restano ~1100 op
  di scarto fra port e vendor a ch52, e sono probabilmente la'.

  Nota che nessuna di queste tre muove il punteggio o il posizionale: stanno
  oltre il muro a `~@9600` e l'LCS non penalizza le inserzioni. Togliere op che
  non ci dovevano essere e' giusto per il driver, non per il numero.

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
  17 delle 18 scritture a quel registro; la diciottesima viene da un altro
  sito e resta da trovare.

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

- **Le due forme di attach: la seconda `0x2e4`.** I 26 segmenti si dividono in
  due forme, 7 lunghe (~36k op) e 19 corte (~20k). Il bloccante del posizionale
  su tutte e 19 era la stessa op, `PHY.MOD 0x02e4 val=0x0f00 mask=0x3f00`:
  nella forma corta il vendor la scrive **tre** volte e nella lunga una, e il
  port ne emetteva una.

  Aggiunta in `b43_phy_ac_frontend_gpio_setup()`, fra la terza
  `b43_maccontrol_set(0x04000400)` e `b43_phy_ac_pmu_req(dev, false)`, gatata
  su `center_freq > 5250`. Il posizionale dei 19 passa da `@54` a **~@9600**.

  Perche' la condizione e' sul canale: lo sweep e' **a freddo**, `rmmod` piu'
  `insmod` fra un ciclo e l'altro, quindi non esiste stato riportato dal ciclo
  precedente e nessuna regola del tipo "il primo canale di questa larghezza"
  puo' essere quella vera. Resta una proprieta' del canale, e i dati la danno
  netta: una scrittura sui canali 36-48, tre da 52 in su.

  Cosa NON e' stabilito: quale proprieta' del canale il driver stock testi
  davvero. 5250 MHz e' dove i domini regolatori mettono il confine DFS, e le
  tracce non mostrano lavoro specifico per il DFS da nessuna delle due parti --
  `B43_SHM_SH_RADAR` non e' toccata da nessun segmento e il poll radar c'e' in
  tutti -- quindi "sopra 5250" e "richiede radar detection" sono lo stesso
  insieme qui e non si distinguono. E cosa significhi quel campo non e' noto.

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
  `switch_analog()` e di `b43_software_rfkill()` -- cioe' prima di dove il PHY
  scrive il chanspec -- e che `b43_op_config()` ripunta a ogni cambio di
  canale. `b43_is_40mhz()` legge da la'. E' gia' impostata, e non c'e' nulla da
  scrivere.

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

  Nell'harness il doppione e' `emit_core_amt()` in `test/main.c`, che emette
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
  `emit_core_shm_macaddr()` in `test/main.c`, col MAC nel profilo di board. La
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
  `emit_core_shm_chipinit()` e `emit_core_hostflags()` in `test/main.c`, con
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

  Uno risolto: `b43_phy_ac_frontend_gpio_setup()` chiudeva con un
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

- **[PARZIALE] indice del ladder crs_min_pwr.** La catena e' chiusa e
  implementata (ladder + ancoraggio per-BW {34,33,30} + clamp[0,14] + bump
  a freddo, verificata sul blob D6220 7.14): vedi `crsminpwr-d6220.md` e
  `b43_phy_ac_op_recalc_txpower`. Resta aperto SOLO il campione
  d'interferenza per freq_range che seleziona la soglia (l'indice concreto
  del canale), non misurabile senza hardware -- `crs_index_for_chan` e' un
  placeholder. Su hardware: verificare se la misura passa da un accessor di
  I/O non coperto, o via iovar `phy_force_crsmin` sul driver stock.

## Doppia programmazione dell'analogico durante l'attach

Osservata sull'agcombo e assente sul d6220, **ma il discriminante non e'
stabilito**. Il gate in `b43_phy_ac_op_switch_analog` e' su `chip_id` perche' e'
l'unica variabile che distingue i due testimoni, non perche' sia dimostrato.

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
pericoloso". Sono greppabili, e sono tre: soglie `crs_min_pwr`, default LUT di
gain del core 2, e **indice massimo di potenza TX** -- quest'ultimo il piu'
pericoloso del file, perche' un indice troppo alto sovrapilota il PA.

Due sono usciti dalla lista, e per ragioni diverse. Il blocco `0x60`/`0x64`
ora scrive `afe_res[]`/`afe_res_cal[]`, cioe' i risultati riletti dalla cal AFE:
il valore e' **derivato**, il marcatore va via. L'ampiezza del tono no: resta un
letterale, ma non e' impalcatura ch36 -- e' misurata invariante su 16 canali e
tre larghezze (vedi sopra), quindi il marcatore diceva una cosa falsa sul suo
raggio di validita'. Un `SCAFFOLD` si toglie derivando il valore **oppure**
dimostrando che l'invarianza copre il dominio; questo e' il secondo caso.

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
  `0x0154`". Falso, e l'errore era gia' smentito dai dati in repo. Sui 52
  segmenti dello sweep d6220 la sequenza delle scritture su `0x0736` e'
  **identica in tutte e 26 le configurazioni** (16 canali x BW20/40/80):

      0x0154, 0, 0x0152, 0, 0x022a, 0, 0, 0x0154, 0

  I tre valori distinguono i **siti di chiamata**, non i canali: `0x0154` in
  `rx_gain_regs_program`, `0x0152` nel banco `b2j_ops`, `0x022a` in
  `rxgain_perchan_config`. Il port aveva le tre costanti giuste per sito da
  subito; l'errore era solo nella descrizione. Chi ha scritto quella riga ha
  confrontato due siti diversi di due catture diverse e ha attribuito la
  differenza al canale.

  Morale, perche' ricorre: prima di dichiarare una dipendenza da canale o
  larghezza, si guarda la classe della chiave in
  `full-sweep.zip/decorrelazione-52-segmenti.csv`. Per `0x0736` dice
  `dinamico`, non `solo-canale` -- e "dinamico" qui significa solo che il
  numero di scritture varia fra i due cicli, non il valore.

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
## Fuori perimetro: la regione probe-response della shared memory

Ogni segmento dello sweep d6220 contiene sette scritture in SHM che il port non
emette, con valori costanti su tutti i 52 segmenti e tutte e tre le larghezze:

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
