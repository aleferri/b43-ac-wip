# router-data/d6220 — Netgear D6220 wl1 dumps

Secondo board family-BCM43b3 acquisito accanto al DSL-3580L. Stesso chip target
del repo (`0x14e4:0x43b3`, BCM4352-family AC-PHY rev 1), boardid `0x668`,
sromrev 11. Board rev `P355` vs `P353` del DSL — due rev consecutive del
Broadcom reference design BCM43b3, OEM diversi (D-Link → Netgear).

## Provenance

Sessione diretta sul Netgear D6220 (kernel OEM Linux 3.4, firmware Netgear
con driver `wl` rev `0x70e590e` ≈ 7.14.89.14, ucode `0x3a02715`).
Acquisizione via shell del firmware OEM:

```
wl -i wl1 revinfo                 -> wl1_revinfo.txt
wl -i wl1 srdump                  -> wl1_srom_raw.txt
wl -i wl1 dump nvram              -> wl1_nvram.txt
wl -i wl1 chanspec 5g{36,100}/20  + wl -i wl1 phyreg 0x{6,8,a}f9
                                  -> wl1_phyreg_rxgain.txt
```

Le letture phyreg sono prese **dopo** `wl up` + associazione a un AP 5 GHz
(tethering telefono), così il populator OEM ha avuto modo di girare prima
del campionamento.

## Cosa il D6220 risolve / non risolve / lascia ambiguo

### Risolve

**Conferma stesse antenne / chain / subband del DSL.** PCI ID,
chipnum, chiprev, corerev, radiorev, phytype, phyrev, sromrev,
subband5gver, txchain/rxchain, femctrl, boardid: tutti identici al
DSL-3580L (vedi `../dsl3580l/wl1_*.txt`). Il path
`b43`/`bcma`/`ssb` patchato deve riconoscere e gestire entrambi senza
distinzione device-tree-side.

**Chiude la SALAME formula populator register-side.** Il delta `+2`
osservato a phyreg bits 14:8 (`0x16` invece dell'atteso `0x14` del 6.30)
è una differenza di comportamento del populator fra i due blob: il 7.14
applica una correzione `+2` che il 6.30 non ha.

### Non risolve

**Disambiguazione encoding rxgains SROM-side.** Triplet rxgain identici al
DSL-3580L (5gl `(3,6,1)`, 5gm/5gh `(7,15,1)`, 2g `(0,0,0)`) → byte SROM a
`srom[112-113]` chain 0 identici (`0xffff 0xb300`). Open question §"Encoding
rxgains nei due word del chain block" del README master resta aperta. Per
chiuderla serve un terzo board della famiglia 4352 con triplet diversi —
preferibilmente con `triso` non saturo né zero su almeno una sub-band.

### Lascia ambiguo

**Origine del `+2` 7.x.** <span style="color:red">**SALAME**</span> — il
`+2` osservato non è verificato come identità algebrica col `(triso+4)<<1`
della 6.30: regge sul singolo data point osservato (`0x16 - 2 = 0x14 =
(6+4)<<1` per `triso=6` su 5gl) ma non è formalmente confermato. Per
chiuderlo black-box serve campionare più valori di `triso` — 5gm/5gh danno
`triso=15` — forzando un re-populate attach-time per sub-band (`wl down`/
`wl up` + associazione su un AP della banda) e rileggendo `phyreg
0x{6,8,a}f9`.

### Aggiunge gratis

**Secondo dataset PA / power tables**, divergente dal DSL. Tutti i
`pa5ga0/1/2[12]` differiscono word-per-word, e `maxp5ga0/1` è asimmetrico
per sub-band sul D6220 (`72,70,86,0`) contro la simmetria del DSL
(`76,76,76,76`). La `maxp5ga0[3]=0` introduce un edge case — quarta
sub-band power-capped a zero — che il driver `b43` deve gestire come
"canale non disponibile su quel chain", non come "trasmetti a 0 dBm".
Test set utile per il post-MVP §"TX power control reale".

**Secondo banco MVP.** Lo stesso `b43_chantab_r2069[]` per UNII-1 36..48
funzionerà identicamente sui due board. Se solo uno dei due funziona dopo
il porting, è bug nel singolo banco, non nel driver.

## Diff sintetico SROM raw vs DSL-3580L

Identici word-per-word su `srom[8..15]` (chip identity), `srom[48]`
(`0x43b3`), `srom[64].lo` (boardnum `0x0634`), `srom[80..103]` (PA/ag),
`srom[104..111]` (`pa2gccka0` + noiselvl), `srom[112-113]` (rxgains chain
0), `srom[132-133]` (rxgains chain 1), `srom[152-153]` (rxgains chain 2),
`srom[168..175]`.

Differiscono `srom[64].hi` (boardrev), `srom[114-127]` (maxp5ga0 +
pa5ga0), `srom[134-147]` (chain 1), `srom[154-167]` (chain 2),
`srom[176..199]` (mcsbw*po + sb*po power offsets), CRC finale.

Il DSL espone `srom[72-74]` non zero (Netgear li azzera); il D6220 espone
`srom[18-39]` non zero (DSL li azzera). Driver OEM diversi loggano blocchi
SROM diversi — il dato fisico sulla SROM è probabilmente sovrapponibile,
ognuno dei due dump è parziale.
