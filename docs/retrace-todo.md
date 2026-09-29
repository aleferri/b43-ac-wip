# Open items

What still separates the port from the stock driver, one item per entry, with
the evidence and the next step. Closed items are not kept: their conclusions
live in the code.

Core items (`b43/` outside the AC-PHY files) cannot be seen by `test/unit`,
which compiles the AC-PHY files only; they need `test/integration`. Blocks the core emits at
another point than the stock driver are declared in `MOVED` of
`test/unit/cmp_skip.py`, each with its reason.

## Core

### Key-table clearing

The stock driver clears the address match rows inside the channel setup; b43
does it from `b43_security_init()` at the tail of `b43_wireless_core_init()`
(a `MOVED` rule). What remains:

- **Row count.** The stock driver clears 56 rows (`0x00`–`0x37`), b43 50
  (`B43_NR_PAIRWISE_KEYS`). The wide layout has 64, so b43's limit is not the
  table's.
- **The first pair.** The stock driver writes the station row with `0x8008`
  and the BSSID row without flags first; b43 does not.
- **Double key material.** The PHY zeroes `0x10f4`–`0x14b2` and `0x05e0`–`0x0666`
  (`shm_zero_10f4`/`shm_zero_05e0`), and `b43_security_init()` writes the key
  material and index block again, the index block as `(kidx << 4) | algo`
  over 54 words where the stock driver writes zeros over 68.

### Shared-memory cells emitted from the PHY

`b43_phy_ac_shm_readback_block()` and the two zeroing runs write MAC cells,
not PHY ones. They sit in the PHY because the captures put them between the
PHY write of `0x0339` and the host flag that follows, and the core has no hook
there. Moving them needs a core entry point at that position.

### Host-flag order

The stock driver keeps a shadow of the five HOSTF cells and writes one only
when the shadow changed (`b43_phy_ac_mhf_maskset()`). b43's core init writes
them between `WLCOREREV` and `MACHW`, much earlier; the harness follows the
stock driver. The shadow survives a down/up, so a hot run needs a seed on the
same lever as `AC_FIRST_INIT`.

### Other shared-memory cells

- **`0x00cc`.** Bits 6–8 are the chain mask in force; bits 0 and 2 are opaque.
  `gates.sh` passes the captured value as `AC_BSS_CC`, the driver does not
  compose it. `0x00d0` is written zero everywhere; nothing sets it.
- **`0x078c`–`0x0790`** (station MAC): whether the ucode needs it is open.
- **`SLOTT`**: the bsinitvals give `0x14`, the stock driver writes `9` in the
  readback block and the core writes `9` at core init. The `0x3ff` before it
  in the wl-diag captures is CWmax in the scratch space, not the slot time.
- **`PSM` (`0x05F4`), `TKIPTSCTTAK` (`0x0318`)** rest on the v4 layout
  assumption and have no evidence.
- **Cipher numbering.** On the AC microcode WEP104 is `3` (a 13 byte key on
  the agcombo's 7.14.43) and a 16 byte pairwise or group key is `5` (WPA on
  the archer-t5e's 6.30.223, and on the BCM4352 WPA2 capture the core patch
  cites), CCMP or TKIP not told apart; b43's enum has WEP104 at 4 and AES at 3. Hardware crypto is off on
  `B43_FW_HDR_AC` until the numbers and the key fields of the TX and RX
  headers are mapped.
- **`MACHW_H` (`0x00c2`), bit 31.** The capability register reads
  `0xb0518c05`. Every stock driver clears bit 31 before writing the high
  half (DSL-3580L 6.30.102.7 with ucode 802; D6220, TG789vac v2 and agcombo,
  7.14 with 928) except the x86 hybrid 6.30.223 with ucode 832, and
  `B43_FW_HDR_AC` does the same, 832 included. Whether that is the ucode or
  the hybrid build, and what the bit is, is open.
- **SSID length.** `AC_SSID_LEN` (default 8) drives the probe-response length,
  the PLCP of the eight rates and `0x001e`. `gates.sh` does not read it off the
  capture (`PRSSIDLEN` at `0x0048`), so a segment with another SSID fails
  silently.

### Probe-response offload

Deliberately off: b43 writes `PRMAXTIME=1`, and the cells are in `SOLO_VENDOR`
of `test/unit/compare.py`. To implement it:

1. keep only `b43_chip_init()`'s `PRMAXTIME=0`;
2. write the template at template RAM `0x0700` (the TODO above `B43_SHM_SH_BT_BASE0_AC` in `b43/b43.h`),
   rewritten after every beacon once its length and the MACCMD valid bits are
   written: one `RAM_CONTROL`, then 76 words on the agcombo;
3. write `0x0180`–`0x0186` = `0x0527`/`0x01f4`/`0`/`0x0032`, twice, the first
   inside `op_init` between `bcma_chipco_gpio_control()` and `mode_init`;
   `0x0184` is read back later;
4. remove the `SOLO_VENDOR` entry.

### Power management queue

b43 keeps `B43_MACCTL_DISCPMQ` set; the AC stock driver clears it in AP mode
(`0x44060402` → `0x04060402` on the agcombo) and drains the queue on
`B43_IRQ_PMQ`. The TODO is in `b43_adjust_opmode()` (`b43/main.c`).

### MAC and DMA, from the bus captures

`router-data/archer-t5e/` (hybrid `wl` 6.30.223, x86) and the agcombo's
`wl-mmio-trap` captures show what `wl-diag` never saw.

- **To diff:** the ~90 IHR writes after the ucode and the SHM bulks are the
  initvals, the five registers rewritten on every band switch (`0x686`,
  `0x680`, `0x682`, `0x700`, `0x684`) the bsinitvals. b43 takes them from
  `b0g0initvals42.fw`/`b0g0bsinitvals42.fw` of 6.30.163; the 6.30.223 values
  (decoded ops #1140–#1480) have not been diffed against them.
- **Missing:** the null-data template at template RAM `0x2c` (power save).
- **TX status, partly mapped.** `B43_FW_HDR_AC` reads the eight words and
  takes the frame ID and the acknowledgement (bit 15); bits 1–14 of the first
  word and words 1–7 are not mapped, so the transmit count is fixed at one.
  Bit 6 is set, with no acknowledgement and word 1 at 0, on three probe
  requests of the archer-t5e; word 2 looks like per-rate counts. A capture
  with retries would say.
- **Scratch and shared memory look alike to the comparison.**
  `tracelib.normalize()` drops `sel=`, and wl-diag prints a scratch word at
  four times its index, so scratch word 3 and shared `0x000c` are the same op
  to `cmp_skip`. The PHY used to write CWmin/CWmax into shared memory and the
  gates counted it a match. `test/unit` now traces scratch in the wl-diag
  form; `test/integration` traces it at the bus, in words, so against a
  wl-diag capture its scratch writes do not pair up. Keeping `sel=` and
  folding the wl-diag form would fix both.
- **Not classified:** the read-modify-writes on `0x6b4`/`0x6b8` (BT coex),
  `0x6c6`, `0x6f0`/`0x6f2`, the `clk_ctl_st` pass on every hop, the `gptimer`
  writes.
- At rmmod the stock driver issues `SI.COREREG core=0 off=0x80 val=4`
  (`pcie_watchdog_reset()`); bcma has no equivalent.

### `do_full_init` does not tell cold from hot

`b43_phy_exit()` sets it back to `true` on every `ifconfig down`, so on b43 it
is true on every bring-up and the comments in `b43/phy_ac.c` that read it as
"cold attach only" are wrong on the real path. It waits for the hot work.

## PHY: values

### Per-core power word `0x0644` on a later bring-up

On a later channel setup the stock driver writes `0x0644+stride = 0x0014 mask
0x007f` after the TX power-control enable bracket; on a first bring-up it does
not. `0x14` is not derivable from the SROM; the port writes it under
`!FIRST_BRINGUP`.

### TX target: the second of three passes at 40/80 MHz

On cold ch36 and ch44 at 40 and 80 MHz the three `txpwrctrl_setup` passes write
`base`, `62`, `base` (66/62/66 on the D6220, 68/62/68 on the agcombo). The same
middle value on two boards with different SROMs points at a different
regulatory cap in force (68 before the margin); which locale state, and why
only on U-NII-1 bonded channels cold, is not understood.

### Regulatory ceilings that cfg80211 cannot express

The stock driver's ceilings are per width, cfg80211's `max_power` per 20 MHz
channel. `AC_MAX_POWER_MAP` in `gates.sh` reproduces the 20 MHz ones; ch60 at
22 dBm and ch100 at 24 dBm hold at 40 MHz only. On the TG789vac v2 the ceilings
behave as conducted power, so the map is right only on a 5.5 dB board.

### Per-rate field `+0x0e`: a saturation

Where the field still diverges the stock driver mostly writes
`max(distance, K)`, K constant across the segment's rates: 1 dB on ch104–144
at 20 MHz and ch60/40, 2 dB on ch116/80, 3 dB on ch36/80. It is not the
regulatory ceiling nor a function of width or band (ch100, ch104–144 and
ch149–165 at 20 MHz share the SROM word and sub-band, with K 0, 1 and 0). It
points at the locale: ch116/80 and ch132/80 share SROM and target and differ in
K, and the agcombo writes twice the D6220's K. ch100 at 40 and 80 MHz has tail
steps of 1.5 and 2.5 dB and fits no form.

### Sub-band row offsets

`sb20in40*`, `sb20in80and160*`, `sb40and80*`, `dot11agdup*`, `mcslr5g*` are
extracted by `bcma/drivers/bcma/sprom.c`, zero on every board, and not applied: which nibble
goes to which rate is in no open source. The recalc warns if a board carries
them.

### RX IQ coefficient `b`, core 1

Solving per tone and averaging (`b43_phy_ac_iq_solve()`), `a` matches on 117
of 118 points and `b` on 90. The misses are one LSB, almost all on core 1, about
0.7 LSB high from ch100 up; `rxgainerr5ga*` is flat and the agcombo is clean on
three chains. An input outside the six accumulators is missing. `VAL_TOLLERANZA`
holds `0x?a1` at ±1 for the positional gate only.

### Loopback gain search at the floor

The halving criterion (`round(ii/1024) + round(qq/1024)` above `0x169e`)
reproduces 128 of 148 searches. The misses are chain 1 at 20 MHz from ch100 up,
where the search reaches index 0 still 0.3–17% above the window. The port
accepts the extreme index.

### Constants without a derivation

- the TX IQ/LO LUT bases of tables `0x42`/`0x62`/`0x82` per pa5g sub-band;
- which series of the 80 MHz eleven-tap bank (`0x?6a4`–`0x?6ae`) goes to which
  chain, tabulated by (chains, core);
- the partial chain mask at `0x05d6`/`0x05d8`, by chain count, width and site;
- table `0x07` `[0x3cd]`/`[0x3dd]`, which read and write the same values on
  every capture; block B2m's middle cell (one point per chain);
- the RX-LPF coefficient, 221/222 and 215/216 ([`txlpf-formula.md`](txlpf-formula.md));
- bits [10:4] of PHY `0x0140`.

### Discarded reads

Some sites read a value into a dummy and write a constant from `cold01`, which
is right on the D6220 at ch36 and invented elsewhere. `reverse-tools/reads.py
consumers` finds the write consuming each read in the capture;
`test/unit/read_perturb.py` perturbs one read at a time (sixteen masks) and
reports CONSUMED or DISCARDED. Closing a site means keeping the read in named
state and writing from it; the proof is the perturbation going to CONSUMED and
the hot segments. Still uncertain:

- PHY `0x0393` (`xor 0x8000`) and `0x0394` (`+0x105/+0x106`), `0x0550`;
- radio `0x0020/0x0022/0x003a` and strides;
- the `PHY.MOD`s the harness cannot perturb: `0x0070`, `0x016c`, `0x03a9`,
  `0x040f`, `0x0678/0x0878/0x0a78`;
- MAC cells: `OBJ 0x0314 → 0x00cc`, the `0x030a`–`0x0312` save/restore,
  `0x0a3a/0x0a56/0x0a72/0x0a8e` (transcribed in `b43_phy_ac_cck_rate_po()`),
  `0x0782 → 0x0048`.

## PHY: operations missing or extra

### `PHY.RDW` on table `0x20`

The stock driver reads 19 cells of table `0x20` at full width, a `PHY.RD 0x0011`
followed by two `PHY.RDW`; the port stops after the `0x0011` read, so 38 reads
are missing (cold01, cold05). Values seen: `0x7f00`, `0xf3ff`, `0xf32f`,
`0x1300`, `0xf34f`. Next step: find what table `0x20` is at that width.

### Counter sweep on late turns

When the stock timer runs late it skips the statistics-window latch on some
turns and the counter clear (`OBJ.WR 0x0308`–`0x0312`) on others: 12–36 extra
operations on the affected segments. `reverse-tools/watchdog_turns.py --check`
measures the turns; the next attempt is a rule on the time since the last
latch.

### When the CRS block is written

The value is closed ([`crs-min-power.md`](crs-min-power.md)); the moment is
not. On 12 cold segments, all radar-duty, the stock driver writes block E at
another latch than the port, or once more or less:

| segments | difference |
|---|---|
| cold12, cold13, cold16 | the stock driver writes after 26, 53 and 21 latches, the port after the first |
| cold07, cold09, cold28, cold35 | the stock driver has one extra block |
| cold17, cold18 | the port has one extra block |
| cold14, cold15, cold32 | weather radar, partial capture: the stock driver has only the first block |

The stock driver's last block falls 150–170 ms after the bss-up. `cold01` is
the only segment whose first watchdog turn collapses onto the window read; a
rule fitted on it alone would be a transcription. The steady-state
re-emission rule is not found (a hysteresis on the ladder fails the negative
test), and neither is the ring reset on the hot sweep
(`hot-sweep.zip!hot-gaps/`).

## 2.4 GHz

`switch_channel` refuses the band unless the driver is built with `ALLOW_24`,
as the unit harness is. `test/unit/band_gate.sh` compares the port's delta
between ch40/ch44 and ch2/ch3 with the one the MacBookAir6,1 and the archer-t5e
share (72 registers or cells, both on the 6.30 hybrid): 58 reproduced with the
d6220 profile, 60 with `BOARD=archer`, whose SROM is the only dual-band one.

**In the port, and matching both boards:** the 2069 channel table rows ch1–14
(ch14 in no capture) with the 5 GHz register map, radio `0x066d = 0x18c0`, the
absence of the `0x08c9`–`0x08c8` block, the Farrow resampler (`D/M = 80/3`,
`K = D * 2^29`, `0x1400`), `0x06de`/`0x06e0 = 0x014a`, `0x0299 = 0x4477` and
`0x03c1 = 0x0010` after `0x030d`, the TX gain table
`acphy_txgain_epa_2g_2069rev4` (384 of 384 words of the table `0x20` load), and
the noise-shaping runs of tables `0x44`/`0x45`.

**In the port, from the SROM, checked on the archer-t5e:** the RX gain header
from `rxgains_2g` (`0x0e`), and est_pwr from `pa2ga` (tables `0x40`/`0x60`,
128 of 128 on both chains).

Table `0x21` is written as zeros on 2.4 GHz, as on both boards.

**Open:**

- **TX target and per-rate fields.** Scan hops rewrite `0x0646` unchanged, so
  the only 2.4 GHz target captured is the MacBook's ch6 calibration, `0x2e`.
  The per-rate cells (`0x099a`… OFDM, `0x0a3a`… CCK) are flat `0x18` on the
  archer-t5e, whose 2.4 GHz offsets are all zero and `maxp2ga` 80, and zero on
  the MacBook, whose SROM is not available. Two boards and one known SROM do
  not fix a rule; a `wl srdump` of the MacBook would.
- **First cell of each noise-shaping run.** 6.30 and 7.14 already write it
  differently on 5 GHz; 2.4 GHz keeps 7.14's 5 GHz value and warns.
- **Board data.** `0x06dd`, `0x06df`, `0x06e1`–`0x06e5` and table `0x07`
  `[0xf9 + core]` change between the two boards on the same core; `0x06e1`
  does on 5 GHz too, where the port writes the D6220's `0x0018`. The CCK rate
  cells `0x0a3a`… need the SROM's CCK offsets.
- **FEM control.** The archer-t5e has `femctrl = 1`; the port has only
  `femctrl = 6`'s table and stops there with its warning.
- **Radio `0x002c`.** The core transition works on `0x002c` instead of
  `0x0033`; the bus shows a read and a write of the same value, so field and
  value are not known.
- **`0x0140`.** 6.30 sets bit 0 on the band with a MOD; the port writes the
  register in 7.14's form.
- **40 MHz** on 2.4 GHz has no capture; the Farrow setup warns and programs
  nothing there.
- **Version.** Everything above is 6.30. On 5 GHz the hybrid's TX gain table
  differs from 7.14's in 38 of 384 words, so the 2.4 GHz one needs a 7.14 blob
  before it is more than a 6.30 value.
- **Calibrations.** The only 2.4 GHz channel set with them is the ch6 one at
  the end of the MacBook capture, a hot one.

## Chip and board differences

- **Double analog programming.** The agcombo (4360, 7.14.43) enters the AFE arm
  unit twice in the preamble, with the PLL rewritten in between; the D6220
  once. The gate in `op_switch_analog` is on `chip_id`. The TG789vac v2 cold
  sweep (4360, 7.14.89) can tell chip from version and has not been checked.
- **TG789vac `watchdog=70000`.** Every rule counted in watchdog turns was built
  on the D6220's 1.004 s beat; measure the distance between two
  `PHY.MOD 0x0520 mask=0xc` on that board before reusing them.

## DSL-3580L (6.30): version differences, not debt

Recorded to tell version forks from hardware facts; none is a target. Compare
the symbols of the two blobs before assuming a board difference.

- prefregs: skips radio `0x065e` and a second write;
- AFE-LPF stage: writes radio `0x0045 = 0x703f`, never `PHY.MOD 0x0728 = 0x0800`;
- rccal: about 6 of 38 operations differ, also on the agcombo;
- init_regs: `0x0395/0x0315` on down→up, no `0x1645`;
- `0x02e4`: `0x0800` in the early phase, where 7.14 writes `0x0f00` cold;
- PLLCTL3 stays at the ROM value `0x00133333` (no fractional vcofreq);
- radio `0x0033`: `0x60b1`, where 7.14 writes `0x4060 → 0x4161 → 0x4181`;
- radio readbacks `0x040c`/`0x0416` differ between the two 4352s;
- the shared-memory zeroing covers `0x10a4`–`0x1402`, 7.14 `0x10f4`–`0x14b2`.

## Working rules

- An absence counts only if the class is traced in that capture
  (`router-data/CLASS-COVERAGE.md`).
- On a positional divergence open `cmp_skip.py --verbose` first: it pairs
  `delete` and `insert` when a block has moved.
- Measure a conditional change where the condition is true, not only where it
  is false.
- Size a tolerance on the residual of the model believed correct.
- Emit a contiguous block at every site or none, and narrow `PERIMETER` in the
  same step.
- Before saying a field is missing, look in `patches/`; before declaring a core
  constant underivable, look in `brcmsmac`.
- "Above 5250 MHz" and "radar duty" agree on ch52–140 and differ only on
  ch144–165: count any predicate on either over all 43 segments.
