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
  0x0006): the words are a function of the SROM fields, driven by core 0's,
  and `srom-sweep.zip` has how each field moves them.
- **Transmit beamforming.** With `txbf 1`, the only writes that differ are
  shared memory: `wl` reads SHM 0x00b0 = 0x0406 (in both runs) and, while the
  beamformee associates, writes slot 0 of the block at byte 0x080c: +0x08
  0x0015, +0x0a 0x0003, +0x0c 0x400d, +0x12..+0x16 the AP's own address. In
  the TX headers of the beamformee's VHT frames, rate-block `phy_ctl` word 0
  has bit 3 (0x0008) set and the rate block's last word is 0x0009 (0x0000
  without txbf), on all four rate blocks; the RTS/CTS word and the VHT-SIG-A
  the host writes are the same with and without.

