# ASUS RT-AC66U B1 — 2.4 GHz read-backs under `wl` 7.14

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
