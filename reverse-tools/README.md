# reverse-tools

Strumenti per il reverse engineering del driver Broadcom `wl` e per validare
il port b43 AC-PHY contro le catture `wl-diag`. Due famiglie: la **pipeline di
trace** (dal log grezzo al confronto col port) e gli **estrattori** one-shot di
tabelle statiche dal blob ELF.

## Pipeline di trace (dal log al confronto)

Ordine tipico: decodifica → fold RETVAL → collapse → (reorder) → confronto.

- **csanity.py** — controllo su file C senza compilatore: `*/` dentro la prosa di
  un commento multiriga, commenti o letterali non chiusi, parentesi non
  bilanciate, e **dichiarazione dopo statement** (i kernel target sono C90, dove
  e' un warning che con `-Werror` diventa errore). Da passare sui `wl_diag.c`
  prima di build su device: ognuno di questi controlli nasce da un errore che ha
  bruciato un ciclo di compilazione sul router. Il controllo C90 va usato **solo
  sui `wl_diag.c`**: il port b43 in `src/` va verso mainline moderno, dove
  dichiarare dopo uno statement e' normale, e la' segnala oltre cento casi
  legittimi. Controlla anche l'**uso di un simbolo `static` di file prima della
  sua dichiarazione**, che in C non compila e che e' facile introdurre spostando
  un blocco durante un refactor; quel controllo non ha falsi positivi sul repo e
  si puo' passare su tutto.
- **decode-wl-diag.py** — decodifica i record binari (28 B BE) emessi dal
  modulo `wl-diag` in righe testuali (`PHY.WR addr=.. val=..`, ecc.).
- **trace_filter.py** — toglie da una traccia gli artefatti della
  strumentazione: tre filtri componibili, applicati in un ordine fisso perche'
  e' l'unico che funziona (prima i valori, poi le op che li portano).
  - `--retvals` ripiega le righe `RETVAL` e `ARGX` nella op che le precede,
    cosi' ogni `RD` porta il suo valore. **Senza questo ogni lettura ha
    `val=UNDEFINED`**, e dimenticarlo e' l'errore piu' facile da fare: una
    `grep 'val=0x...'` su un file non ripiegato non trova nessuna lettura e
    sembra che l'op non ci sia.
  - `--mod-reads` toglie la lettura interna di ogni `MOD` (la RMW che
    `phy_reg_mod` fa da se'), che e' puro artefatto e in un confronto
    posizionale e' un falso mismatch. Non nasconde ne' il meccanismo delle
    tabelle ne' i valori.
  - `--collapse` toglie le op sulle porte delle tabelle (0x00d id, 0x00e
    offset, 0x00f/0x010/0x011 dati, 0x019e gate) lasciando la sola
    intestazione `TBL`. Prerequisito di `reorder_trace` e `macro_order_map`.
    ATTENZIONE: nasconde anche i valori, quindi una divergenza vera sparisce
    con loro. Per il confronto op-per-op col port si usa la traccia
    **grezza**, non la collassata.
- **reorder_trace.py** — riordina una traccia sull'ordine di un'altra, per
  allineare due catture.
- **compare.py** (in `test/`) — **il confronto canonico**: match posizionale per
  sequenza tra l'output dell'harness e la cattura vendor grezza. Non si lancia a
  mano: `test/gates.sh` ricava la finestra e la schedule dei tick dal segmento e
  chiama lui e `cmp_skip.py`. Il numero corrente sta in `README.md`, che e'
  l'unico posto dove va aggiornato.

## Analisi

- **tracelib.py** — non e' un tool, e' la libreria che gli altri importano:
  parsing delle righe vendor e port, normalizzazione delle op, attribuzione di
  ogni op alla funzione piu' interna tramite la pila dei marcatori, calcolo
  degli span, scoperta delle catture (zip compresi, con la sintassi
  `archivio.zip!interno.txt`). Normalizzazione e attribuzione stanno qui e non
  nei chiamanti perche' sono la *definizione* di "la stessa op" e "questa op e'
  di questa funzione": due tool che le implementano in modo diverso danno due
  risposte diverse alla stessa domanda.
- **fn_map.py** — dove cade ogni funzione del port dentro una cattura, in tre
  modi di forza crescente. `coverage` conta le op ritrovate per funzione e
  mostra i **gap**, cioe' le op vendor che nessuna funzione copre; `span` da'
  l'intervallo di episodi di cui ogni funzione rende conto, allineando le due
  sequenze per intero, e vale solo se il port riproduce quella cattura;
  `fingerprints` ricava le sequenze dai letterali nei sorgenti invece che dai
  marcatori, e serve quando l'harness non puo' riprodurre la cattura (altra
  board, altra build del blob). La cattura va passata **grezza**, non
  collassata. Le funzioni che scrivono tabelle vanno confrontate per contenuto,
  non per sequenza (il vendor intercala le `TBL` in ordine diverso) e qui
  appaiono con una copertura bassa che non e' un difetto del port.
  Il `--gap` di `coverage` decide cosa significa il risultato e sbaglia in due
  versi: troppo stretto non chiude una fase intercalata, troppo largo raccoglie
  op sparse per tutta la cattura e il punteggio sale fino a non voler dire
  niente. Il conteggio va letto insieme all'intervallo che lo accompagna.
- **anchors.py** — i riferimenti `#NNNN` nei commenti: `resolve` dice quali
  catture contengono quell'indice, `class` se l'op a quell'indice e' della
  classe che il codice emette li' (un ancoraggio confermato dalla classe non e'
  necessariamente giusto, ma uno smentito e' sbagliato di sicuro), `span` se
  l'indice cade nell'intervallo che la funzione copre davvero -- ed e' l'unico
  dei tre che identifica *quale* cattura, cioe' l'informazione che
  all'ancoraggio manca. Esce con stato 1 quando qualcosa non si risolve, quindi
  e' usabile come gate prima di un commit.
- **phase_absent.py** — quali fasi il vendor non esegue oltre una soglia, che e'
  cio' che decide cosa gatare. Confronta quante volte il vendor tocca gli
  indirizzi di ogni fase sotto e sopra la soglia. Il default guarda **tutti**
  gli indirizzi propri della fase; `--witness-only` ne guarda uno solo, quello
  esclusivo, e risponde a una domanda piu' debole: i tre registri esclusivi di
  `idle_tssi_meas` sono a zero sopra i 5250 MHz mentre la fase gira, quindi un
  testimone esclusivo a zero prova assente il **registro**, non la fase.
- **find_readback_hardcodes.py** — trova registri che il vendor legge ma il
  port hardcoda un literal invece di derivarli (dà `<vendor_trace> <harness>`).
- **macro_order_map.py** — mappa l'ordine macro delle fasi confrontando due
  trace collassati (`<harness.collapsed> <vendor.collapsed>`).
- **decorrelate_channels.py** — decorrela le scritture rispetto a canale e
  larghezza su N segmenti, classificando ogni chiave in invariante,
  solo-canale, solo-larghezza o dipendente da entrambi.
- **srom.py** — quali valori che il driver scrive sono dati di board e non
  codice, con tre metodi. `literals` cerca le costanti del driver che
  coincidono con un valore dichiarato dalla SROM (con `--canonical`, il
  `bcmsrom_tbl.h` di Broadcom decide se il campo esiste davvero o se il match
  era fortuna). `correlate` confronta due catture e legge cosa *cambia*, che
  e' l'unico modo utile visto che ogni valore SROM viene trasformato prima di
  essere scritto. `verify` prende i quattro input che il port legge davvero da
  `bus_sprom`, applica la trasformazione del consumatore e cerca il risultato
  nella finestra della funzione che lo consuma. Le catture si passano come
  argomenti.
- **reads.py** — le letture che il port scarta, nei tre stadi della stessa
  indagine. `risk` dice quali hanno un valore che **varia** fra le catture: una
  costante e' al massimo fragile, una che varia e' un bug in attesa di un'altra
  board, perche' il port non sta imparando niente e ogni scrittura che ne
  dipende e' trascritta invece che derivata. `intent` dice cosa il driver stock
  ne **fa** -- ciclo di campionamento, poll, read-modify-write, read-to-clear,
  probe -- perche' "letta per l'ordine sul bus" non e' una spiegazione, e' cio'
  che resta quando nessuno l'ha ricavata. `plan` emette il piano di lettura in
  C per un registro, che serve ai flow che girano senza `AC_READ_ORACLE`.
- **sweep_report.py** — un rapporto su uno sweep, in due modi. `score` da' una
  riga per canale ed e' il segnale grosso: dice quali canali si sono mossi.
  `regdiff` dice quali registri il port scrive in modo diverso, aggregato su
  tutti i canali, ed e' quello che dice cosa e' sbagliato davvero oggi.
  ATTENZIONE: il denominatore di `score` sono le sole op del vendor, mentre
  `test/cmp_skip.py` usa l'unione dei due flussi e penalizza le op in piu'.
  **I due numeri non sono confrontabili** e quello citabile e' di `cmp_skip`.
- **annotate_enables.py** / **dataflow.py** — utility di debug (stato enable
  riga-per-riga; data-flow attraverso le table read).

## Lato device: hook del tracer

- **capture_plan.sh** — sweep **a caldo** sul device: un solo `insmod` e N cicli
  `{chanspec; up; attesa; down}`, ognuno dei quali e' un down->up. Si confronta
  col gate `switch_channel` e `AC_FIRST_INIT=0`, non col flow `full`.
- **cold_capture.sh** — un ciclo **a freddo** per canale: `rmmod wl` +
  `insmod wl` a ogni giro, senza toccare `wl_diag`, che si arma da se' al
  `MODULE_STATE_COMING` del bersaglio -- nel 3.4 prima di `mod->init`, quindi
  l'attach cade sotto gli hook senza remove/rescan PCI. Solo kernel 3.4 (sul
  2.6.30 l'ordine delle notifiche non e' stato verificato). Il primo `up` dopo
  il ricarico e' un `phy_init` con `do_full_init` e una `cal_init` mai fatta,
  che nessuna manomissione della struct del PHY riprodurrebbe: azzerare il byte
  della cal lascerebbe come sono i flag 250 ("phy_init fatto") e 249 (POR).
  Procedura in `wl-diag/README.md`.
- **split_trace.py** — taglia una traccia in un segmento per configurazione.
  Il criterio del confine si sceglie con `--on`, e quale sia quello giusto
  dipende da come la cattura e' stata presa:
  - `--on mark` sui record **MARK**, che li inietta chi cattura (`echo "ch36
    bw20" > /proc/wl_diag`) e `wl_diag` ai bordi di ogni caricamento del
    bersaglio: il confine e' scritto nella traccia e i nomi vengono dalle
    etichette. E' il criterio dello sweep a freddo di `cold_capture.sh`.
  - `--on chanspec` sulla scrittura del chanspec in shared memory. E' il
    criterio con cui sono stati prodotti i segmenti dentro `hot-sweep.zip`, e
    l'unico che li riproduce. Attenzione a cosa comporta: dentro un ciclo il
    chanspec cade dopo la testa del ciclo, quindi quella testa finisce in coda
    al segmento precedente.
  - `--on gaps` sui **salti temporali** (soglia 1.03 s), per una traccia che
    non porta ne' MARK ne' chanspec. Taglia piu' avanti di `chanspec` e tiene
    la testa del ciclo col ciclo, che e' il verso giusto, ma non riproduce lo
    split pubblicato negli archivi. Il gap da solo non basta e per questo il
    criterio non e' solo una soglia: mentre l'interfaccia e' su, un
    campionamento periodico lascia buchi di ~1.00 s indistinguibili per durata,
    e li si riconosce perche' hanno la **stessa** op ai due lati.

  I nomi dei file prodotti (`00-init-parziale.txt`, `00-scartati.txt`) sono
  quelli pubblicati dentro gli archivi: sono dati, non codice.
- **audit_hooks.py** — dato un `wl.ko` e `wl-diag/wl_diag.c`, dice per ogni hook
  se il simbolo **esiste** in quella build, se il prologo e' **agganciabile** col
  detour a 4 parole (o a 2 con `.shortj`) e quanti **siti di chiamata indiretta**
  ci sono. Risponde su un blob prima di bruciare una corsa di cattura: su agcombo
  (7.14.43) ha mostrato che `wlc_bmac_read/write_objmem16` non esistono affatto,
  mentre sul d6220 (7.14.89) ci sono. NB: `klookup=` esiste nella variante
  **2.6.30**, non in quella 3.4, che risolve da se' via `kallsyms`: la riga di
  `gen_syms.py` va all'insmod del `wl-diag-2630`.
- **gen_syms.py** — costruisce la riga `insmod` di `wl-diag` (con `klookup=`) da un
  `/proc/kallsyms` copiato dal device. La lista degli accessor vendor tracciati
  è in `wl-diag/wl_diag.c` (`hooks[]`): `phy_reg_*`, `write/read/mod_radio_reg`,
  `si_pmu_*`, `si_corereg`, `si_gpio*`, `wlc_phy_table_*_acphy`, `wlc_bmac_*`,
  `osl_delay`. Se il blob usa accessor fuori da questa lista (batch-writer,
  varianti radio-specifiche), le loro op non vengono catturate: verificare col
  kallsyms al prossimo riflash.

## Estrattori di tabelle (one-shot, dal blob ELF)

Leggono l'ELF via `pyelftools` (`pip install -r requirements.txt`).

- **extract_acphy_tables_from_descriptor.py** — segue `acphytbl_info_rev{0,2}`
  e dumpa le 25 tabelle init come array C (ha generato `tables_phy_ac.c`).
- **extract_acphy_txgain.py** — le tabelle `acphy_txgain_*` orfane (band-specific).
- **extract_chan_tuning_2069rev4.py** — `chan_tuning_2069rev4` (DSL, radio rev4).
- **cmp_funcs.py** — confronta i corpi delle funzioni omonime di due o piu'
  build di `wl`, a due livelli: identiche byte per byte, e identiche a meno
  delle **costanti**. Il secondo livello e' il punto: in un file REL l'addendo
  delle rilocazioni sta dentro l'istruzione, quindi senza azzerare l'immediato
  a 16 bit delle I-type e il target a 26 delle J-type ogni differenza di layout
  conta come differenza di codice. Con due build da' le funzioni divergenti una
  per una; con piu' di due, la matrice a coppie con anche la sovrapposizione
  dei nomi (jaccard), che e' quello che dice se due build sono lo stesso driver
  prima che valga la pena confrontare i corpi. Confronta solo le funzioni
  presenti in entrambe **e di pari dimensione**: dimensione diversa vuol dire
  codice diverso e non c'e' niente da confrontare.
- **ppr_invert.py** — inverte la catena PPR su un segmento, sotto due modelli
  del massimo di riferimento, che vanno lanciati entrambi perche' quale valga
  e' cio' che i dati devono decidere. `--model capped` prende `M = max(cp)`, il
  massimo **dopo** il taglio: allora M non e' un'incognita libera e si enumera
  il solo tetto, tenendo ogni valore che riproduce gli otto campi. `--model
  srom` prende `M` dalla SROM, non tagliato: allora `cp` segue
  dall'osservazione e il tetto segue da `cp`, in forma chiusa e senza
  enumerazione. Nessuno dei due e' un raffinamento dell'altro.
