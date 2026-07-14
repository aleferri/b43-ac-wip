# router-data/agcombo — BCM4360 r2069 reference dumps

Terzo board nella collezione, e il primo non-BCM43b3: chip `0x14e4:0x43a2`,
`chipnum=0x4360`, `chiprev=0x3`, `corerev=0x2a`, AC-PHY rev 1, sromrev 11,
boardrev `P353` (display di `0x1353`), 3×3 (`txchain=rxchain=7`), dual-band
(`aa2g=7`, `aa5g=7`), full PA chain populato per entrambe le band. Driver
OEM `0x70e2b15` ≈ 7.14.43.21, ucode `0x3a004b1` ≈ 3.160.4.177.

## File

```
agcombo_revinfo.txt        wl -i wl1 revinfo
agcombo_srom.txt           wl -i wl1 srdump   (zero-only su questo blob;
                                               la SROM nominale è in nvram_dump)
agcombo_nvram.txt          wl -i wl1 nvram_dump
agcombo_phytable_5gl.txt   wl -i wl1 phytable 0x{44,45} {0..41} 8 +
                            width-disambiguation probe @0x44 offset 0
```

## Cosa stabilisce

**Dispatch chip-side a due famiglie reali.** Il path `b43_phy_ac_*` è ora
esercitato su BCM4360 (`0x4360`, agcombo) oltre a BCM4352-family
(`0x43b3`, DSL-3580L + D6220), entrambi sromrev 11 / r2069 rev 1 /
subband5gver 0x4. Il dispatch chip-aware del reverse tooling
(`reverse-output/by-chip/`) aveva già `{4352, 4360, default}` come
target — ora ognuno ha un dump di campo associato.

**Triplet rxgain default radio-side.** I tre chain leggono triplet
identico per tutte e tre le sub-band 5g (5gl `(3,6,1)`, 5gm `(7,15,1)`,
5gh `(7,15,1)`), confermato sul terzo chain *fisico* dell'agcombo. Lo
stesso vale per il register `phyreg 0x{6,8,a}f9` bits 14:8 = `0x16`
identico sui tre chain. La triplet è una proprietà del radio rev 1 r2069,
non una calibrazione board-specific.

**Populator OEM 7.14 single-shot.** Su 7.14.43 sia i tre register
rxgains sia le table 0x44/0x45 sono attach-time-frozen: phyreg `0x6f9`
ritorna `0x1602` identico fra `5g36/20` e `5g100/20`, phytable `0x44 32 8`
ritorna `0x07` identico fra le stesse due chanspec. Il porting `b43`
chiama `b43_phy_ac_rxgain_init` solo a `op_init`, design coerente col
firmware OEM moderno.

**Encoding rxgains SROM-side, half triso.** Phyreg `0x16` con formula
`(triso+4)<<1 + 2` (correzione `+2` 7.14, già osservata sul D6220 e
qui presente in 7.14.43 quindi proprietà del ramo 7.14 nel suo insieme)
implica `triso=6` runtime. L'unico bit-field-ordering naturale che
mappa il byte SROM `0xb3` → triso=6 è `(b<<7)|(t<<3)|e`. Le altre
half (elnagain, trelnabyp) restano da derivare; per il porting MVP sono
bypassate dal dump phytable statico.

**Format `wl phytable` su questo blob.** Width=8 byte-stride,
LSB del print (32 bit) è id-echo da scartare, byte high-order è il
dato. Width=16 e width=32 sono reinterpretazione LE multi-byte dello
stesso buffer. Coerente sui 37 byte campionati, niente maschere
chip-specific.

**Chanspec 5g100/20 accettato.** `wl -i wl1 chanspec 5g100/20` ritorna
`Chanspec set to 0xd064` con `ccode=""` e `regrev=0`. Sul DSL-3580L
sotto firmware OEM 6.30, lo stesso comando con `-i wl1` accetta almeno
UNII-1 a 20 e 80 MHz (`5g36/20`, `5g36/80`, `5g40/80` tutti `Chanspec
set` puliti); la sessione precedente di `Bad Channel` sul DSL era
artefatto di `wl chanspec` senza `-i wl1`, che agisce sul core 2.4 GHz
`wl0` (band-locked). UNII-2/2e/3 sul DSL non è stato testato con la
forma corretta — ma per il bring-up MVP non interessa.
