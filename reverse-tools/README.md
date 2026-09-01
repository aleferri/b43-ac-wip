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
- **merge_retvals.py** — ripiega le righe `RETVAL` nella op di lettura che le
  precede, così ogni `RD` porta il suo valore.
- **fold_mod_reads.py** — ripiega la read-implicita di ogni `MOD` (RMW).
- **collapse_trace.py** — rimuove il meccanismo raw delle table-op, lasciando le
  `TBL.*` compatte. Prerequisito di `reorder_trace` e `macro_order_map`.
  ATTENZIONE: per il confronto op-per-op col port usare il trace **grezzo**,
  non il collassato (vedi sotto).
- **reorder_trace.py** — riordina una traccia sull'ordine di un'altra, per
  allineare due catture.
- **compare.py** (in `test/`) — **il confronto canonico**: match posizionale per
  sequenza tra l'output dell'harness e la cattura vendor grezza. È la misura di
  correttezza del port (gate di regressione: `switch_channel d6220` = 22268/22268).

## Analisi

- **localize_functions.py** — localizza le funzioni del driver in una traccia.
  Modo nuovo (preferito): `localize_functions.py <harness_con_marcatori> <trace>`
  usa i confini esatti emessi da `B43_AC_FN()` (harness con `AC_FN_MARKERS=1`).
  Modo vecchio (fallback): un solo trace, fingerprint indovinati dai sorgenti.
- **coverage_by_function.py** — copertura per-sequenza di un flow (rfkill,
  op_init, …) contro la cattura vendor **grezza**, con i gap tra funzioni.
  Le funzioni che scrivono tabelle vanno confrontate per contenuto, non per
  sequenza (il vendor intercala le `TBL` in ordine diverso).
- **find_readback_hardcodes.py** — trova registri che il vendor legge ma il
  port hardcoda un literal invece di derivarli (dà `<vendor_trace> <harness>`).
- **macro_order_map.py** — mappa l'ordine macro delle fasi confrontando due
  trace collassati (`<harness.collapsed> <vendor.collapsed>`).
- **decorrelate_channel_bw.py** — decorrela le scritture deterministiche tra più
  catture canale/BW (utile per la generalizzazione BW).
- **diff_traces.py** / **verify_nvram_consumption.py** — coppia NVRAM:
  scoperta delle dipendenze NVRAM (differenziale tra board) e verifica mirata
  dei SROM input che il driver consuma.
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
  che `force_full_init` non da': nel modulo `zero_off` e' un solo campo,
  assegnato al solo hook `OP_CAL_INIT`, quindi azzera il byte della cal e non i
  flag 250 ("phy_init fatto") e 249 (POR). Procedura in `wl-diag/README.md`.
- **split_by_mark.py** — taglia una traccia sui record **MARK**, un file per
  etichetta. I MARK li inietta chi cattura (`echo "ch36 bw20" > /proc/wl_diag`)
  e `wl_diag` ai bordi di ogni caricamento del bersaglio, quindi il confine e'
  scritto nella traccia e i nomi vengono dalle etichette.
- **split_by_chanspec.py** — la controparte per gli sweep a caldo di
  `capture_plan.sh`, che non hanno MARK: taglia sui **salti temporali** (soglia
  1.03 s) perche' i record `CHANSPEC` sono in ritardo di un ciclo e ne arriva
  uno solo per fase, e prende i nomi dall'ordine della fase.
- **gen_syms.py** — costruisce la riga `syms=` per l'insmod di `wl-diag` da un
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
