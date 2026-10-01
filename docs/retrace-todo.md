# Open items

What still separates the port from the stock driver, one item per entry, with
the evidence and the next step. Closed items are not kept: their conclusions
live in the code.

Core items (`b43/` outside the AC-PHY files) cannot be seen by `test/unit`,
which compiles the AC-PHY files only; they need `test/integration`. Core work
the stock driver runs inside a PHY sequence, and b43 runs from the core at
another time, is emitted by `test/unit` at the stock driver's point through the
`#if UNIT_TEST` sites of `b43_phy_ac_core_site()` (`b43/phy_ac.h`); each site
is known work that moves to the core, none stands for work not understood.

## Core

### MACCONTROL of the core reset and of the ucode start

The PHY writes no MACCONTROL value of the core's: the four `0x04000400`
of the cold attach are `b43_wireless_core_reset()`'s, `0x04000404` and
`0x04020402` are `b43_upload_microcode()`'s, the GPOUT clear and the empty
GPIO control are `b43_gpio_init()`'s, and the MAC toggle plus `0x04000400`
of the down are `b43_wireless_core_stop()`'s and the exit's. `test/unit`
emits them at the stock points (`B43_AC_SITE_CORE_RESET`, `_UCODE_LOAD`,
`_UCODE_START`, `_CORE_DOWN`). What differs from the stock driver in the
core itself, seen on the agcombo bus capture:

- `b43_wireless_core_reset()` writes IHR only; the stock driver writes
  IHR | AWAKE at every reset (brcmsmac does the same), and b43 sets AWAKE
  at the first `b43_mac_enable()`.
- The PHY reset bracket: stock `0x14f -> 0x141 -> 0x145` (FGC high while
  in reset, FGC and PHY_CLKEN low on release, then clock), b43
  `0x14d -> 0x143 -> 0x145`.
- `b43_bcma_wireless_core_reset()` requests the two PLLs on `clk_ctl_st`
  (`0x300`); the stock driver never does, `EXTRESST` is already 7.
- In AP mode the stock driver runs with `DISCPMQ` clear and without
  `SHM_ENABLED` (`0x0416040x`); b43 keeps both (`0x4416050x`), the first
  under "Power management queue" below. `BEACPROMISC` is set for an AP by
  `b43_adjust_opmode()`, as the stock driver does on the AC.
- The mode bits the stock driver toggles inside the PHY's phases -- beacon
  promiscuity off in the calibration flush and at the tail of the channel
  switch, INFRA off / DISCPMQ on / AP off on the down -- have no core site in
  b43: `b43_adjust_opmode()` sets the mode once and the exit does not rewrite
  it. `test/unit` emits them at the stock points (`B43_AC_SITE_CAL_BCNPROMISC_OFF`,
  `_OPMODE_FILTERS`, `_DOWN_BCNPROMISC_OFF`, `_DOWN_OPMODE_INFRA`, `_DOWN_OPMODE_AP`);
  the BSS mode block before the TX power adjust -- TBTT hold, AP, INFRA,
  PRETBTT 2, beacon promiscuity -- is emitted by the flow before
  `adjust_txpower`, which now suspends the MAC around its own body.
- The MAC toggles the stock driver makes inside its down have no effect
  in b43: the PHY's down runs under `b43_wireless_core_stop()`'s suspend
  and the `b43_software_rfkill()` bracket, so the MAC stays suspended from
  the stop to the core disable. `test/unit` emits them raw at the stock
  points (`B43_AC_SITE_DOWN_OPMODE`, `_DOWN_OPMODE_END`, `_CORE_DOWN`).

### Order of the core init and of the exit around the PHY

The stock driver brings the whole MAC up before it touches the PHY -- ucode,
initvals, TX FIFOs, shared-memory cells, host flags, DMA rings, address
tables -- and on the AC `b43_wireless_core_init()` does the same:
`b43_chip_init()` stops after the MAC init and the PHY comes up at its end,
after `b43_security_init()`. At exit the PHY goes down before the PSM stops,
as the stock driver runs its down before the core reset. What still differs
at the bus:

- the six DMA interrupt masks: b43 writes all six, the stock driver
  `0x0024 = 0x10000` only;
- the template RAM: the stock driver fills every template at init, b43 at
  start_ap;
- the BSS bring-up. The stock driver's `wl up` runs the channel switch, the
  BSS configuration, the operating mode, the TX power adjust, the EDCF
  parameters and then the post-switch calibrations in one sequence. Under
  mac80211 the BSS configuration and the operating mode arrive at start_ap,
  after `config` and the `conf_tx` calls, so b43 runs the adjust and the
  calibrations from `b43_op_config()`, before the BSS exists. Moving them to
  `BSS_CHANGED_BEACON_ENABLED` would put them after the mode block as the
  stock driver has them; `conf_tx` cannot move. Open.

### Interrupts and DMA at the bus

With the agcombo capture's interrupts replayed (`IRQ` events of
`reverse-tools/timeline.py`, `dma.c` in the integration build), the
interrupt path and the rings are measurable. What differs from the stock
driver:

- `GEN_IRQ_MASK`: b43 runs with `0x38058264`, the stock driver with
  `0xb2e7a864`. The low half agrees on TBTT, ATIM, PMQ, MAC TX error and
  DMA; the stock driver also unmasks `0x0800`, `0x2000`, `0x20000`,
  `0x00e00000`, `0x02000000` and `0x80000000` and leaves `0x10000` and
  `0x08000000` masked, which b43 unmasks. What those bits mean on this
  microcode is not established; b43's names for them are the pre-AC ones.
- Per interrupt b43 reads and acknowledges five DMA channels; the stock
  driver acknowledges channel 0 only, and only when it has a frame.
- Ring control: the stock driver writes `0x03700841` (TX) and `0x00500851`
  (RX), b43 `0x00000801` and `0x00000851`: enable, parity disable and the
  40-byte receive frame offset agree, the burst-length and prefetch fields
  above bit 16 b43 leaves at zero.
- The receive index: the stock driver writes the low 32 bits of the
  descriptor's address, b43 the offset in the ring. Both address the same
  descriptor with the 64 KB ring alignment `dma.c` gives the AC cores.
- The stock driver reads the receive status twice per frame and the TSF
  once; b43 once and never.

### Key-table clearing

The stock driver clears the address match rows inside the channel setup, right
after `shm_zero_05e0`; b43 does it from `b43_security_init()` at the tail of
`b43_wireless_core_init()`. `test/unit` emits them at the stock point
(`B43_AC_SITE_KEYS_CLEAR`) with the stock count. What remains in the core:

- **Row count.** The stock driver clears 56 rows (`0x00`–`0x37`), b43 50
  (`B43_NR_PAIRWISE_KEYS`). The wide layout has 64, so b43's limit is not the
  table's.
- **The first pair.** The stock driver writes the station row with `0x8008`
  and the BSSID row without flags first, after `0x018a`/`0x018c`
  (`B43_AC_SITE_MACFILTER_FIRST`); b43 does not.
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

- **`0x00cc`.** Bits 8:6 are the `0x05d6` mask of the site that writes it,
  composed by `b43_phy_ac_bss_cc()`; bit 2 is always set and bit 0 is the
  core's, and what either means is open. The PHY rewrites it at its two
  chain-mask sites; the first write, at the BSS configuration, b43's core does
  not do, and `test/unit` mirrors it. Wrong wherever the mask is (see
  "TX power"). `0x00d0` is written zero everywhere; nothing
  sets it.
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
`base`, `62`, `base` (66/62/66 on the D6220, 68/62/68 on the agcombo). The
middle pass is the TX power site (`b43_phy_ac_txpwr_adjust()`), and what it
writes is **width-independent**: on each board the second pass of ch36/40 and
of ch36/80 carries the same target, the same legacy power and the same
beacon cell -- D6220 target 62, legacy 56; agcombo 62 and 44 -- and the
legacy power is exactly the board's own ch36/20 legacy power (D6220 56,
agcombo 44, from `+0x0e` of the 20 MHz segments). So the second pass is the
20 MHz computation of the primary channel, with one difference from the
20 MHz segments themselves: the target caps at 68 (target 62) where ch36/20
caps at 62 (target 56) on every board. A 20-in-40/80 limit 1.5 dB above the
20 MHz channel's own, with the legacy limit (62) unchanged, fits both boards;
the tg789vac's second pass leaves the target alone (68 on ch36/40 with a
first-pass cap of 74 and rows at 88, so a 68 cap would have dropped it to
62) and only moves the chain masks (ch108-140/40: 5 to 7), which is the
same pattern as its legacy rates: its driver does not recompute the power
table at that site. Not modelled; the port writes the first pass three
times. The port's masks on that pass take the Local Max 1 dB higher, which
reproduces every board but is a fit, not the mechanism above.

### TX power: regulatory limits and chain masks

One problem, seen in three places: the target on `0x?46`, the per-rate field
`+0x0e`, and the chain masks `0x05d6`/`0x05d8`. The target is closed on both
boards through the locale table of `b43_phy_ac_locale_ceiling()` (the caps
listed under "band edges" below, per channel and width, conducted), except
the d6220's second pass at 40/80 MHz. The chain masks `0x05d6`/`0x05d8` are
closed on the first pass of every segment of the three boards but one
(agcombo ch36/40) through the chain choice below with a Local Max table
(`b43_phy_ac_local_max()`), and on the second pass with that level 1 dB
higher above 20 MHz. `0x05da` follows `0x05d8` where that keeps more than
one chain and is coremask where it drops to one (7.14.89; the agcombo on
7.14.43 writes coremask throughout). Not temperature: the tempsense samples
of the five TG789vac segments with 0x5 there read 49-62 degC on the
agcombo's calibration (`docs/tempsense-wiring-evidence.md`), in the middle
of the sweep's 44-62, against a tempthresh of 120. Open: `0x05d8` at 0x5 on
ch100 and ch116 at 80 MHz where ch132/80, same rows and width, has 0x7 --
the port writes 0x7 on all three; and `+0x0e` at 40 and 80 MHz on both
boards.

**The stock limits, from the three `curpower` dumps** (`agcombo/stats.txt` at
ch100/80 and ch36/80, `dsl3580l/wl1_curpower_ch52-bw80.txt`, two driver
branches). The limits are per rate class, chain count and width, and their
structure is the same in all three to the quarter dB; only the level moves:

| rows | offset |
|---|---|
| 1 chain (OFDM, MCS0-7, VHT8-9), 20in80 | Local Max − 2 dB |
| 1 chain, 40in80 and 80 | Local Max − 1 dB |
| CDD on 2 chains, STBC | 1 chain − 3 dB |
| CDD on 3 chains | 1 chain − 5 dB |
| TXBF on 2 chains | 1 chain − 6 dB |
| TXBF on 3 chains | 1 chain − 9.75 dB |

The level is the "BSS Local Max", the value cfg80211 gives as `max_power`
(23 dBm at ch36, 30 at ch100: ETSI), less the antenna gain the driver reports
(0 on the agcombo whatever its NVRAM says, 5.5 dB on the DSL). The target is
`min(board, limit) - 1.5 dB` per rate and the register takes the maximum over
the rates, as the Power Targets of `curpower` show.

**The chain masks follow the limits.** Per class the stock driver takes the
chain count with the highest total power, the limit plus 3.0 / 4.77 dB for 2 /
3 chains, the fewer chains on a tie; `0x05d6` behaves like the TXBF rows and
`0x05d8` like the CDD rows. With one level per channel and width this gives the
two masks and the target on 75 of the 86 cold segments of the d6220 and the
TG789vac, and the same level fits both boards on 41 of 43 channel/width pairs.
**SALAME**: the class of each cell is the best of four assignments tried, not a
known meaning. The mask carries the chain count: 1, 3 on the d6220; 1, 5, 7 on
the TG789vac, two chains being 0 and 2.

**What does not fit, and why.**

- A 20 MHz channel has its own limit: ch36/20 writes 56 on all three boards,
  15.5 dBm per chain with antenna gains of 0, 4.25 and 5.5 dB, where the
  20in80 row of the same channel is 21 dBm on the agcombo. No 80 MHz table has
  that column.
- Band edges: ch60/40 (60), ch100/40 (68), ch36/40 and ch36/80 on the
  TG789vac (68), ch100 at 20 and 80 MHz (76) are the same on every board that
  shows them, below what the level of the channel allows, and without changing
  the masks. They are locale caps per channel and width.
- The antenna gain: the d6220 (5.5 dB) and the TG789vac (4.25 dB) write the
  same targets, so on the 7.14.89 boards the gain the driver subtracts is not
  the SROM's.
- On the TG789vac ch36/80, 68 needs 18.5 dBm per chain, which no row of the
  agcombo's 36/80 table gives with the margin. **SALAME**: the edge levels
  differ between 7.14.43 and 7.14.89.

The port today takes the tighter of cfg80211's ceiling (one EIRP per 20 MHz
channel, less the SROM antenna gain) and its own locale table of the caps
above, and computes the masks from the chain choice on a Local Max table
measured from the masks themselves (84 on ch36-48/20, 120 on ch52-64/20,
124 on ch100-144/20, 106 on ch36-44/40, 136 on ch108-140/40, 104 on ch36/80,
132 on ch100-132/80, quarter-dBm EIRP, less the SROM gain). The two tables
are the same locale seen from two sides and should become one; the antenna
gain the agcombo's driver reports (0) is not its SROM's (5.5), so on that
board the SROM gain gives the wrong masks.

**The TG789vac's legacy rates at 40 and 80 MHz** are flat across the eight
OFDM rates on every bonded segment. Where a legacy limit binds on both
boards the port now carries it (`b43_phy_ac_reg_ofdm_ceiling()`: 84 on
ch108-140/40, 72 on ch100-128/80, after the margin). Where none binds the
TG789vac writes the maximum of its **40 MHz row** on every rate, at 40 MHz
and at 80 alike -- 84 on ch108-140/40 and on ch132/80 (`mcsbw405ghpo`
nibble 1 under maxp 92), where its 80 MHz row would give 80/78 -- while the
d6220 on ch132/80 follows its 80 MHz row with its per-group offsets
(76/72/68, `mcsbw805ghpo` 2/4/6). Below the limits the two boards also
differ: ch100/40 TG789vac 60, d6220 64 on the six lower rates then 62 and
58; ch60/40 52 against 56; ch36 at 40 and 80 68 against 66 and 54. So the
TG789vac's driver reads the legacy rows of a bonded channel from a
different place than the d6220's, and the port's spacing table (the
operating width's row) is the d6220's reading.

The agcombo (4360, 3 chains, 7.14.43) settles what it is not. Its rows
hold exactly where nothing binds (ch52-64/20, ch52/40, ch108-132/40:
68/68/68/68/64/64/60/60 and 76/76/76/76/68/68/60/60 are its nibbles to the
quarter), so the group map is right on all three boards. Where a limit
binds it sides with neither: at ch100/40 and ch60/40 it writes the
TG789vac's 60 and 52 (d6220 64, 56); at ch100/80 the d6220 writes 72 and
the TG789vac 72 but the agcombo 60; at ch36/40 and 36/80 it writes 56 on
every rate, which is its own ch36/20 legacy power (d6220 66 and 54,
TG789vac 68); at ch100-140/20 its legacy sits at 68 under a target of 76
where the other two sit at 76 under 80 and 84. Chip and chain count are
ruled out (agcombo and TG789vac share both and disagree at ch100/80,
ch36/40, ch108/40); the SROM's `dot11agdup*` are ruled out (the agcombo
has none and still differs from the d6220). What is left is the driver
build's own legacy table, so a port can follow one reference only; it
follows the d6220 (7.14.89, the two full sweeps).

### Per-rate field `+0x0e`

The distance of each rate from the target, in sixteenths of a dB. At 20 MHz
it is closed: the legacy OFDM rows sit under their own limit, 76 after the
margin on ch52–144, on the d6220 (cold and hot) and the TG789vac alike, and
ch100/20 (target 76, K 0 on both) fits it too
(`b43_phy_ac_reg_ofdm_ceiling()`; the beacon cell `0x00ce` takes the same
cap). At 40 and 80 MHz the duplicate legacy rows are not under that limit
(ch108–140/40: target 80, fields 0) and what they are under is not one
number: d6220 ch60/40 writes K 0x10 and the TG789vac 0x20 at the same target
60, the TG789vac carries `dot11agduphrpo=0x4444` which the loader does not
apply, and on the d6220 ch149/80 writes K 0x10 with the target at the floor.
The three-pass segments (ch36/ch44 at 40 and 80) follow the target of each
pass. The CCK field is closed: the CCK rates sit at the 1 dBm floor and the
field is the target's distance from it saturated at 0xf8, on all 86 segments
(`b43_phy_ac_cck_rate_po()`); the d6220's UNII-3, maxp5ga 0, writes 0xf8
where the rule gives 0, kept as an observed exception.

**Where the general limit binds, the two boards disagree on the OFDM rows.**
The d6220 on ch100/20 (rows 86/86/82/78/74 for the five OFDM groups, target
76 under the 82 cap) keeps its spacing, 0/0/16/32/48: the cap moved the
target and left the rows. The TG789vac on ch36/20 (rows 76/76/76/76/72, cap
62) writes 0 on every rate, on ch100/20 (86/86/82/82/76, cap 82) 0 on every
rate, on ch104/20 (same rows, target 84) 32 on every rate, on ch108-140/40
(target 86, uncapped) 8 on every rate then 56 then 8, and on ch116/80 56 on
every rate -- while on ch149/20, uncapped, it follows its rows exactly
(0/0/0/16/16/40, `mcsbw205ghpo` nibbles 0/0/2/2/5). So the TG789vac's legacy
rates are flat wherever a limit binds and at every bonded width, the d6220's
are never flat. A per-rate clamp at the cap reproduces the TG789vac's 20 MHz
segments and breaks the d6220's ch100 at all three widths; the port keeps
the d6220's model. What separates the two -- 4360 vs 4352, 3 chains, the
`dot11agduphrpo=0x4444` the loader does not apply, a different legacy limit
table -- is not established. **SALAME** on all four.

### Sub-band row offsets

`sb20in40*`, `sb20in80and160*`, `sb40and80*`, `dot11agdup*`, `mcslr5g*` are
extracted by `bcma/drivers/bcma/sprom.c` and not applied: which nibble goes to
which rate is in no open source. They are zero on the d6220, the agcombo and
the DSL-3580L; the TG789vac v2 carries `dot11agduphrpo=0x4444`. The recalc
warns if a board carries them.

### RX IQ coefficient `b`, core 1

Solving per tone and averaging (`b43_phy_ac_iq_solve()`), on 309 points (every
cold segment of the d6220 and the agcombo, the TG789vac's outside the radar
channels, the d6220 and agcombo hot ups), `a` matches on 296 and `b`
on 259. The 16-bit mantissa on `ii` that gains `a` six points loses one, core 1
of the d6220's bss-up on ch52 (two tones averaging to −38.5005, the stock driver
writes −39), which is the first divergence of that segment. The `b` misses are one LSB, mostly the stock driver high and mostly on
core 1; `rxgainerr5ga*` is flat. An input outside the six accumulators is
missing. `VAL_TOLLERANZA` holds `0x?a1` at ±1 for the positional gate only.

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

### Hot `up`: the core's MAC block after the PHY

On the three hot `up` segments 232-250 of the missing ops are one block, at
the end of the segment after the PHY's last write (`PMU.RC` release):
`MAC.MCTRL` 0x04000400 x4 and 0x04000404, host-flag masksets on MHF0-4
(`0x0080`, `0x0100`, `0x0010`, `0x1504`, ...), `MAC.MCTRL` 0x04020402, the
address-match table cleared row by row (`AMT.WR` + `OBJ.BULKR`/`BULKW`
0x0000-0x0010 and 0x00e8-0x00f8), and the core's shared-memory init
(`OBJ.WR` 0x0016, 0x0018, 0x001c, 0x0044, 0x0046, 0x005c-0x0062, 0x0078,
0x0080, 0x00c0, 0x00c2, 0x00d4, 0x078c-0x0790). It is `wl up`'s MAC
re-initialisation -- b43's `b43_wireless_core_init()` material, host flags,
keys, TSF and BSS cells -- which the stock driver runs after the PHY where
b43 runs it before. Not the PHY's, but the perimeter cannot drop it by
pattern: the PHY's own `b43_phy_ac_mhf_maskset()` traces as `MAC.MHF` with a
mask exactly like `b43_hf_write()` does, and the shared cells are not all in
`b43.h`. Either the harness's `up` flow mirrors main.c's block in the stock
order, as it already does for the BSS templates, or the perimeter learns the
block by position (after the last PHY op of the flow). The 16 extra ops are
the two rewrites of the 0x033a-0x0343 gain block at 0x3bf.

### When the CRS block is written

The value is closed ([`crs-min-power.md`](crs-min-power.md)), and so is the
rule in the port: after a calibration the block is written at the next latch;
otherwise at a latch where some chain's index in force has moved three ladder
steps or more from the one it carries (`b43_phy_ac_crs_moved()`). It explains
every block of 80 of the 85 cold segments of the d6220 and the TG789vac, and
the 70 hot ups of the d6220 and the agcombo, where 16 latches sit two steps
away and none is written.

Open: five blocks the stock driver writes at two steps or less, d6220 cold07
(latch 45), cold09 (12), cold35 (40), TG789vac cold32 (36) and cold39 (2 and
6). The rings give exactly the values written there, so the arithmetic is
right and only the trigger is missing. The operations before those latches are
an ordinary watchdog turn. Tried and worse: a band on the levels of the sample
instead of the ladder steps (67), on the mean level of the ring (77), in dB of
the mean power (67 at best), separate bands on common threshold and bank (68).
Neither the time since the last write explains them (cold39 rewrites at the
second latch). The ring reset on the hot sweep (`hot-sweep.zip!hot-gaps/`) is
open too.

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
