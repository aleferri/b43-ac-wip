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
- **init_regs sul DSL — costanti di versione.** Lista di registri e ordine
  identici al d6220 (8× hi `0x033a/033b/033e/033f/0342/0343/0346/0347`, poi 8×
  lo `0x033c/033d/0340/0341/0344/0345/0348/0349`), ma i valori no: la wl 6.30
  del DSL scrive `(hi, lo) = (0x0395, 0x0315)` contro `(0x03bf, 0x0340)` della
  7.14 (`#122785` contro `#51731`), e non emette affatto `PHY 0x1645 = 0x025c`.
  Entrambe le board sono 4352, quindi è una differenza di **versione del blob**,
  non di chip: il dispatch `{4352, 4360, default}` su `chip_id` non la può
  esprimere. Il port scrive i valori 7.14, quindi su DSL `init_regs` misura
  0/17. Da decidere quale coppia vuole il silicio prima del prossimo run.
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
