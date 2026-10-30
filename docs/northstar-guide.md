# Captures and tests on a Northstar router

How to take traces of the stock `wl` driver on an ARM Northstar router
(BCM4709 with BCM4360 radios: Netgear R7000 and R6400, Linksys EA6900) and
send back what the port needs. Written for a router running a kernel the
user builds, with `wl` as a loadable module.

`wl-diag/4-4-arm/` is built against v4.4 only. Start with the dry run of
step 3, and send its output before arming anything.

It has run on a Netgear R7000 under DD-WRT ([`router-data/r7000/`](../router-data/r7000/)).
DD-WRT's `wl` lacks `up`, `isup`, `revinfo`, `bss` and `ap`, and its build
compiles `printk` out of out-of-tree modules: see
[`wl-capture-scripts/ddwrt/`](../wl-capture-scripts/ddwrt/) for both. On
that board the `si_*` functions are in the kernel, not in `wl.ko`, so
`wl_diag` does not trace them, and some PHY tables are written inline:
use `wl-mmio-trap/4-4-arm/` for those.

## What the port needs from these boards

- **2.4 GHz.** The port runs 2.4 GHz at 20 MHz from two 2.4 GHz captures
  only; 40 MHz on 2.4 GHz has none. Cold and hot sweeps of the 2.4 GHz radio
  are the main thing.
- **femctrl 3.** The port loads the FEM control table for femctrl 6, 2
  (sub 1) and 1 (sub 0), the boards captured so far
  (`b43_phy_ac_set_regtbl_on_femctrl()`); on femctrl 3 it stops with
  "not supported". Its table is unknown, on both bands. The sub is in
  `boardflags3`, so the NVRAM of step 2 says which one these boards have.
- **Board data from NVRAM.** These boards have no SROM; the port now reads
  the SROM rev 11 fields from NVRAM (`bcma/drivers/firmware/broadcom/
  bcm47xx_sprom.c`), checked on dumps of other boards only.

## 1. Kernel and module

The kernel needs, on top of its usual configuration:

```
CONFIG_KALLSYMS=y
CONFIG_KPROBES=y
```

`CONFIG_KRETPROBES` is not a separate option: it is `def_bool y` once
`KPROBES` is on and the architecture has `HAVE_KRETPROBES`, which ARM
selects for every little-endian non-XIP build, so enabling `KPROBES` is
enough. `wl_diag` attaches to `wl` with kprobes; the MIPS variants patch
code by hand because the vendor kernels lack them. Rebuild and flash that
kernel,
then build the module against the same tree, with its `.config` and
`Module.symvers`:

```sh
cd wl-diag/4-4-arm
make KDIR=/path/to/kernel-4.4 ARCH=arm CROSS_COMPILE=<your toolchain prefix> -j
```

Copy `wl_diag.ko`, and `wl-capture-scripts/capture_cold_init.sh`,
`capture_hot_init.sh`, `capture_profiles.sh` and `capture_txrx.sh`, to the
router, the three capture scripts in the same directory.

## 2. Board information

Before any capture, for each radio:

```sh
wl -i <if> ver
wl -i <if> revinfo
wl -i <if> chanspecs
```

`revinfo` says which interface is which radio (`deviceid`: 0x43a1 is a
BCM4360 for 2.4 GHz only, 0x43a2 one for 5 GHz only, 0x43a0 a dual-band
one) and gives the chip revision. The
port's PMU init is programmed for chip rev 3 only, the only revision
captured so far; another revision is something to know now. `chanspecs`
gives the strings the driver accepts, which is how a 40 MHz 2.4 GHz sweep
will be written.

And the NVRAM of the radios. Their variables sit under a per-device
prefix: `0:`, `1:`, ... on some boards, but `pci/1/1/`, `pci/2/1/`, ...
on the Netgear boards, the `devpathN` that names each radio's PCIe slot.
The kernel builds the same `pci/U/U/` prefix from `devpathN` itself
(`bcm47xx_sprom.c`), so the driver reads them wherever they are; the dump
is only for the offline check. Capture both the `devpath` lines and every
prefixed line so the prefix in use is visible:

```sh
nvram show | grep -E '^(devpath[0-9]+=|[0-9]+:|pci/[0-9]+/[0-9]+/)' > nvram-radios.txt
```

The harness in `bcma/harness/` runs the real `bcm47xx_sprom.c` on that
dump; pass the radio's prefix, and it strips it and checks every field:

```sh
make -C bcma/harness test_nvram
# the 2.4 GHz radio, on a Netgear board:
bcma/harness/test_nvram nvram-radios.txt pci/1/1/
# the 5 GHz one:
bcma/harness/test_nvram nvram-radios.txt pci/2/1/
```

`femctrl=3` showing up as a PASS there is the point: it means the driver
now reads it from NVRAM. It also means the PHY code will then stop at
"femctrl 3 not supported" until that table is filled from the captures.

## 3. The plan, dry run

With `wl` loaded as usual:

```sh
insmod wl_diag.ko
dmesg | grep wl_diag
rmmod wl_diag
```

One line per hook: `kprobe` or `kretprobe` with its address, `absent` if
this `wl` has no such symbol, `dropped` for a fallback whose accessor is
hooked, then how many resolved. Nothing is attached with `arm=0`. Send this
output: the hook table comes from `wl` 7.14.89 on MIPS, and which names
7.14.164.18 has is the first thing to know.

## 4. Capturing

The procedure is the one of `wl-diag/README.md`, "Capture workflow". In
short, on the collecting host first:

```sh
ncat -l 5555 > trace.bin
```

then on the router, from a wired connection or the serial console, not over
Wi-Fi (`rmmod wl` takes down both radios):

```sh
insmod /tmp/wl_diag.ko arm=1
cat /proc/wl_diag | nc <host> 5555 &
```

`trace.bin` is decoded on the host with
`python3 wl-diag/decode-wl-diag.py < trace.bin > trace.txt`; send the
`.bin`, it is the complete record.

The scripts need, in the environment:

- `WL_KO`, the path of `wl.ko`, found before the first `rmmod`;
- `IF`, the radio's interface, and `DEVID`, its deviceid from step 2: a
  sweep refuses an interface whose deviceid is not in `DEVID`, so a wrong
  `IF` does not give a plausible trace of the other radio;
- `SSID`, so that the BSS comes up and the per-core tables are programmed;
- `BR` and `KILL` if a bridge or a daemon holds the interface and `rmmod wl`
  fails.

### 2.4 GHz radio

```sh
export WL_KO=/path/to/wl.ko IF=<2.4 GHz if> DEVID=0x43a1 SSID=test-ap
sh capture_cold_init.sh 2g20
sh capture_hot_init.sh 2g20 $IF 10 $SSID
```

`2g20` is channels 1-13 at 20 MHz; channels the board's domain refuses are
skipped. For a few channels only, `BAND=2g sh capture_cold_init.sh 20 1 6 11`.

### 5 GHz radio

```sh
export WL_KO=/path/to/wl.ko IF=<5 GHz if> DEVID=<its deviceid> SSID=test-ap5
sh capture_cold_init.sh 20
sh capture_cold_init.sh 40
sh capture_cold_init.sh 80
sh capture_hot_init.sh 20 $IF 10 $SSID
```

The radar channels have their own profiles (`20dfs`, `20meteo`, ...) and
need `SETTLE` long enough for the channel availability check; see the
head of `capture_cold_init.sh`.

### Frames, if a station is at hand

`capture_txrx.sh` takes the TX and RX headers and the template RAM while a
station passes traffic; its chanspec list is written out in full, so it
covers 2.4 GHz as it is:

```sh
TXBF=0 VHTMODE=0 sh capture_txrx.sh <2.4 GHz if> test-ap "2g1/20 2g6/20 2g11/20"
```

## 5. What to send back

- the output of steps 2 and 3;
- `nvram-radios.txt`;
- each capture's `.bin`, with the command line that produced it;
- `dmesg | grep wl_diag` after the captures: `lost` and the kretprobe
  `return values lost` lines say whether anything was dropped.

## Known limits

- A kprobe costs an exception per accessor call. `wl` runs slower under it
  than under the MIPS detours; a sweep that times out where it should not
  is worth reporting.
- `maxactive` (default 64) bounds the pending calls of one accessor whose
  return value is recorded; past it the value is lost and counted.
- `wl-mmio-trap/4-4-arm/` traces the register accesses no accessor carries,
  by faulting the window; load it after `wl` is up and its self-test must
  pass first. It records TX descriptors too, with `dd_len`/`dd_budget` as on
  MIPS.
