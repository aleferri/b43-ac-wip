# ASUS RT-AC66U B1 — read-backs and `wl-mmio-trap` captures under `wl` 7.14

The RT-AC66U B1 is RT-AC68U hardware: BCM4708, two BCM4360 (chip rev 3, AC-PHY
rev 1, radio 2069 rev 4, femctrl 3) on PCIe, the 2.4 GHz radio in PCIe domain 1
(DD-WRT `eth1`, BAR0 0x08000000), the 5 GHz one in domain 2.

`readback-2g.zip` holds the 2.4 GHz radio's state read back while DD-WRT
r63734 (the same `wl.ko` 7.14.164.18 as r66396, see PROVENANCE.md) ran it as an
AP, set to a channel with `wl chanspec` and left beaconing, 2026-10-30:

| directory | chanspec |
|---|---|
| `2g_ch1_20` | ch 1, 20 MHz |
| `2g_ch6_20` | ch 6, 20 MHz |
| `2g_ch11_20` | ch 11, 20 MHz |
| `2g_ch6_40` | ch 6, 40 MHz (`6l`, 0x1808) |

These are read-backs, not captured writes: what the PHY, the radio and the PHY
tables hold once `wl` has set the channel up, with no record of the order or
the intermediate values. They are what the 2.4 GHz 40 MHz path of b43 was
checked against on this board.

Each directory has:

- `chanspec.txt`: the output of `wl chanspec` after setting it;
- `phy.txt`: `<reg> <value>` for the PHY registers in `phyranges.txt`;
- `radio.txt`: `<reg> <value>` for radio registers 0x000-0x7ff;
- `tables.txt`: `<table> <offset> <lo> <hi>` for offsets 0x000-0x1ff of the
  PHY tables listed in `wlcap.sh`, read through 0x0d/0x0e (id, offset), then
  0x0f (low half) and 0x10 (high half), each entry twice; `RACE` marks an
  entry whose two reads differed.

`bcmpeek.c` is the reader (indirect PHY/radio access through the core's BAR0,
via `/sys/bus/pci/devices/*/resource0`) and `wlcap.sh` the script that ran it.
The values were read with `wl` running and nothing else touching the core;
`wl` itself may have changed a register between two reads, which the double
table read would show and the single register read would not.

## Captures and the SROM sweep (DD-WRT r66396, 2026-11-02)

DD-WRT r66396 (`4.4.302-rt232-st79`, the same `wl.ko` 7.14.164.18), with
`wl-mmio-trap` (`4-4-arm`) built and loaded as its README's section on a
stock DD-WRT firmware says. Each zip has a `README.txt` with the exact
sequence, the trap's counters and the scripts that ran it.

| file | what |
|---|---|
| `mmio-cold-2g6.zip` | `wl-mmio-trap` on `eth1`, cold to beaconing on 2g6/20, as `router-data/r7000/mmio-cold-2g6.zip` was taken: 109 541 accesses + 7 marks, drops 0, mismatch 0 |
| `mmio-cold-2g6-40.zip` | the same on 2g6/40 (`bw_cap 2g 3`, `6l`): 108 811 accesses |
| `mmio-txbf-5g36-80.zip` | `eth2` on 5g36/80, cold to beaconing, then a OnePlus Nord 2 (VHT SU beamformee) joining an open BSS and pulling 8 s of `iperf3` downlink, with the TX descriptors and headers recorded during the traffic; once with `wl txbf 1`, once with `txbf 0` |
| `srom-sweep.zip` | read-backs of the 2.4 and 5 GHz AGC words after changing one SROM rx-gain field in NVRAM at a time and rebooting (eight variants, the board's values restored after) |

What they show:

- **2.4 GHz AGC words.** The cold 2g6/20 trace writes 0x?6dc-0x?6e5 and
  0x?6ee with the values read back in `readback-2g.zip`. With the R7000's
  rx-gain fields in this board's NVRAM (`rxgains2gelnagaina*=3`,
  `rxgains2gtrisoa*=7`), `wl` writes exactly what the R7000 trace has
  (0x014a 0x0634 0x014a 0x0024 0x014a 0x0058 0x014a 0x0008 0x014a 0x0008,
  0x0006): the words are a function of the SROM fields, and `srom-sweep.zip`
  has how each field moves them. The sweep reads cores 1 and 2 at 0x16xx and
  0x26xx, which are not those cores: 0x16xx reads back as 0x06xx (505 of 512
  registers the same, the rest live values), and `readback-2g.zip` and the
  traces have cores 1 and 2 at 0x08xx and 0x0axx. So the sweep shows core
  0's words only and does not support its `README.txt`'s conclusion that
  core 0's field drives every core; table 7 0xfa and 0xfb, the cells of
  cores 1 and 2, point the other way: they move only in `r7k`, the variant
  that changes the fields of those cores too.
- **Transmit beamforming.** With `txbf 1`, the only writes that differ are
  shared memory: `wl` reads SHM 0x00b0 = 0x0406 (in both runs) and, while the
  beamformee associates, writes slot 0 of the block at byte 0x080c: +0x08
  0x0015, +0x0a 0x0003, +0x0c 0x400d, +0x12..+0x16 the AP's own address. In
  the TX headers of the beamformee's VHT frames, rate-block `phy_ctl` word 0
  has bit 3 (0x0008) set and the rate block's last word is 0x0009 (0x0000
  without txbf), on all four rate blocks; the RTS/CTS word and the VHT-SIG-A
  the host writes are the same with and without.

## Radar FIFO decode with known pulses (2026-11-02)

`radar-fifo.zip`: known radar pulses from a HackRF One next to the board (5254-5326 MHz, ch 52/80), the
four words of every pulse read back from the radar FIFOs (PHY 0x0253/0x0254, single reads), under `wl`
7.14.164.18 (DD-WRT r66396, radar and 802.11h on through `WLC_SET_RADAR` / `WLC_SET_SPECT_MANAGMENT`) and
under b43 with `wl`'s radar block written live; the `wl-mmio-trap` capture of that block (`radar-on-5g52`);
ETSI 1-4 and FCC 0-4 bursts with every pulse sent (`patterns/`); the scripts. Its `README.md` has the decode:

- time in 12.5 ns units: `w0 << 16 | (w1 & 0xfff) << 4 | (w3 & 0xf)` (1000 us PRI -> 80004);
- width in 0.2 us units: `w2 & 0xff` (5/10/20 us -> 25/50/100, floor ~2 us, ~27 us per entry);
- `w2 >> 8`: 0xff for CW and chirped pulses, erratic for noise-like ones and the AP's own beacons (meaning open);
- `w1 >> 12`: a 4-bit value that moves with frequency and chirp;
- the 13 writes `wl` makes when radar is on, which b43 lacks: without them its FIFOs stay empty.


## Transmit beamforming at 20/40/80 MHz, WPA2, two beamformees (2026-11-03)

`mmio-txbf2.zip`: `wl-mmio-trap` captures of `wl` 7.14.164.18 (DD-WRT r63734) bringing up txbf on 36/20, 36/40
and 36/80 with a OnePlus Nord 2 (two streams) joining, the last with WPA2, and with the rtl8821ae (one stream) as a
second beamformee, open and with WPA2, TX headers recorded during traffic; and SHM read-backs of the txbf block with the stations
joined. Its `README.txt` has the details:

- slot word 6 at the join: 0x000d / 0x200d / 0x400d at 20 / 40 / 80 MHz, the same in slot 1;
- word 5: 0x0003 for the two-stream station, 0x0002 for the one-stream one;
- words 7 and 8 are not written at a join: 0x1471 and 0x8492 at every width, in both slots;
- with WPA2, the key's row in the table at SHM 0x0668 gets 0x1000 | slot << 13 right after the station's AMT entry
  (0x0668 0x1000 for slot 0, 0x066a 0x3000 for slot 1); nothing on an open BSS;
- the RTS/CTS word of the rate blocks: 0x0905 to the slot-0 station, 0x1905 to the slot-1 one.
