# Riferimento NVRAM / SROM rev 11 — significato e consumo lato PHY

Questo documento ha due scopi: (1) dare il significato di ogni variabile
NVRAM presente nei dump `router-data/*/*_nvram.txt`, e (2) registrare
**dove** ciascun valore finisce nel programmming PHY/radio, per quanto
stabilito confrontando sorgente driver e trace vendor.

La semantica dei campi è quella canonica dello SROM rev 11 Broadcom
(`bcmsrom_tbl.h`, citato in [`../sprom-rev11/cross_check.md`](../sprom-rev11/cross_check.md));
le correlazioni al programming sono ricavate in questa collezione dai
board con trace vendor (d6220, agcombo) — il dsl3580l non ha una trace di
registro e non contribuisce alla parte di consumo.

## Legenda confidenza

- **✓ verificato** — riprodotto dal driver e/o confermato op-per-op nella trace.
- **~ standard** — significato noto da `bcmsrom_tbl.h`; footprint non verificato qui.
- **⚠ SALAME** — ipotesi plausibile ma non confermata dai dati disponibili.
- **TODO** — da derivare o da verificare (encoding, unità, o formula ignota).

## Convenzione dei nomi (indicizzazione)

Quasi tutte le variabili sono varianti indicizzate di poche famiglie. Leggere
il suffisso è sufficiente per interpretarle:

- suffisso `a0` / `a1` / `a2` → core / catena (chain) 0, 1, 2;
- `2g` / `5g`, oppure `5gl` / `5gm` / `5gh` → banda (2.4 GHz / 5 GHz, o low/mid/high UNII);
- `bw20` / `bw40` / `bw80` / `bw160` → larghezza di canale;
- `po` → *power offset* (backoff), quasi sempre impaccato per-rate in nibble con segno;
- `ma0..2` in `pdoffset*` → *modulation antenna*, cioè per core.

Gli array multi-valore separati da virgola sono indicizzati per sub-band
(4 valori: le quattro sub-band 5 GHz) o per core.

## Come il driver consuma davvero la SROM

Riscontro di questa sessione, importante per leggere il resto: il driver
`phy_ac` legge **simbolicamente da `bus_sprom` solo quattro** ingressi —
`rxchain`, `subband5gver`, `core_pwr_info[].pa5ga`, `rxgains_5gl`. Tutto il
resto dell'NVRAM è, in questo porting: (a) hardcoded da cattura, (b)
consumato dal *core* b43 / MAC fuori dal path AC-PHY, oppure (c) parte di
blocchi di calibrazione che emettono op nella trace ma con valori catturati,
non derivati. Di conseguenza per la maggior parte delle variabili il
"consumo" documentato sotto è la destinazione osservata nella trace **vendor**,
non necessariamente un punto in cui il driver legge il campo NVRAM.

## Correlazioni verificate (sintesi)

| NVRAM | trasformazione | destinazione | confidenza |
|---|---|---|---|
| `pa5ga{c}[grp·3..]` | transfer function est_pwr (128 voci) | tbl `0x40`/`0x60`/`0x80` (core 0/1/2) | ✓ 128/128 su d6220 e agcombo |
| `rxgains_5gl.triso[c]` | `((triso+4)<<1)+2` | reg `0x06f9 + c·0x200`, bit 14:8 (val `0x1600`) | ✓ per core, entrambi i board |
| `rxgains_5gl.elnagain[c]` | `(elnagain+3)<<1` | tbl `0x44 + c·0x20`, offset 0 | ✓ |
| `maxp5ga{c}[grp]` | `maxp − 6` | reg `0x0646 + c·0x200`, max index (mask 0x00ff) | ✓ 3 punti / 2 board (offset −6: TODO) |
| `rxchain` | `& 0x07` → coremask | numero di blocchi per-core (tbl `0x80` solo se 3×3) | ✓ strutturale |
| `subband5gver` | confini 5250/5500/5700 → `pa5g_group` | selettore dello slice `pa5ga`/`maxp5ga` | ✓ (indiretto) |
| `tssifloor5g` | clamp per-chain | reg `0x0724 + c·0x200` (val `0x03ff`) | ⚠ SALAME (valore+struttura) |
| `mcsbw*po` | — (non derivata) | tbl `0x21` ppr, slot 1/5/6 = `0x0202` fisso | ⚠ origine `mcsbw205glpo` dichiarata nel sorgente ma **falsificata** dal differenziale |
| idle-TSSI (non NVRAM) | misura runtime | reg `0x0645 + c·0x200` (mask 0x03ff) | ✓ misurato, **non** derivabile da NVRAM |
| `tssiposslope5g` | segno dello slope TSSI (1 bit) | config detector TSSI (non isolabile) | ~ costante ovunque |

Dettagli e note di derivazione: est_pwr e max index in
[`txlpf-formula.md`](txlpf-formula.md) e nel corpo di
`b43_phy_ac_txpwrctrl_setup`; verificatori in
`../reverse-tools/verify_nvram_consumption.py` e `../reverse-tools/diff_traces.py`.

## Dettaglio per famiglia

### Identità board / host
- `sromrev` — formato SROM (11). ~
- `boardrev` — revisione board (es. `0x1355` P355, `0x1353` P353). ~
- `boardtype` — board-id (`0x668` DSL/D6220, `0x633` agcombo). ~
- `boardflags`, `boardflags2`, `boardflags3` — bitmask capacità/config board. ~ (singoli bit: confidenza minore)
- `subvid` — vendor sottosistema (`0x14e4` = Broadcom). ~
- `boardnum` — seriale board. ~
- `macaddr` — MAC. ~
- `devid` — PCI device-id (`0x43b3` / `0x43a2`). ~
- `ccode` + `regrev` — dominio regolatorio (vuoto = world, regrev 0). ~
- `ledbh10` — comportamento/mappatura LED 0-1. Scritte reali su reg `0x0182`, `0x0202..0x0204` ma lato **core b43**, non AC-PHY. ~
- `watchdog` — periodo watchdog in ms; `0x0bb8`→reg `0x0554/0x0555` (core, off-path). ~
- `xtalfreq` — frequenza quarzo (`0xffff` = default). ~

### Topologia antenne / catene
- `aa2g`, `aa5g` — bitmask antenne disponibili per banda. ~
- `txchain`, `rxchain` — bitmask catene attive; `rxchain & 7` → coremask (3=2×2, 7=3×3). ✓ per rxchain
- `antswitch` — config switch antenna. ~
- `agbg0..2` — guadagno antenna 2.4 GHz per antenna (quarti di dB). ~
- `aga0..2` — guadagno antenna 5 GHz per antenna (quarti di dB). ~

### Front-end / configurazione del path
- `femctrl` — tipo/pilotaggio front-end module; nel driver è assunto `==6` (tabella FEM applicata incondizionatamente), **non** letto da NVRAM. ~
- `epagain{2g,5g}` — guadagno PA esterno. ~
- `pdgain{2g,5g}` — guadagno power-detector. ~
- `papdcap{2g,5g}` — capacità pre-distorsione PAPD. ~
- `tssiposslope{2g,5g}` — **segno** dello slope TSSI (1 bit; `1` = TSSI cresce con la potenza). Costante ovunque → non isolabile per differenziale; lo slope *numerico* della potenza è invece `a1` in `pa5ga` (vedi est_pwr). ~
- `tworangetssi{2g,5g}` — TSSI a due range on/off (1 bit; `0` ovunque). ~
- `gainctrlsph` — modo gain-control. ~ (bassa confidenza)
- `paparambwver` — versione formato dei parametri PA bw-specifici. ~
- `subband5gver` — versione della suddivisione sub-band 5 GHz; fissa i confini 5250/5500/5700 usati da `pa5g_group()`. ✓ (indiretto)

### Coefficienti PA (polinomio pdet)
Ogni gruppo è una terna `(a1, b0, b1)` del modello pdet.
- `pa2ga0..2` — 2.4 GHz per core. ~
- `pa2gccka0` — variante CCK. ~
- `pa5ga0..2` — 5 GHz, 12 valori = 4 sub-band × 3 coeff per core. **Alimenta la transfer function est_pwr** (`a1/b0/b1 = pa5ga[grp·3 .. grp·3+2]`), scritta nelle tabelle `0x40/0x60/0x80`. ✓ 128/128
- `pa5gbw40a0`, `pa5gbw80a0`, `pa5gbw4080a{0,1}` — stessi coeff per canali 40/80 MHz; le trace bw80 usano tabelle est_pwr distinte (`0x42/0x62/0x82`). ~ (footprint bw non ancora portato)

### Potenza massima e offset per-rate
- `maxp2ga0..2` — potenza max 2.4 GHz per core (quarti di dBm; es. 66 = 16.5 dBm). ~
- `maxp5ga0..2` — potenza max 5 GHz per core, 4 valori sub-band. **Deriva il max index**: `reg 0x0646+c·0x200 = maxp5ga[grp] − 6`. ✓ (offset −6 su 3 punti; TODO conferma con un board a maxp diverso)
- `cckbw202gpo`, `cckbw20ul2gpo` — offset CCK. ~ (packing TODO)
- `mcsbw{20,40,80}2gpo`, `...5g{l,m,h}po` — offset per-MCS impaccati per banda/bw. Footprint: tabella ppr `0x21`. Il sorgente dichiara che `mcsbw205glpo` deriva lo slot `0x0202`, ma il differenziale d6220 vs agcombo mostra ppr **identico** con `mcsbw205glpo` diverso → la derivazione dichiarata è ⚠ falsificata; `0x0202` è un backoff per-rate fisso per il rate-set a ch36/5gl. ~ (packing per-rate TODO)
- `mcsbw160...po` — offset 80+80/160 MHz. ~
- `mcslr5g{l,m,h}po` — offset low-rate. ~
- `dot11agofdmhrbw202gpo`, `ofdmlrbw202gpo` — offset OFDM legacy (high/low rate). ~
- `sb20in40...po`, `sb20in80and160...po`, `sb40and80...po` (varianti `hr`/`lr`) — offset per sotto-canale dentro bw più larghe. ~
- `dot11agdup{hr,lr}po` — offset modo duplicato. ~
- `sar2g`, `sar5g` — limiti SAR. ~
- `txidxcap{2g,5g}` — cap dell'indice di potenza TX. ~

### Power-detector / TSSI / temperatura
- `pdoffset2g40ma0..2` + `pdoffset2g40mvalid`, `pdoffset40ma0..2`, `pdoffset80ma0..2`, `pdoffsetcckma0..2` — correzioni pd-offset per core/bw (impaccate). ~ (packing TODO)
- `tssifloor{2g,5g}` — floor/clamp del TSSI per sub-band (10 bit). Footprint candidato: reg `0x0724 + c·0x200` (ciclato `0`↔`0x03ff`, per-chain, scala coi core). ⚠ SALAME: `tssifloor5g` è `0x3ff` ovunque, quindi la destinazione è inferita da valore+struttura, non provata per differenziale; servirebbe un board con `tssifloor5g ≠ 0x3ff`.
- `measpower{,1,2}` — potenza di riferimento misurata (`0x7f` = non impostato). ~
- `tempthresh`, `tempoffset`, `rawtempsense`, `tempsense_slope`, `tempcorrx`, `tempsense_option` — sensore di temperatura (valori `0xff`/`0x1ff`/`0x3f` = default su questi board). ~
- `phycal_tempdelta` — delta-T che innesca una ri-calibrazione (`0xff` = disabilitato). ~
- `temps_period`, `temps_hysteresis` — cadenza / isteresi della misura di temperatura. ~

### RX gain / rumore
- `rxgains{2g,5g,5gm,5gh}{elnagain,triso,trelnabyp}a0..2` — per core/banda: `elnagain` indice guadagno eLNA, `triso` isolamento T/R, `trelnabyp` bypass eLNA in T/R. Impaccati come `(trelnabyp<<7)|(triso<<3)|elnagain`. Il driver usa **solo** `rxgains_5gl` (UNII-1); il vendor lo congela ad attach per tutte le bande. Trasformazioni verificate: `triso→((·+4)<<1)+2` (reg `0x6f9+c·0x200`), `elnagain→(·+3)<<1` (tbl `0x44+c·0x20`). ✓ per 5gl
- `rxgainerr{2g,5g}a0..2` — correzione errore di gain per core/sub-band. ~
- `noiselvl{2g,5g}a0..2` — livello di rumore per core/sub-band. ~
- `eu_edthresh{2g,5g}` — soglia energy-detect EU (regolatoria). ~

### Calibrazioni di path
- `rpcal2g`, `rpcal5gb0..3` — coefficienti di calibrazione del path RX per banda 5 GHz. ~
- `pcieingress_war` — workaround PCIe ingress (presente solo su dsl3580l). ~

### idle-TSSI (misura, non NVRAM)
Il *base index* idle-TSSI (`reg 0x0645 + c·0x200`, mask 0x03ff) **non** è una
variabile NVRAM: è una misura runtime del detector idle (readback di PHY
`0x013/0x012/0x464` + radio) e varia per core, per board e per iterazione
(deriva/jitter). Nessuna variabile `tssi*` lo determina: `tssifloor` è un
clamp costante, `tssiposslope` è solo un segno. La derivazione
`readback → index` resta il TODO principale del blocco txpwrctrl. ✓ (natura misurata)

## Punti aperti

- **Encoding esatto delle famiglie `*po` e `pdoffset*`**: unità e layout dei
  nibble (0.25 vs 0.5 dB, segno) da confermare su `bcmsrom_tbl.h` prima di
  scriverci codice di derivazione.
- **Offset `−6` del max index**: fit su 3 punti / 2 valori di `maxp`; confermare
  con un board a `maxp5ga` distinto.
- **Destinazione di `tssifloor5g`** (`0x0724`): inferita, non provata; serve un
  board con `tssifloor5g ≠ 0x3ff`.
- **`tssiposslope5g` / `tworangetssi5g`**: costanti su tutti i board; per vederne
  il footprint serve un board con `tssiposslope5g=0` o `tworangetssi5g=1`.
- **Derivazione base index idle-TSSI**: formula `readback → index` ignota.
- **Bit di `boardflags*` e `gainctrlsph`**: significato dei singoli bit a bassa
  confidenza.
