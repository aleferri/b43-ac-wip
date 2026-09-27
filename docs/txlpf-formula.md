# Analog LPF caps (table 7 cells)

TX-LPF, RX-LPF and DACBUF are not transcribed. The cap is derived from rccal,
and the read-modify-write keeps the cell's prior contents. Checked on three
boards (D6220, DSL-3580L, agcombo).

## Formulas

- **TX-LPF.** The cap is `((RCCAL_F - RCCAL_E) * 193) >> 8`, with
  `RCCAL_E = 0x414` and `RCCAL_F = 0x415`. The cells are written as
  `lo = pre_lo | (cap << 9)` and `hi = pre_hi | (cap << 1)`.
- **RX-LPF.** `f17 = lpf_cap1`. `f6 = (lpf_cap0 * k[stage]) >> 8`, with
  `k = {221, 215, 215}` for the three sections. 7.14 scales; the DSL-3580L's
  6.30 uses `k = 256` (no scaling), a version difference.
- **DACBUF.** The cap is `(RCCAL_G & 0x03e0) >> 5`, with `RCCAL_G = 0x416`,
  taken from the readback **after** the apply. The first readback is before the
  apply and gives cap 0.

## Verification data

### Live readings via `wl phytable` (agcombo silicon)

| session | chan/BW | E | F | lpf_cap | cell 0x142 |
|---|---|---|---|---|---|
| t0 | 36/20 | `0x0a56` | `0x0c1f` | `0x58` | `0x5cdb` |
| t1 | 44/20 | `0x0a55` | `0x0c1e` | `0x58` | `0x5edb` |
| t2 (back to ch36) | 36/20 | `0x0a56` | `0x0c1f` | `0x58` | `0x5cdb` |

The write is deterministic for a given channel and temperature: t2 reproduces
t0 exactly, so it is not drift.

### Prior contents per stage group (invariant on D6220 and agcombo)

| stage | pre_lo |
|---|---|
| 0, 1, 2, 8 | `0x00db` |
| 3, 4, 5 | `0x0123` |
| 6, 7 | `0x016b` |

`pre_hi` is 0 in the relevant bits. Bit 0 of `hi` comes from the cap's top bit.

### Per-stage check (agcombo, ch36/20, cap `0xae`)

| cell | observed | `pre_lo \| (cap << 9)` |
|---|---|---|
| stage 0 lo (`0x142`) | `0x5cdb` | `0x5cdb` |
| stage 3 lo (`0x145`) | `0x5d23` | `0x5d23` |
| stage 6 lo (`0x148`) | `0x5d6b` | `0x5d6b` |
| stage 8 lo (`0x14a`) | `0x5cdb` | `0x5cdb` |
| stage 0 hi (`0x362`) | `0x015d` | `(cap << 1)` = `0x015d` |

### When rccal runs again

- **agcombo.** The stock driver re-reads E/F and recomputes the cap on every
  chanspec set, so `wl radioreg 0x414/0x415` shows the **last** rccal, not the
  one used for the initial write.
- **D6220.** rccal runs only in `radio_2069_init`, and the cap stays constant
  across channel switches.

## Open

Only the RX-LPF coefficient: 221 against 222, and 215 against 216, are
ambiguous on two samples. A third `lpf_cap0`, from another 5 GHz channel,
settles it. The TX-LPF is closed.

## In the code

- `b43_radio_2069_rccal` (`radio_2069.c`) reads `R2069_RCCAL_E/F`, computes the
  cap and fills `lpf_cap0`/`lpf_cap1`.
- `set_analog_tx_lpf` uses them as `f9`/`f17`.
- The RX-LPF path scales `f6` with `rx_k[stage]`.
