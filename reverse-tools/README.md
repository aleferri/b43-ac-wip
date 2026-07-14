# reverse-tools

Script Python per estrarre **tabelle statiche** dal driver Broadcom `wl`
MIPS BE. Ogni tool cammina un simbolo/descriptor nell'ELF e ne dumpa il
contenuto verbatim come array C.

## Prerequisiti

- `python3` >= 3.8
- `pyelftools` (`python3 -m venv .venv && .venv/bin/pip install -r reverse-tools/requirements.txt`)

i tool leggono l'ELF via `pyelftools`.

## Tool

### extract_acphy_tables_from_descriptor.py
Estrazione descriptor-driven (preferita). Identifica `acphytbl_info_rev0`
e `acphytbl_info_rev2`, li interpreta come array di entry da 20 byte
(`void *ptr; u32 len; u16 rsvd; u16 id; u32 off; u32 width;`), segue le
reloc `R_MIPS_32` sul campo `ptr` e dumpa ogni tabella in base alla `width`.

```sh
python3 extract_acphy_tables_from_descriptor.py wl.o output_dir/
```
Output: `acphy_tables_full.c` (i 25 array pronti) + `acphy_tables_index.txt`.

### extract_acphy_txgain.py
Dumpa le tabelle orfane `acphy_txgain_*` non coperte dai descriptor
(non sono in `acphytbl_info_rev{0,2}`). Band-specific, post-MVP.

### extract_chan_tuning_2069rev4.py
Legge `chan_tuning_2069rev4` (DSL-3580L, radio 2069 rev4,
stride 0x3a = 58 u16/entry). Il register-map cluster1 (offset 4..92) e' una
LUT statica nel file. Gli offset 94..114 sono programmati dal tail
chip-specifico 0x4352 e restano `TODO`: vanno ricavati dalla traccia MMIO.

## Numeri (sul blob di riferimento)

- 25 tabelle init coperte dai descriptor (sufficienti per il MVP CCK)
- ~10 tabelle `acphy_txgain_*` orfane (band-specific, post-MVP)
