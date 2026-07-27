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
  `set_channel` scrive sul 4360 (`agcombo #5810`, `#60373`): quello cade dopo
  `init_regs` e nessun testimone 4352 lo emette, quindi il gate
  `chip_id == 0x4360` in `set_channel` e' corretto.
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
- **Max index TX su 0x0646/0x0846 sul DSL.** Al primo bring-up tutte le board
  scrivono la costante `0x38`; su un channel setup successivo il valore e'
  `maxp5ga[grp] - 6`, cioe' `0x42` sul d6220 (maxp5ga0=72) e `0x44` su agcombo
  (74). Il DSL emette `0x38` anche sul down→up, quindi o non fa la derivazione
  o la fa da altri campi.
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

## Catture ancora utili (al prossimo riflash DSL)

Attach da freddo non catturabile (wl si deregistra). Restano: secondo canale
5G (terzo `lpf_cap0`), BW80, 2.4G (mappa radio 2G non validata), cicli down/up
(budget dei poll). Dettaglio in `dsl-capture-plan` (output di sessione).
