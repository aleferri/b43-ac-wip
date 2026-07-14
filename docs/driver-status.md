# Stato del bring-up

Il draft 1 di `set_channel` è validato op-per-op contro il trace vendor
`d6220/attach-to-bss-ch36` (22268/22268 op, 100%). Il primo bring-up HW
è quindi atteso funzionante su:

- **Board**: NetGear D6220 (o board equivalenti con chip 0x4352, radio
  2069 rev 4, 3 cores).
- **Canale**: 36 (5180 MHz) validato op-per-op. Ch44 dovrebbe funzionare
  (95.8% delle op sono chan-invariant vs ch36, vedi
  `channel-generalization.md`) ma non è validato.
- **Bandwidth**: solo BW20. BW40 è rifiutato esplicitamente da
  `set_channel` con `-EOPNOTSUPP` in attesa di implementazione.

Altri canali 5 GHz sono untested. La generalizzazione richiede
estrazione di ~40 registri radio-chain per canale + ~30 chanspec
tables (piano operativo in `channel-generalization.md`).

## Mappatura file sorgente → patch

I sorgenti in `src/` sono la **fonte di verità**; le patch in `patches/`
sono un artefatto rigenerato.

| File sorgente | Patch |
|---|---|
| — (ssb/bcma SPROM) | 0001 |
| `phy_ac.h`, `tables_phy_ac.{c,h}`, `Makefile` (primi add) | 0002 |
| — (registrazione canali 5 GHz, `main.c`) | 0003 |
| — (DMA 64 KB alignment) | 0004 |
| `radio_2069.{c,h}`, `Makefile` (radio add) | 0005 |
| `phy_ac.c`, `phy_ac.h` (update), `farrow_phy_ac.{c,h,inc}`, `rxgain_phy_ac.{c,h}`, `rxiqcal_phy_ac.{c,h}`, `Makefile` | 0006 |
| — (bcma PMU init) | 0007 |
| — (core TX/RX wiring, `xmit.c`/`main.c`) | 0008 |
| — (bcma PCI bridge ID) | 0009 |

Le patch "—" toccano file fuori da `drivers/net/wireless/broadcom/b43/` e
non hanno corrispondente in `src/`: vanno mantenute editando la patch.

## Rigenerazione patch 0006

Non esiste ancora uno script automatico. Procedura manuale:

```sh
# 1. Sparse checkout del kernel target
git clone --depth=1 --filter=blob:none --sparse \
    https://github.com/torvalds/linux.git /tmp/linux
cd /tmp/linux
git sparse-checkout set drivers/net/wireless/broadcom/b43

# 2. Applica in sequenza le patch 0001-0005 e committa ognuna
for p in $B43_REPO/patches/000[1-5]*.patch; do
    git apply --3way "$p" && git add -A && git commit -m "$(basename $p)"
done

# 3. Applica 0006 originale e sovrascrivi phy_ac.{c,h} con i sorgenti
#    di questo repository
git apply --3way $B43_REPO/patches/0006-*.patch
cp $B43_REPO/src/phy_ac.c $B43_REPO/src/phy_ac.h drivers/net/wireless/broadcom/b43/
git add -A && git commit -m "post-0006"

# 4. format-patch tra i due commit → nuova 0006
git format-patch --start-number=6 -1 --subject-prefix='PATCH 6/9'
```

## Split upstream previsto per 0006

La 0006 attuale è un monolite (~7300 righe diff); prima della submission
a `linux-wireless` va spezzata in commit da ~300-500 righe ciascuno. Lo
schema previsto, con `set_channel` stub al passo 1 e riempito via via:

| # | contenuto | dipende da |
|---|-----------|------------|
| a | ops scaffold: allocate/free/prepare_structs, mode_init, init_regs, phyop r/w, ops struct con stub `set_channel`/`op_init`/`rfkill` | 0002 |
| b | TX power: pa5g_group, txpwrctrl_setup, txgain table, txpwr_by_index, idle_tssi_meas | a |
| c | RF sequencer + reset-time: rfseq tables/tbl_init, set_reg_on_reset, force_rf_sequence, reset_cca | a |
| d | Analog on reset: femctrl, tx_lpf, rx_lpf, dacbuf, pdet, analog_on_reset | c |
| e | Channel setup: classifier, clip_det, rxcore_setstate, rx_gate, channel_setup, chanspec_tail, coeff_bank, chan_tables, rx_evm_shaping, adc_reset, rx_enable — riempie `set_channel` | b, c, d |
| f | op_init + op_software_rfkill: wira il PHY nel framework b43 | e |
| g | rxgain_phy_ac.c/h | e |
| h | farrow_phy_ac.c/h/inc | a |
| i | rxiqcal_phy_ac.c/h (skeleton, gated off) | a |

## Note sulle scelte di implementazione

**SALAME**: la formula `(cur & 0x0800) | 0x05f4` per reg 0x0140 assume che
solo il bit 11 sia di stato PHY tra i 16 bit e che gli altri bit siano
invarianti; convalidata su 4 catture d6220 + 4 agcombo ma il RE dei
bit [10:4] non è completo — potrebbero esserci altri bit di stato che
nelle catture osservate erano casualmente uguali. Da rivalidare se il
compare fallisce su catture non testate.

**SALAME**: sul chip vero, se il PLL non locca velocemente, il poll
100×10µs dà 1ms di budget prima di emettere `b43dbg` — nessuna evidenza
che sia troppo o poco. Il vendor non fa polling (peek singolo), quindi
la scelta del budget è ingegneristica arbitraria.

**Divergenze cosmetiche note**:

- Convenzione `phy_mask`/`phy_set` (mask=0x0000) vs `phy_maskset` — 6 op
  residue nel freeze RX, semantica identica.
- Bit [10:4] di 0x0140 non ancora reverse-ingegnerizzati.

## TODO — set_analog_tx_lpf (celle table 7)

La formula RMW attuale in `b43_phy_ac_set_analog_tx_lpf` non riproduce
i valori che il vendor scrive nelle celle table 7 TX-LPF. Attualmente
skippata con hardcoded per il call-site `channel_setup` (`stages=0x1ff`,
`f0/f3/f6=-1`), targetizzato solo su d6220 ch36 BW20 attach.

Analisi bit-per-bit e piano di reverse engineering in `txlpf-formula.md`.
Skip temporaneo: valori hardcoded dal capture vendor d6220 ch36. I log
`[TXLPFLOG]` restano attivi in `tbl_write_lock`/`unlock`,
`actab_read`/`write_bulk`, `txlpf` loop, `dacbuf` loop, `rxlpf` loop —
da raccogliere via `dmesg | grep '\[TXLPFLOG\]'` su chip reale, poi
invertire la formula e rimuovere il hardcoded.
