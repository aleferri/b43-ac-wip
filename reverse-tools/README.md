# reverse-tools

Strumenti per il reverse engineering del driver Broadcom `wl` e per validare
il port b43 AC-PHY contro le catture `wl-diag`. Due famiglie: la **pipeline di
trace** (dal log grezzo al confronto col port) e gli **estrattori** one-shot di
tabelle statiche dal blob ELF.

## Pipeline di trace (dal log al confronto)

Ordine tipico: decodifica → fold RETVAL → collapse → (reorder) → confronto.

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
  correttezza del port (gate di regressione: `set_channel d6220` = 22268/22268).

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
