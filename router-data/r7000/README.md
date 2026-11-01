# router-data/r7000 — Netgear R7000, two BCM4360 on wl 7.14.164.18 (ARM)

Netgear R7000 on DD-WRT r66097M (kernel 4.4.302, built with
`CONFIG_KPROBES=y` for `wl_diag`), a BCM4709 (two Cortex-A9) with two BCM4360
on PCIe:

- `eth1` is the 2.4 GHz radio (`deviceid 0x43a1`, `pci/1/1/`);
- `eth2` is the 5 GHz radio (`deviceid 0x43a2`, `pci/2/1/`).

```
chipnum    0x4360      chiprev 0x3     corerev 0x2a    radiorev 0x42069
boardid    0x661 (eth1) / 0x621 (eth2)  boardrev 0x1150 / 0x1451   sromrev 11
chains     3x3 (txchain=rxchain=7)      femctrl 3       ccode CA
driverrev  0x70ea412 = 7.14.164.18 (r692288)    ucoderev 0x3a02716
```

The same SoC family and radio as the Linksys EA6900 and the 5 GHz radio of
the Netgear R6400 ([`docs/northstar-guide.md`](../../docs/northstar-guide.md)).

## Files

| file | content |
|---|---|
| `cold-2g20.zip` | decoded `wl-diag` trace and log of `capture_cold_init.sh 2g20` on `eth1`: ch 1–11 (12 and 13 are not allowed in CA) |
| `cold-5g20.zip`, `cold-5g40.zip`, `cold-5g80.zip` | the same on `eth2`: 36–48, 144, 149–165 at 20 MHz; 36, 44, 149, 157 at 40; 36/80 and 149/80 |
| `hot-2g20.zip`, `hot-5g20.zip` | `capture_hot_init.sh` on the same channels |
| `sta-2g20.zip` | the first 2.4 GHz cold and hot sweep, taken before `wl ap 1` was set (see *Capture*): the driver was a STA, so `ssid` + `bss up` brought no BSS up |
| `txrx-2g11-phone.zip` | `capture_txrx.sh` with `MANUAL=1` on 2g11/20, DD-WRT's own WPA2 AP, a phone (Samsung S25 FE) passing `iperf3 --bidir` for 90 s: 68 `TPL.RAMW`, 961 `TX.PKT`, 512 TX statuses, 512 RX frames |
| `txrx-no-traffic.zip` | three earlier `capture_txrx.sh` attempts without usable traffic (a WPA2 BSS with `nas` stopped, an open BSS without a DHCPv4 lease), for completeness |
| `mmio-cold-2g6.zip` | `wl-mmio-trap` (`4-4-arm`) on `eth1`, cold to beaconing on 2g6/20: 52 621 MMIO accesses + 7 marks, the raw stream, `mmio2ops.py` output, the run log |
| `mmio-cold-5g36-80.zip` | the same on `eth2`, 5g36/80: 141 979 accesses + 7 marks |
| `fem-table-2g-ch6.txt` | PHY table 0x0a (FEM control) of `eth1` read back with `wl` up on 2g6/20, two identical passes: the femctrl 3 sub 3 table, which `mmio-cold-2g6.zip` does not contain |
| `revinfo.txt` | `wl revinfo` of both radios |
| `wl-info.txt` | `wl ver`, `cap`, `phylist`, `bands`, `chanspec(s)` of both radios |
| `nvram-radios.txt` | the `pci/1/1/` and `pci/2/1/` nvram (the SROM values), MAC addresses removed |
| `dmesg-wl_diag.txt` | `wl_diag`'s kernel log over all the captures: the hook plan at every insmod (39 of 57 hooks), no `lost` lines |

The `wl-diag` zips hold the decoded text (`decode-wl-diag.py`) and the
script's output with its full command line. In the two `txrx-*` zips the
addresses of other devices (neighbouring access points and their clients)
are replaced by `02:00:00:00:xx:xx`, and their solicited-node multicast
addresses by `33:33:ff:00:xx:xx`, consistently across both zips; the
R7000's, the phone's (a random one) and the broadcast and standard
multicast addresses are as captured. No other byte is changed: the op
counts are those of the original decode.

## Capture

DD-WRT's `wl` lacks `up`, `isup`, `revinfo`, `bss` and `ap`. `wlx` sends those
ioctls directly, and a `wl` shim hands them to it
([`wl-capture-scripts/ddwrt/`](../../wl-capture-scripts/ddwrt/)); with it
the capture scripts ran unchanged. After `insmod wl` the driver is a STA
(DD-WRT sets AP mode in its own `wlconf`), so the shim sends `ap 1` before
`ssid`. Every run had `PATH=/tmp/wlbin:$PATH
WL_KO=/lib/modules/4.4.302-rt232-st78/wl.ko KILL='nas wland'`.

`wl-mmio-trap` was loaded once `wl` was up, with the window pinned (both
radios are 0x14e4:0x4360 on PCI) and both addresses from `System.map`:

```sh
insmod wl_mmio_trap.ko autoarm=0 win_phys=0x08000000 win_phys_len=0x8000 \
    fsr_info_fn7=0x8055de04 init_mm_addr=0x80564928 fifo_recs=2097152 poll_secs=10
```

(`win_phys=0x40000000` for `eth2`.) Under the trap: `rmmod wl; insmod wl` (so
the attach is not traced), `ap 1`, `chanspec`, `up`, 10 s, `ssid`, `bss up`,
10 s beaconing; `drops=0`, `mismatch=0`. These two runs were taken before the
reader and label fixes of the ARM build: the mark labels in them decode
byte-swapped per word (the mapping is in their `README.txt`), the stream
was read with `dd bs=28`, and the records do not yet flag their device
offsets: decode them with `mmio2ops.py --format wl-mmio-trap-arm`, as the
`*-ops.txt` in the archives are. With the default format every 16-bit
access lands on the other half of its word (PHY register selects read as
data writes, shared memory 0x00cc as 0x00ce).

## What the wl-diag captures do not contain

On Northstar the `si_*` and `otp_*` functions and `dma64_txfast`/
`dma64_txunframed` are in the kernel, shared with the `et` Ethernet driver,
not in `wl.ko`; `wl_diag` lists them as `absent` and does not trace them. So
these captures have **no `SI`, `GPIO`, `PMU`, `OTP`, `SROMCTL` or `CS`
operations at all**, although `check_class_coverage.py` calls them complete.
`wlc_bmac_read_objmem16`, `wlc_bmac_write_objmem16` and
`wlc_bmac_set_addrmatch` are not in this `wl` either. See
[`../CLASS-COVERAGE.md`](../CLASS-COVERAGE.md).

## What it establishes

- **The PHY tables `wl-diag` never sees are written inline.** The MMIO trace
  of 2g6/20 writes tables 0x0e (40 entries, 32 bit), 0x15, 0x21, 0x40/0x60/
  0x80 (128 entries each) and 0x42/0x62/0x82 (first set to 0x0001 / 0x0101 /
  0xfe00 per core, then cleared), none of which appear in any `wl-diag`
  capture of this board, AP mode with a beaconing BSS included. On 5g36/80
  the trace selects 37 table IDs, `wl-diag`'s cold 5g80 run 13; 0x0e gets
  160 entries in 22 passes there, and 0x42/0x62/0x82 512 writes each. On this
  build `wl-diag` is not enough for the tables; use `wl-mmio-trap`.
- **On 2.4 GHz the full PHY init runs at the BSS start.** `wl up` alone makes
  167 PHY writes and selects no table; all 1943 table-ID writes and ~11.4k PHY
  writes come 6–67 ms after `wl ssid`. On 5 GHz `up` already does a large
  init (10.1k PHY writes) and `ssid` a second, larger one (~34.6k).
- Table counts are from a small parser of `mmio2ops.py`'s `PHY.WR` lines
  (0x00d = ID, 0x00e = offset, 0x010 high half, then 0x00f with
  auto-increment); check them against your own decoding.
