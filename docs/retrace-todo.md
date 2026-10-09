# Open items

What still separates the port from the stock driver, one item per entry, with
the evidence and the next step. Closed items are not kept: their conclusions
live in the code.

Core items (`b43/` outside the AC-PHY files) cannot be seen by `test/unit`,
which compiles the AC-PHY files only; they need `test/integration`. Core work
the stock driver runs inside a PHY sequence, and b43 runs from the core at
another time, is emitted by `test/unit` at the stock driver's point through the
`B43_AC_CORE_SITE()` sites (`test/unit/core_sites.h`, a no-op in the driver);
each site is known work that moves to the core, none stands for work not
understood.

## Core

### MACCONTROL of the core reset and of the ucode start

The PHY writes no MACCONTROL value of the core's: the four `0x04000400`
of the first `up` are `b43_wireless_core_reset()`'s, `0x04000404` and
`0x04020402` are `b43_upload_microcode()`'s, the GPOUT clear and the empty
GPIO control are `b43_gpio_init()`'s, and the MAC toggle plus `0x04000400`
of the down are `b43_wireless_core_stop()`'s and the exit's. `test/unit`
emits them at the stock points (`B43_AC_SITE_CORE_RESET`, `_UCODE_LOAD`,
`_UCODE_START`, `_CORE_DOWN`). What differs from the stock driver in the
core itself, seen on the agcombo bus capture:

- The stock driver writes the first value of the PHY reset bracket
  (`0x14f`) twice; `b43_bcma_phy_reset()` once.
- In AP mode the stock driver runs with `DISCPMQ` clear (`0x0416040x`);
  b43 keeps it set, `0x4416040x`: the power management queue is out of
  scope (see below).
- The mode bits the stock driver toggles inside the PHY's phases -- beacon
  promiscuity off at the tail of the channel switch and at the head of the
  down, INFRA off / DISCPMQ on / AP off on the down -- have no core site in
  b43: `b43_adjust_opmode()` sets the mode once and the exit does not rewrite
  it. `test/unit` emits them at the stock points (`B43_AC_SITE_OPMODE_FILTERS`,
  `_DOWN_OPMODE`, `_DOWN_OPMODE_END`, and the head of the down from the flow);
  the BSS mode block before the TX power adjust -- TBTT hold, AP, INFRA,
  PRETBTT 2, beacon promiscuity -- is emitted by the flow before
  `adjust_txpower`, which suspends the MAC around its own body.
- The MAC toggles the stock driver makes inside its down have no effect
  in b43: the PHY's down runs under `b43_wireless_core_stop()`'s suspend
  and the `b43_software_rfkill()` bracket, so the MAC stays suspended from
  the stop to the core disable. `test/unit` emits them raw at the stock
  points (`B43_AC_SITE_DOWN_OPMODE`, `_DOWN_OPMODE_END`, `_CORE_DOWN`).

### Core sites inside the PHY: where the stock flow has a boundary

A `B43_AC_SITE_*` is core work the harness emits at the stock point because
b43 does it elsewhere. Before a site stays, the question is whether the PHY
function around it is one stock call or two pieces with the core in
between, in which case the PHY function splits and the core calls the
pieces in order. On the D6220 `cold01` (folded, op numbers of
`trace_filter.py --retvals` on the stripped segment) the evidence is:

- **The cold preamble spanned two driver entry points.** Ops 48-84 run in
  the stock attach (cpu1): the cold AFE arm with `0x02e4`, the seven
  host-flag clears, the second AFE arm, the PMU request and the slot 2/3/3
  host flags. Op 85 comes 2268 ms later on cpu0, in the `up`. That part now
  runs at b43's attach reset (`b43_phy_ac_attach_mac_preamble()`). What
  remains in `b43_phy_ac_cold_mac_preamble()` is the stock `up`: core reset,
  PMU and clock, LED GPIO, slot 4, two resets, slot 0 (the first that
  reaches shared memory), a reset, the PMU release, ucode load, the seven
  clears again, ucode start. Its sites are perimeter: the three extra
  resets are stock only (b43 resets once), and the first reset, the ucode
  load and the ucode start are b43's own, earlier. The host flags reach
  the same cells either way: `b43_upload_microcode()` zeroes shared memory
  after the PSM jump, and this part writes them after the ucode is up.
- **The tail of `b43_phy_ac_op_switch_channel()` alternates.** Ops 85-11906
  are one chain (cpu0, 200 ms, no gap above 22 ms), but in blocks: PHY up
  to op 11094; then core only, no PHY or radio op, ops 11094-11390 -- the
  `0x05e0` zeroing, the 56 key rows (`_KEYS_CLEAR`), the MAC configuration
  cells, the first top rows (`_MACFILTER_FIRST`); PHY again, ops 11390-11834
  (chain mask, TX power control setup, AFE gains, 310 PHY writes); core only,
  ops 11834-11906 (`_OPMODE_FILTERS`, host flags, `_MACFILTER`). The two
  core blocks are room for the core: the tail splits into PHY pieces, and on
  the first bring-up b43 runs `b43_security_init()`, the MAC cells,
  `b43_upload_card_macaddress()`, `b43_adjust_opmode()` and
  `b43_macfilter_set()` between them. The `0x05e0` zeroing and
  `0x018a`/`0x018c` go with the first core block (see "Shared-memory cells
  emitted from the PHY").
- **That tail is the `up`'s, not the channel switch's.** Every D6220 and
  TG789vac capture is a down/up per channel, so they cannot tell. The two
  6.30 captures with a scan can: on the MacBookAir6,1 first load (38
  segments) and on the archer-t5e (25) the `0x05e0`-`0x0666` zeroing comes
  only in the first segment, the `up`; `0x018a`, `0x003c`, `0x0074`,
  `0x0082`, `0x00ba` and `0x007c` only there and at the association; the TX
  power target `0x0646` and the width cell `0x005a` on every hop. The port
  runs the whole tail on every `op_switch_channel()`, so a mac80211 scan
  zeroes those cells and rewrites the MAC configuration on each hop. The
  hop/`up` split of the tail rests on 6.30 alone: on the 7.14 boards, in AP
  mode, the channel changes only through `wl down`, so every 7.14 capture of
  a new channel is a whole `up`, the agcombo bus ones included.
  The port follows 7.14 without the core part of the cycle: on the AC a
  channel change on a tuned interface takes the PHY down and up again
  (`b43_ac_phy_recycle()`) on the hot path, keeps the PHY's own state, which
  `op_prepare_structs()` resets only together with a core reset, and leaves
  out the core resets, the ucode load and the PLL words, which are the same
  on every channel of every 7.14 capture. In the integration harness a
  ch36 -> ch40 change after the AP start finds 20349 of the 25023
  ops of the D6220 hot `up` on ch40 (`02-up-ch40-bw20`), with no MACCONTROL
  reset, no ucode jump and no PLL write; its reads still come from the ch36
  oracle, so the values and the polls past its end do not count.
- **What stays perimeter.** A site stays a `B43_AC_CORE_SITE()` when b43 does the
  same core work at another point with the same effect; it leaves when the
  work has to happen at that point, or must not happen on some path the PHY
  function also serves. The key index block was of the second kind (see
  "Shared-memory cells emitted from the PHY"). The others are of the first:
  - `_MACFILTER_FIRST`, `_MACFILTER`: the BSSID and own-address rows, which
    `b43_upload_card_macaddress()` and `b43_macfilter_set()` write at the
    core init and on a BSSID change; the PHY cycle of a channel change does
    not reset the core, so the rows stay valid.
  - `_KEYS_CLEAR` (the rows; the key material and the index block are now
    with them in the harness):
    `b43_security_init()` at the core init, before mac80211 installs a key.
  - `_OPMODE_FILTERS`, `_DOWN_OPMODE`, `_DOWN_OPMODE_END`: MACCONTROL mode
    bits. b43 sets them in `b43_adjust_opmode()` and keeps beacon
    promiscuity on for an AC AP; the
    stock driver drops it at the switch tail, which receives no beacon that
    matters there, and at the head of the down.
  - `_CAC_CLOSE`: `b43_ac_cac_match_gate()`, from `b43_op_config()` right
    after the switch that armed the check.
  - `_BEACON_START`, `_BEACON_WD`: beacon template reloads, which b43 does
    from `b43_update_templates()` on the beacon changes and interrupts.
  - `_CORE_DOWN`: the core's down, `b43_wireless_core_stop()` and the exit.
  - `_CORE_RESET` x4, `_UCODE_LOAD`, `_UCODE_START`: see the preamble above.

### Order of the core init and of the exit around the PHY

On the AC, as in the stock driver, the whole MAC comes up before the PHY
and the PHY goes down before the PSM stops. What still differs at the bus:

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

- `GEN_IRQ_MASK`: `0xbae7aa64` (`B43_IRQ_MASKTEMPLATE_AC`) is the
  agcombo's AP mask, `0xb2e7a864`, plus b43's `MAC_TXERR` and `UCODE_DEBUG`,
  which the stock drivers mask; the mask writes differ from the capture by
  those two bits. The radio power-up and the three reserved bits of
  brcmsmac's `d11.h` are unmasked with no handler.
- Per interrupt b43 reads and acknowledges the DMA channels; the stock
  driver acknowledges channel 0 only, and only when it has a frame. Same on
  6.30: 672 acknowledgements of `0x0020` in the MacBook traffic capture and
  none of `0x0028`-`0x0048`, whose reasons it reads 43 times in all. b43's
  reads of the other channels are what catches a fatal DMA error on the TX
  rings.
- Ring control: b43 leaves burst and prefetch at the engine's reset values,
  0 on the agcombo before the stock setup. The stock drivers write their
  own: 7.14.43 on the agcombo TX `0x03700841`, RX `0x00500851`, the 6.30
  hybrid TX `0x03780841` and RX `0x036c0851`, so the values follow the
  driver and the host, not the chip. With the agcombo's the DSL-3580L raised
  a descriptor protocol error after every MAC enable. Whether the D6220's
  7.14.89 writes the agcombo's is not in any capture, since the wl-diag ones
  have no MAC registers.
- The receive index: the stock drivers keep it 256 (archer-t5e, 6.30) and
  500 (agcombo, 7.14) descriptors ahead of the engine; b43 gives the engine
  every buffer but the one before the next it reads (`b43_dma_rx_give()`),
  on OpenWrt 128 of them (see "On hardware").
- The stock driver reads the receive status twice per frame and the TSF
  once; b43 once and never. On 6.30 the TSF pair (`0x0180`/`0x0184`) also
  comes before every TX descriptor post.

### Key-table clearing

The stock driver initialises the key table inside the channel setup: it reads
the key table pointer (`0x0056`, `0x087a`), zeroes the key material from
`0x10f4` and the key index block at `0x05e0`, and clears the address match
rows; b43 does it from `b43_security_init()` at the tail of
`b43_wireless_core_init()`. `test/unit` emits them at the stock point
(`B43_AC_SITE_KEYS_CLEAR`) with the stock count. What remains in the core:

- **Row count.** The stock driver clears 56 rows (`0x00`–`0x37`), b43 50
  (`B43_NR_PAIRWISE_KEYS`). The wide layout has 64, so b43's limit is not the
  table's.
- **The first pair.** The stock driver writes the station row with `0x8008`
  and the BSSID row without flags first, after `0x018a`/`0x018c`
  (`B43_AC_SITE_MACFILTER_FIRST`); b43 does not.
- **Key material and index block.** The stock driver zeroes 480 words of
  key material and 68 of index block. `b43_security_init()` writes the
  material of its own 54 slots, 432 words, and the last six slots stay as
  they are: the key material lies above the 4 KiB of shared memory that
  `b43_upload_microcode()` zeroes. The index block it zeroes over 54 words,
  as the AC ucode reads an empty slot; the other 14 are inside those 4 KiB.
  `test/unit` emits the stock zeroing with the rows; at the bus the
  integration gates miss its 548 words per `up`.

### Shared-memory cells emitted from the PHY

`b43_phy_ac_shm_readback_block()` writes MAC cells, not PHY ones. It sits
in the PHY because the captures put it between the PHY write of `0x0339` and
the host flag that follows, and the core has no hook there. Moving it needs a
core entry point at that position. The key table init the stock driver runs
at the same point is under "Key-table clearing".

### Host-flag order

The stock driver keeps a shadow of the five HOSTF cells and writes one only
when the shadow changed (`b43_phy_ac_mhf_maskset()`). b43's core init writes
them between `WLCOREREV` and `MACHW`, much earlier; the harness follows the
stock driver. The shadow survives a down/up, so a hot run needs a seed on the
same lever as `AC_FIRST_INIT`.

### Other shared-memory cells

- **`0x00cc`.** Bits 8:6 are the `0x05d6` mask of the site that writes it,
  the OFDM cell of the TX core table (see "TX"), composed by
  `b43_phy_ac_bss_cc_update()`; bit 2 is always set and bit 0 is the
  core's: the encoding, 1 for OFDM, which `b43_write_beacon_phytxctl_ac()`
  sets before each beacon template as the stock drivers do. The PHY rewrites
  the mask at its two chain-mask sites; the first write, at the BSS
  configuration, b43's core does not do, and `test/unit` mirrors it. Wrong wherever the mask is (see
  "TX power"). `0x00d0` is written zero everywhere; nothing
  sets it.
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
  half (DSL-3580L 6.30.102.7 with ucode 784; D6220, TG789vac v2 and agcombo,
  7.14 with 928) except the x86 hybrid 6.30.223 with ucode 832, and
  `B43_FW_HDR_AC` does the same, 832 included. Whether that is the ucode or
  the hybrid build, and what the bit is, is open.

### Probe-response offload

Deliberately off: b43 writes `PRMAXTIME=1`, keeps HOSTF5 bit 15 clear, and the
cells are in `SOLO_VENDOR` of `test/unit/compare.py`, the bit in
`VAL_BIT_FUORI_PERIMETRO`.

HOSTF5 bit 15 is the switch, read from the 784 disassembly (`d11ucode42` of
`wlDSL-3580_EU.o_save`, `extract_ucode.py` + `b43-dasm -a 15`): the probe
request handler at `0x0A7D` (reached from the management dispatch at
`0x093C`, `r19 = fc >> 2 = 0x10`) matches the request's SSID against
`PRSSIDLEN`/`PRSSID` (`[0x24]`, `[0xB0]`, compare at `0x0AB5`-`0x0AC9`),
queues a response from the template (`0x0ACC`-`0x0AEE`, slot at `[0x5E]`,
age from TSF), and then at `0x0AF2` and `0x0CAC` tests `[0x6A]` bit 15: set,
`r20 |= 1` and the frame is flushed at `0x0A27`→`L665`, never reaching the
host; clear, it goes to the host. The response itself is dropped at
`0x0273`-`0x0279` when its age exceeds `PRMAXTIME`, counting in `[0xA6]`
(SHM 0x14C). So with the bit set and `PRMAXTIME=1` nobody answers: the AP
is invisible to an active scan and cannot be joined. The initvals leave
`PRSSIDLEN = 0x0b64`, so the SSID compare fails anyway. The stock driver
sets the bit at the `op_switch_channel` site (cold01 #9943 on the DSL,
#13879 on the D6220, and on every `up`) because it runs the offload; b43
writes the slot there on a first bring-up (`b43_phy_ac_mhf_write()`), with
the bit clear, and `b43_wireless_core_init()` ends HOSTF5 at `0x0088`.

To implement the offload:

1. keep only `b43_chip_init()`'s `PRMAXTIME=0`;
2. write the template at the layout's probe-response base, `0x04d8` on 784 and
   `0x0700` on 832 and 928 (the TODO above `struct b43_tpl_layout` in `b43/b43.h`),
   rewritten after every beacon once its length and the MACCMD valid bits are
   written: one `RAM_CONTROL`, then 76 words on the agcombo;
3. write `PRSSIDLEN`, `PRSSID` and `PRTLEN` with the beacon, as the stock
   driver does (`0x48`, `0x160`, `0x4a`);
4. write `0x0180`–`0x0186` = `0x0527`/`0x01f4`/`0`/`0x0032`, twice, the first
   inside `op_init` between `bcma_chipco_gpio_control()` and `mode_init`;
   `0x0184` is read back later;
5. set HOSTF5 bit 15 as the stock driver does (`0x8000` on slot 4 at the
   `op_switch_channel` site, `0x8088` in `b43_wireless_core_init()`), and
   handle `B43_RX_MAC_RESP` on the probe requests that still come up when the
   bit is clear;
6. remove the `SOLO_VENDOR` entries and `VAL_BIT_FUORI_PERIMETRO`.

### Power management queue

Out of scope. The AC stock driver clears `B43_MACCTL_DISCPMQ` in AP mode
(`0x44060402` → `0x04060402` on the agcombo); b43 keeps it set on every PHY,
so the ucode holds nothing back and mac80211 buffers for the stations in
power save on its own. The AP `MACCONTROL` differs from the capture's by
that bit on the integration gate, and `test/unit` emits the stock value
from its own model of the core (`emit_core_bss_mode()`).

What turning it on takes: `B43_TXH_MAC_IGNPMQ` on the frames mac80211 sends
to a sleeping station on purpose (`NO_PS_BUFFER`, `CLEAR_PS_FILT`);
`IEEE80211_TX_STAT_TX_FILTERED` for a status with suppression reason 1,
which `b43_txstatus_read_ac()` already decodes; and a reading of the queue
entries, which no capture shows the stock driver doing: the agcombo has no
client in power save, so no `B43_IRQ_PMQ`. `handle_irq_pmq()` drains at
most 256 entries per interrupt and warns with the last value read if the
queue is still not empty.

### HT and VHT

The AC announces HT 20/40 and VHT 80 on 5 GHz (`b43_ac_set_ht_vht_cap()`),
from the capabilities of the stock beacon in the agcombo bus capture
(`ch36.bin`, template RAM through `TPLWRPTR`/`TPLWRDATA`): HT `0x086e`
(`0x086f`, LDPC added, in its later beacons), MCS `ff ff ff`, TX MCS set not
defined; VHT `0x0f825832`, MCS 0-9 on three streams both ways. The port
takes the streams from the SROM `rxchain` and leaves out what b43 does not
do: A-MPDU and A-MSDU, MPDUs over 3895 bytes, LDPC reception, beamforming
and link adaptation. It defines the TX MCS set the stock beacon leaves
undefined, so that HT MCS go out as well. HT MCS and VHT MCS 0-9 go out on
as many streams as the board has TX chains, in the layout of the stock
drivers' own descriptors (`b43_txhdr_ac_ht()`, `b43_txhdr_ac_vht()`, from
the captures below, the DSL-3580L's 6.30 code and, for HT20, gonsolo's port
under 832.127). On the board HT20 and VHT20/40/80 are under test (see "On
hardware"). What is open:

- **HT40.** TODO. The width field and the HT-SIG's 40 MHz bit are written
  from 6.30's code; no microcode has run them and no capture has a HT
  frame at 40 MHz.

- **TX.** The TX descriptor is the AC microcode's long format,
  `d11actxh_t` of Broadcom's `d11.h` (see `PROVENANCE.md`), 124 bytes:
  per-frame fields, four rate blocks of which b43 fills the first, and the
  link fields left zero. In front of it go 4 bytes of TX offload header,
  `02 00 02 00`, and the IV offset carries the length of the 802.11 header,
  as gonsolo's port sends them under the 832.127 microcode, where they
  associate and carry traffic; under 784.2, the DSL-3580L's, the prefix is
  not checked against anything, and the PLCP of the rate block at offset 0
  is his as well. It replaced `format_598`, which put b43's cookie,
  the frame length and the chanspec where the AC ucode does not read them:
  on the DSL-3580L the first frame sent over DMA came back with frame ID 0
  and the beacon, out until then, stopped. With the HT TX MCS set and a VHT
  TX map of MCS 0-9 on every TX chain, mac80211 picks HT for an HT station
  and VHT for a VHT one, on up to as many streams as both sides have, and
  `b43_op_tx()` drops anything else. The VHT power offsets take
  the MCS's class on the frame's width (`b43_phy_ac_vht_rate_po()`), not
  checked against the stock values below, taken under the firmware's own
  configuration, whose power targets (`wl curpower`) are not in the
  collection. PHY TX control word 1 carries
  the rate's power offset, the field the PHY writes into the rate blocks
  for the ucode's own frames at that rate; 6.30 puts its per-rate power
  there on data frames too (`wlc_acphy_txctl1_calc()`), gonsolo's port
  leaves it zero. Open: `D11AC_TXC_UPD_CACHE`
  is not set; `ASEQ` and `LFRM`, which his port does not set; the multicast
  frame ID still goes to shm `0x00a8`, `B43_SHM_SH_MCASTCOOKIE` of the older
  microcode, which no AC capture writes (none sends a multicast frame).
  The only PHY TX control words in the captures are the beacon's, shm
  `0x00cc`/`0x00ce`/`0x00d0`, on every segment at 20, 40 and 80 MHz (D6220
  cold sweep): word 0 is `0x0045` or `0x00c5` (OFDM, bit 2, chain mask),
  word 1 the power offset alone (`0x0000`-`0x0060`), word 2 zero. A legacy
  frame on a 40 or 80 MHz channel thus has the 20 MHz words, with no width
  field set, as b43's descriptor has. The subband field is never non-zero:
  every chanspec of every capture (agcombo, D6220, TG789vac v2) has the
  primary at the bottom of its block.

  The stock driver's own TX headers are in `router-data/vd625-agcombo/`:
  the agcombo's `wl_vd625.ko` (7.14.43.21) and its microcode, not 784.2, at
  5g36/20, 5g40/40 and 5g44/80, primaries 36, 40 and 44, so off the bottom
  of the block at 40 and 80 MHz. `rxtx-1s-ht20-40-80.zip` has 1385 frames to
  a one-stream VHT station, `rxtx-2s-ht20-40-80.zip` 1089 to a two-stream
  one. Taken by `wl-diag` at `wlc_txfifo`:
  - The prefix is `02 00 00 00` on broadcast data and `02 00 02 00` on
    unicast QoS data; the 124-byte header and the frame follow. The IV
    offset is the 802.11 header's length (24, 26). `frame_len` counts the
    frame on air, FCS and the 8 bytes of CCMP MIC the hardware adds
    included, and the frame may go on in a further buffer.
  - Word 0: frame type in bits 1:0 (1 OFDM, 3 VHT), bit 2 on every frame,
    bit 3 on every VHT block from MCS 4 up and on none at MCS 0, with or
    without STBC (1-3 never appear), the core mask in bits 8:6 (`0x7`, the
    4360's three), and the width in bits 15:14 on VHT frames that fill the
    channel:
    `0x0000` at 20 MHz, `0x4000` at 40, `0x8000` at 80. OFDM frames keep
    `0x01c5` at every width.
  - Word 1: the subband in bits 2:0 on OFDM frames, 0, 1 and 2 for
    primaries 36, 40 and 44, the chanspec's sideband as b43 writes it; 0 on
    VHT frames that fill the channel. The power offset in bits 8:3, 3 to 8.
  - Word 2: on VHT the MCS in bits 3:0 and NSS - 1 in bits 5:4 (`0x0019`
    for MCS 9 on two streams), `0x40` with STBC on one stream; 0 on the
    6 Mbps OFDM frames. Word 1's power offset is the same for one and two
    streams at the same MCS.
  - PLCP: L-SIG on OFDM; on VHT, VHT-SIG-A1/A2 as 802.11 lays them out,
    group ID 63, both reserved bits set, NSTS - 1 in A1 bits 12:10, CRC and
    tail left to the hardware: the layout `b43_rx_rate_ac()` reads on
    receive. The SGI disambiguation and LDPC extra-symbol bits stay clear on
    frames of every length.
  - Rate: the PHY rate in 500 kbps (12 for 6 Mbps, 780 for VHT80 MCS8 SGI).
    FBW `0x0300` on every block; RTS/CTS `0x0905` on VHT blocks, with
    `0x0020` on the last one used; what b43 calls `bfm` has `0x43`-`0x48`
    in its high byte on VHT blocks, larger at higher MCS, 0 on OFDM.
  - Up to four rate blocks per frame: the stock rate control fills the
    fallbacks (MCS8 SGI, then 6, 5, 4).
  - MAC TX control `0x4c00`/`0x0002` on broadcast data, `0x45c0`/`0x0000`
    on unicast QoS data.
  - Beacon: shm `0x00cc` `0x01c5` at every width (`0x0045` at the first
    bring-up), `0x00ce` `0x0028`, `0x0019`, `0x002a` at 20, 40 and 80 MHz,
    subband and power offset as on OFDM data frames, `0x00d0` 0.

  No HT frame (FT 2): both stations are VHT, and the stock driver sends VHT
  at 20 and 40 MHz as well. The two zips are named after the channel width.

  The DSL-3580L's own `wl` (6.30, `wlDSL-3580_EU.o_save`, MIPS with symbols)
  builds the same words in `wlc_acphy_txctl0/1/2_calc()` and
  `wlc_compute_plcp()`, called from `wlc_d11hdrs()` and
  `wlc_beacon_phytxctl()`. From its code, against the 7.14.43 frames:
  - Same: VHT-SIG-A1/A2 (group ID 63, 0 only on a frame to an AP; partial
    AID 0), the HT-SIG (MCS, `0x80` at 40 MHz, byte 3 `0x07` with STBC,
    LDPC and SGI on top), word 2, the width in word 0's 15:14 (`0x0000`,
    `0x4000`, `0x8000`, `0xc000` at 20 to 160), the PHY rate in 500 kbps.
  - Word 0 bit 3: 6.30 never sets it, on data frames or on the beacon (the
    flag argument of `txctl0_calc()` is zero at both calls). Where it is
    set the function takes every TX chain instead of the rate's cores.
    **SALAME**: it is transmit beamforming, which is why 7.14.43 sets it
    from MCS 4 up together with the `bfm` byte. b43 leaves it clear. Under
    832.127 gonsolo's HT20 frames carry it clear and are acknowledged on MCS
    0-15 (his `notes/123`), with the cores at `0x3` from MCS 8 up.
  - The cores: by rate class, `wlc_stf_txcore_get()`: CCK, OFDM, then one,
    two and three space-time streams, the five cells
    `wlc_stf_txcore_shmem_write()` writes in that order. They are shm
    `0x05d4`-`0x05dc` (`3 3 3 3 0` in the DSL-3580L's cold sweep, every
    channel), so b43 takes the cell of the frame's class
    (`b43_txhdr_ac_cores()`) from the values it writes there. The table is
    at the same address under every AC microcode: 784 (the DSL-3580L), 832
    (the MacBookAir6,1's `wl-firstload-5g`, which also writes a sixth cell,
    `0x05de`, zero; the two-stream cell is `0x3` on all 305 writes while
    OFDM and one stream are `0x1` on most) and 928 (D6220, agcombo,
    TG789vac).
  - Word 1's subband: the chanspec's sideband shifted by the frame's width
    (`sb >> bw`) on every frame, which is the primary at 20 MHz and 0 on a
    frame that fills the channel.

  Not in the code read so far: FBW, MAC TX control and the RTS/CTS word of
  6.30. A short run of `wl-mmio-trap` with `dd_len` checks the headers on
  the bus side.
- **RX rates.** `b43_rx_rate_ac()` takes the frame type from PHY RX status
  0 with HT at 2 and VHT at 3, Broadcom's FT_HT and FT_VHT for these PHYs,
  and reads HT-SIG and VHT-SIG-A from the six bytes in front of the frame.
  The signal is the larger of the two cores' powers in bytes 9 and 10 of
  the header, -128 for a core that did not receive, as gonsolo's port reads
  them under 832.127. In `router-data/vd625-agcombo/rxtx-1s-ht20-40-80.zip`
  (7.14.43.21 and its microcode, taken by `wl-diag` at `wlc_recv`) all 1228
  frames read with b43's offsets (`reverse-tools/d11ac_rxh.py`): `frame_len`
  is the packet less the 40-byte header; PHY status 0 says OFDM (1) or VHT (3)
  and the six bytes after the optional padding hold the L-SIG or the
  VHT-SIG-A1/A2 `b43_rx_rate_ac()` decodes, the width in SIG-A1 that of the
  channel (34 frames at 20 MHz on the wider ones); the padding flag of MAC
  status puts the PLCP at +42 on 463 frames; the power bytes read -91 to -30
  dBm. `rxtx-2s-ht20-40-80.zip` adds 1242 frames from a two-stream station,
  all read the same way: VHT on two streams at 20, 40 and 80 MHz, and BCC
  (LDPC clear in SIG-A2) at MCS 0. The field at +22, which b43 does not read
  on the AC, is the chanspec (`0xd024`, `0xd926`, `0xe22a`). +24..+39 change
  with every frame and stay unread. Under 784.2 none of it is checked.
- **Receive buffer.** `B43_DMA0_RX_AC_BUFSIZE` holds a 3895-byte MPDU, the
  VHT minimum; the stock driver's own buffer size is not in any capture.
  The frame offset, 40, is the one both stock drivers write into the RX
  control register: `0x036c0851` on the MacBookAir6,1 (6.30.223),
  `0x00500851` on the agcombo (7.14).

### MAC and DMA, from the bus captures

`router-data/archer-t5e/` (hybrid `wl` 6.30.223, x86) and the agcombo's
`wl-mmio-trap` captures show what `wl-diag` never saw.

- **To diff:** the ~90 IHR writes after the ucode and the SHM bulks are the
  initvals, the five registers rewritten on every band switch (`0x686`,
  `0x680`, `0x682`, `0x700`, `0x684`) the bsinitvals. b43 takes them from
  `b0g0initvals42.fw`/`b0g0bsinitvals42.fw` of 6.30.163; the 6.30.223 values
  (decoded ops #1140–#1480) have not been diffed against them.
- **`MACCMD` bit 2** (`B43_MACCMD_DFQ_VALID`): set in every command on
  `B43_FW_HDR_AC`, as both stock drivers do; what it gates in the AC ucode
  is open.
- **Missing:** the null-data template at template RAM `0x2c` (power save).
  The AC writes the station address at template RAM `0x48`, eight bytes,
  right after the address-match row of the station (index 63), and b43 does
  the same in `b43_upload_card_macaddress()`; nothing is written at `0x20`,
  where the older microcode keeps it. In front of the null-data frame the
  MacBookAir6,1 `wl-init` capture also writes the BSSID, the station address
  and the BSSID again at `0x30` (18 bytes) when the station associates,
  which belongs with that template and is not written. The agcombo `ch36`
  bus capture writes `0x48` twice, both times after row 63; b43 writes it on
  every address upload, three times there, two of them with the address
  still zero.
- **A-MPDU.** b43 announces no aggregation and opens no block ack session:
  `b43_op_ampdu_action()` refuses every one, and is there because mac80211
  tries a TX session on every QoS frame to an HT station and warns on a
  driver without the op.
  What the stock driver does, from `router-data/vd625-agcombo/` (7.14.43.21 and
  its microcode): every MPDU is posted on its own, one descriptor each on TX
  ring 1, and the microcode builds the aggregate; the status that comes back
  covers the MPDUs it sent (see "TX status"). The descriptors of the MPDUs of a
  session (`rxtx-1s-ht20-40-80.zip`) differ from those of the two unicast QoS
  frames sent outside one in MAC TX control, `0x45c0`/`0x0000` against
  `0x4080`/`0x0004`, and in the cache info at +100, `50 04 20 20 26 15 3f
  14` against zeros: by the names of `d11actxh_t`'s cache fields in
  Broadcom's `d11.h`, BSS index and cipher, key index, 32 MPDUs at the
  primary and at the fallback rate, an A-MPDU duration of 5414, a block ack
  window of 63 and `0x14` as the maximum length. Through the association and the first
  aggregates the stock driver writes nothing to shared memory but the
  station's keys and its address match entry, so a session is not
  programmed into the microcode. Open: the density, the block ack request
  after lost MPDUs, and the receive side, whose upload traffic the trap run
  does not have.
- **TX status.** `B43_FW_HDR_AC` reads both packages of an entry and decodes
  the first. In the low half of its first word bit 0 is the valid bit, bit 1
  is set on every first package, bit 2 marks an intermediate status, bit 3
  the PM bit indicated to the AP, bits 7:4 are the suppression reason with
  the pre-AC codes (1 PMQ, 4 channel mismatch), bits 14:8 the number of
  MPDUs the status covers and bit 15 the acknowledgement. The third and
  fourth words hold the transmit attempts at four rates, rates 0 and 1 in
  bits 7:0 and 23:16 of the third, rates 2 and 3 in the same bits of the
  fourth. Bits 15:8 and 31:24 next to each count are zero or add up to
  bits 14:8 on all 472 statuses of the two captures, the MacBookAir6,1
  `wl-tx` (368) and the archer-t5e (104). On those statuses: two
  unacknowledged frames end after 3 + 1 + 1 + 2 = 7 attempts, the short
  retry limit; the three probe requests of the archer-t5e scan have
  suppression reason 4 and no attempt; the acknowledged statuses whose count
  sits in bits 23:16 only are aggregates of 2 and 4 MPDUs sent at rate 1;
  `0x810b`, bit 3 on an acknowledged frame, is the MacBook in power save. On
  b43's frames, which ask for the first rate only, bits 23:16 read 1 on
  every acknowledged frame (gonsolo's port, his note 110: counted as
  attempts they made every frame look retried, and minstrel kept the lowest
  rates), so `frame_count` is bits 7:0 alone. The field names are those of
  `TX_STATUS40_*` in Broadcom's `d11.h` (see `PROVENANCE.md`). Open:
  `b43_fill_txstatus_report()` splits the count between the rate and its
  fallback by the retry limit, though the descriptor asks for no fallback;
  the second package is read and dropped, the low half of its first word is
  `0x0001` on every entry. That 784.2 reports the same is not in any
  capture: both above run the hybrid 6.30.223, the driver gonsolo's 832.127
  comes from.

  `router-data/vd625-agcombo/rxtx-1s-ht-ampdu.zip` (7.14.43.21 and its
  microcode, `wl-mmio-trap` through an association and downlink traffic to a
  phone) has 2762 statuses, most of them for A-MPDUs of up to 32 MPDUs. The
  first package reads as above: bits 14:8 the MPDUs covered, their transmit
  attempts at rate 0 in bits 7:0 of the third word and the acknowledged ones
  in bits 15:8. The second word holds the 802.11 sequence number of the first
  MPDU covered. In the second package the second and third words are the block
  ack bitmap from that sequence number on (`0x3f` for six MPDUs all
  acknowledged; with gaps and bits past the MPDUs covered when the block ack
  says so), the first word has bit 0 set and `0x00010100` to `0x00030300` or
  zero above it, and the low half of the fourth reads `0xdead` on a status
  with no acknowledgement. The MPDUs of one status carry consecutive frame
  IDs: the next status starts its ID that many frames on.
- **Scratch and shared memory look alike to the comparison.**
  `tracelib.normalize()` drops `sel=`, and wl-diag prints a scratch word at
  four times its index, so scratch word 3 and shared `0x000c` are the same op
  to `cmp_skip`. `test/unit` traces scratch in the wl-diag form;
  `test/integration` traces it at the bus, in words, so against a wl-diag
  capture its scratch writes do not pair up. Keeping `sel=` and folding the
  wl-diag form would fix both.
- **Not classified:** the read-modify-writes on `0x6b4`/`0x6b8` (BT coex),
  `0x6c6`, `0x6f0`/`0x6f2`, the `clk_ctl_st` pass on every hop, the `gptimer`
  writes.
- At rmmod the stock driver issues `SI.COREREG core=0 off=0x80 val=4`
  (`pcie_watchdog_reset()`); bcma has no equivalent.

### ChipCommon and PMU

`test/integration` runs bcma's own ChipCommon, PMU and PCIe2 init, and
`test/d11sim` keeps their state (regions `CC`, `PMUPLL`, `PMUREG`,
`PMUCHIP`, `PCIE`, `PCIECFG`, `WRAP`). On the agcombo's `ch36` the cells both
sides write agree; what is open:

- **`max_res_mask`.** bcma writes `0x7ff` on the 4352 and the 4360, the
  value the agcombo's PMU reads after a `wl down`. On the MacBookAir6,1, a
  4360 rev 3 as well, `wl` 6.30 writes `0x1ff` and the register reads `0x1ff`
  with b43 loaded. Whether the mask is per board, per driver build or the
  hardware's implemented resources is not established, and bcma has one
  value for both chips. The `wl-diag` captures trace the PLL and regulator
  accessors (`PMU.PLL`, `PMU.RC`), not the resource masks.
- **`pmucontrol` `NOILPONW`.** bcma sets it on every PMU revision but 1
  (`bcma_pmu_init()`); `wl` 6.30 on the MacBookAir6,1 clears it (`0x01770381`
  read, `0x01770581` written) and the register reads `0x01770181` with b43
  loaded. The agcombo capture starts after the attach and does not show it.
- **Rewrites.** At every `up` `wl` writes `res_table_sel`/`res_updn_timer`
  (`6`/`0x00200001`), the PCIe2 `clk_control` (`0x00030050`) and the agent's
  out-of-band selectors (`0x02848180`/`3`) with the values it reads; b43
  leaves them, so the simulator reports them as written by `wl` alone.
- The ChipCommon GPIO cells are the LEDs, which b43 drives from the MAC.

### `do_full_init` after a down

`b43_phy_exit()` sets it back to `true` on every `ifconfig down`, so a down/up
takes the first bring-up branches (`B43_PHY_AC_STATE_FIRST_BRINGUP`), where
the stock driver's later `up` takes the others. Only a channel change on a
running interface (`b43_ac_phy_recycle()`), which does not go through
`b43_phy_exit()`, runs with it false. The hot `up` waits for the hot work.

## PHY: values

### Per-core power word `0x0644` on a later bring-up

On a later channel setup the stock driver writes `0x0644+stride = 0x0014 mask
0x007f` after the TX power-control enable bracket; on a first bring-up it does
not. `0x14` is not derivable from the SROM; the port writes it under
`!FIRST_BRINGUP`.

### TX target: the second of three passes at 40/80 MHz

The pass is in the port (`txpwr-target-derivation.md`, "The second pass").
Open:

- **The TG789vac does not take the cap**: 68 on all three passes of
  ch36/40, ch44/40 and ch36/80, where the port writes 62, 16 ops per
  segment. Same `wl` 7.14.89 as the d6220, same empty `ccode` and `regrev`
  0 on all three boards; its 40 MHz row (88) and 20 MHz row (80) give 68
  only through the 40 MHz cap of 74. A board gate would be a fit; the port
  follows the d6220.
- **The agcombo's legacy field on that pass** (72) follows its chain masks,
  which are already wrong on its first pass at ch36/40 (the one exception
  under "TX power: regulatory limits and chain masks").
- The chain masks of the pass take the Local Max 1 dB higher above 20 MHz
  (`b43_phy_ac_chain_pair()`), a fit that reproduces every board.

### TX power: regulatory limits and chain masks

The model is in [`txpwr-target-derivation.md`](txpwr-target-derivation.md).
Open:

- `0x05d8` at 0x5 on ch100 and ch116 at 80 MHz, where ch132/80, same rows
  and width, has 0x7; the port writes 0x7 on all three. Not temperature:
  the tempsense samples of the five TG789vac segments with 0x5 there read
  49-62 degC on the agcombo's calibration
  (`docs/tempsense-wiring-evidence.md`), in the middle of the sweep's
  44-62, against a tempthresh of 120.
- The agcombo's masks on the first pass at ch36/40: its driver reports an
  antenna gain of 0, the SROM has 5.5, and the port takes the SROM's.
- The ceiling (`b43_phy_ac_locale_ceiling()`) and the Local Max
  (`b43_phy_ac_local_max()`) are the same locale seen from two sides and
  should become one table.
- On the TG789vac ch36/80, 68 needs 18.5 dBm per chain, which no row of the
  agcombo's 36/80 table gives with the margin. **SALAME**: the edge levels
  differ between 7.14.43 and 7.14.89.
- `+0x0e` at 40 and 80 MHz on both boards (see "Per-rate field `+0x0e`").

### Per-rate field `+0x0e`

Both chips take the legacy limit of `b43_phy_ac_legacy_cap()`: a
single-chain limit per channel and width less the CDD offset of the chains in
0x05d6, the same table for the d6220 (4352), the tg789vac and the agcombo
(4360). What differs is the rows: on the 4352 they keep their SROM distance
from a capped target (`b43_phy_ac_rate_po()`), on the 4360 each rate sits at
its own entry of the finished table (`b43_phy_ac_rate_po_capped()`). The
beacon cell takes each chip's form, on the 20 MHz row at 80 MHz. On the final
pass of the cold sweeps the fields and the beacon cell are within 0 dB on 38
of the d6220's 43 segments (1 dB at most elsewhere but ch100/80's beacon,
3 dB) and on 32 of the tg789vac's 43; the agcombo's 26 are within 0 dB on 22
when the chain masks follow its driver's antenna gain of 0. Open: the second
pass at 40 and 80 MHz, where the stock single-chain limit is 1 dB higher on
ch52/40, ch108-140/40 and ch132/80 and the tg789vac's legacy rows are flat;
ch149 at 40 and 80 MHz on the tg789vac; the beacon at ch100/80 on all
three.

### Sub-band row offsets

`sb20in40*`, `sb20in80and160*`, `sb40and80*`, `dot11agdup*`, `mcslr5g*` are
extracted by `bcma/drivers/bcma/sprom.c` and not applied: which nibble goes to
which rate is in no open source. They are zero on the d6220, the agcombo and
the DSL-3580L; the TG789vac v2 carries `dot11agduphrpo=0x4444`. The recalc
warns if a board carries them.

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
d6220 profile, 60 with `BOARD=archer`, one of the two dual-band SROMs.

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
  the MacBook, whose SROM (`router-data/macbookair6-1/srom-raw.txt`) has
  `mcsbw202gpo = mcsbw402gpo = 0xaa555000`, `dot11agofdmhrbw202gpo = 0x4400`,
  CCK offsets 0 and `maxp2ga` 80. The rule is still to find.
- **First cell of each noise-shaping run.** 6.30 and 7.14 already write it
  differently on 5 GHz; 2.4 GHz keeps 7.14's 5 GHz value and warns.
- **Board data.** `0x06dd`, `0x06df`, `0x06e1`–`0x06e5` and table `0x07`
  `[0xf9 + core]` change between the two boards on the same core; `0x06e1`
  does on 5 GHz too, where the port writes the D6220's `0x0018`. The CCK rate
  cells `0x0a3a`… need the SROM's CCK offsets.
- **FEM control.** The port has the tables of `femctrl 6, femctrl_sub 0` (the
  routers) and `femctrl 2, femctrl_sub 1` (the MacBook, `boardflags3 = 1`),
  and stops with its warning on any other: the archer-t5e has `femctrl = 1`,
  and `femctrl 2, femctrl_sub 2` waits for a capture. After its table the
  MacBook reads and writes back unchanged `0x0418` (`0x0010`), `0x040a`
  (`0x0390`) and the chipcommon `gpioout`, `gpioouten` (`0xa7`) and
  `gpiocontrol`; with no change on the bus, field and value are not known
  and the port does not emit them. The 6.30 `ifup` captures (`wl-init-*`)
  do not write the table at all, where 7.14 rewrites it on every `up`.
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

- **Double analog programming.** Recorded on the agcombo (4360, 7.14.43): the
  AFE arm unit entered twice in the preamble, with the PLL rewritten in
  between. The port does not reproduce it and has no chip gate for it: it
  follows the D6220 (4352) and the TG789vac v2 (4360), both on 7.14.89, which
  agree. In their `cold01` the attach writes PLL control 2 and 3, then runs
  one unit -- AFE arm, `0x02e4`, the seven host-flag clears, AFE arm again,
  no core reset in between -- and the `up` writes the PLL again before the
  chanspec and arms the AFE once more inside `op_init`. The agcombo `cold01`
  has the same shape (its unit moves from cpu1 to cpu0 half-way, the same
  task migrating); where the double entry was seen is not recorded. On the
  hybrid 6.30.223 (MacBookAir6,1) the two arms of the attach are two entries,
  each after its own core reset, with the DMA rings probed in between.

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
- the shared-memory zeroing covers `0x10a4`–`0x1402`, 7.14 `0x10f4`–`0x14b2`;
- the beacon templates at template RAM `0x00d8`/`0x02d8` and the probe
  response at `0x04d8`, where 7.14 has `0x0200`/`0x0480`/`0x0700`: the bases
  are immediates in the ucode (784 at `0x045c`).

The captures carry the N-PHY core (wl0) as well, and `strip_other_core.py`
cuts nothing from them (one OTP read, no chip reset). Its ucode, `d11ucode22_mimo` 784.2, puts the
beacons at `0x0068`, so the `0x00d8` writes are wl1's. `0x0322`/`0x00cc` at
shared memory `0x0000`/`0x0002` (#9879) is a read in the BSS setup, not the
ucode revision.

## On hardware (DSL-3580L, OpenWrt)

- **Beacon and association.** The beacon goes out and stations find the AP
  on passive and active scans with every SSID length tried, and associate.
  Active scans and associations used to work only on some runs: the
  microcode was keeping the probe requests, HOSTF5 bit 15 (see
  "Probe-response offload"), set or not depending on whether a hot PHY
  cycle had rewritten the word. Fixed by keeping the bit clear. The stock
  side, for a comparison of the templates: `wl-diag` with `tpldump` on the
  DSL-3580L's own `wl` records each template RAM write with its content.
  On `router-data/vd625-agcombo/rxtx-1s-ht20-40-80.zip` (7.14.43.21 and its
  microcode) it is two writes per update: 12 bytes at template RAM `0x0e80`,
  zero but for the L-SIG at bytes 3-5 (6 Mbps, the length of the frame with
  FCS), then the frame at `0x0e8c`, padded to a multiple of four; b43's layout
  for 784 and 832, a 12-byte header with the PLCP at 3, is the same shape. The
  probe response goes to `0x3680` with no header at all, and the BSSID, 8
  bytes, to `0x48`. That is the firmware's MBSS mode, one template per BSS.
  `rxtx-1s-ht-ampdu.zip`, the same board under `wl-mmio-trap`, also has a
  stretch (369-634 s, SSID `TIM-Test` on 149) in the mode b43 uses, the layout
  b43 takes for 832: at every update the stock driver writes the PHY TX
  control word to `0x00cc` (`0x01c5`), the TIM position to `0x001e` counted
  from the start of the template, 12-byte header included (`0x44`), the
  template to `0x0200` or `0x0480`, whichever MACCMD does not mark valid, its
  length with the header to `0x0018` or `0x001a`, then sets that slot's valid
  bit in MACCMD; at the BSS setup also DTIMPER (`0x0012`) and the beacon TSF
  offset (`0x001c`, `0x3a`), and with every beacon the probe response template
  at `0x0700`, its length at `0x004a` and the SSID at `0x0160`. b43 writes the
  same cells in the same order.
- **TX under traffic, legacy.** `bringup-log-2026-10-08-bis.txt`, 20 MHz
  `NOHT`, has a station through the WPA2 4-way handshake and traffic both
  ways, with no DMA error and no restart. The first attempts end in
  `AP-STA-POSSIBLE-PSK-MISMATCH`, as in the 2026-10-09 log; one completes
  later. Not explained. `bringup-log-2026-10-08.txt` has
  one `TX-status contains invalid cookie: 0x0000` before `AP-ENABLED`. b43
  posts two descriptors per frame where the stock driver mostly posts one.
- **HT and VHT.** Under test. `bringup-log-2026-10-09.txt` is the first run
  with HT and VHT announced: the station associates and completes the 4-way
  handshake, then gets no address and leaves after about 20 s, again and
  again. The EAPOL frames go at the lowest rate (mac80211 marks the control
  port `USE_MINRATE`), the DHCP replies at the rate minstrel picks. That
  build put word 0 bit 3 on every VHT frame but MCS 0 and the OFDM cores,
  one on that channel, on every frame, two streams included; both now follow
  6.30 (see "TX" under "HT and VHT"), not yet run. The same log has the
  mac80211 warning for the missing `ampdu_action`, fixed since. HT20, sent
  since, tells the two apart: it shares the cores, the power and the
  subband with VHT, and not the VHT-SIG-B and the single-MPDU A-MPDU
  format every VHT frame has.
- **RX under traffic.** It works and is slow: at 20 MHz `NOHT` the station
  uploads at 4 Mbit/s and downloads at 11. In the 2026-10-08-bis log the RX
  ring underruns once at the association (229.5 s), then 36 `RX descriptor
  underrun` are printed, and `net_ratelimit()` drops 78 messages, between
  268.9 s and 301.4 s. Whether the log predates the underrun interrupt
  draining the ring (`b43_dma_rx_give()`) is not recorded. The ring had 32
  slots, from `813-b43-reduce-number-of-RX-slots.patch`, before the 816 in
  the package; the 816-02 sets 128. Every frame is decrypted by mac80211 (no
  hardware crypto on the AC) and none is aggregated. Next: the same traffic
  with a counter of underruns instead of the warning, with the 128 slots and
  with 256, and the CPU load of the softirq.
- **Values of the bring-up logs against `cold01-ch36-bw20.txt`.** Same as the
  board's own driver: radio `0x040b` reads `0x0169` after power-on, `0x0140`
  goes `0x0df7 -> 0x0df4`. Same as 7.14 and not as 6.30, which the board
  runs: the rccal comparators, `E/F = 0x0ac5..9/0x0ba8..b`, cap `0xaa`-`0xab`
  (6.30 here: `0x0b38/0x0c2c`, cap `0xb7`; the 7.14 boards `0x0a7e`-`0x0adc`),
  and the TX gain at index 64, `0x2f13`, bbmult `0x35` as on the D6220, where
  6.30 here writes `0x0767` and `0x42`. Different from both: the idle-TSSI
  readings, `0x0012 = 0x09a4..0x09ac/0x09bc..0x09d0` and base index
  `0x269..0x26b/0x26f..0x274`, where 6.30 here reads `0x0930/0x0944` and
  writes `0x24c/0x251`, and the D6220's core 1 reads zero. `PLLCTL3 =
  0x100e` is bcma's own write, which 6.30 here does not make
  (`0x00133333`). The ranges cover the six cycles of the two logs. Which TX
  gain table fits this board's front end is open: the two drivers program
  different gains at the same index.

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
- Before saying a field is missing, look in `b43/` and `bcma/`; before
  declaring a core constant underivable, look in `brcmsmac`.
- "Above 5250 MHz" and "radar duty" agree on ch52–140 and differ only on
  ch144–165: count any predicate on either over all 43 segments.
