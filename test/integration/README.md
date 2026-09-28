# test/integration — the whole of b43 on AC

This suite compiles real b43 — `main.c`, `phy_common.c`, `leds.c`, `rfkill.c`
and the rest, at the tag of the installed kernel headers — with the
`patches/` series applied and the `src/` of this repository inside. It stubs
the bus and subsystem layers below and runs the probe and `ifconfig up` path.

Nothing stands in for b43: its gates, `else` branches and call order are its
own, and the trace is taken at the MMIO bus.

**State:**

- b43 compiles and links;
- `b43_bcma_probe` returns 0 and `hw->ops->start()` returns 0 on ch36 BW20;
- the stop and remove leave nothing behind, also under AddressSanitizer;
- the reference segment yields a trace of 29772 operations.

## Running it

Everything runs from this directory.

### Prerequisites

- `gcc`, `binutils`, `make`, `patch`, `curl`, `python3`, `unzip`; `gdb` for
  backtraces.
- **Kernel headers in `/usr/src`** (`apt-get install linux-headers-generic`).
  They need not match the running kernel, since nothing is loaded, but they
  decide which b43 is fetched. `fetch-upstream.sh` takes tag `vX.Y` from the
  header version (`6.8.0-142` → `v6.8`). With several versions installed, pass
  `KVER=6.8.0-142` to every `make`.
- Network access to `raw.githubusercontent.com`, for `make fetch` only.

### Steps

```sh
make fetch          # vanilla b43 at the headers' tag + the repository's patches/
make check          # every file of b43 and of the port: must say "0 errori"
make b43-trace      # compile, link, print the symbol count
```

**`make fetch`** must end with `applicate 5, saltate 5`:

- the 5 patches that touch `b43/` — the core, the PHY, the rev-42 MAC init, the
  address read-backs and the AC template layout — are applied;
- the 5 on bcma/ssb have nothing to apply here, but their hunks on
  `include/linux/ssb/` go into `kinc/`.

If a patch does not apply, the script exits with an error. Discard the tree
(`rm -rf b43-upstream kinc`) before retrying: `patch` applies one file at a
time, and a half-patched tree still compiles.

**`make b43-trace`** is expected to report `kernel 0` and every other bucket
at zero except `libc`. The `libc` symbols (27 today) are the ones `trace_out.c`
and `kernel_shim.c` ask of libc, and they resolve at the final link. If
`kernel` is not zero, the residue is in `residual.txt`: either stub work, or a
libc symbol missing from the `LIBC` list in the `Makefile`.

### The oracle

Without an oracle every read returns zero, the trace looks plausible and
measures nothing. Pass a D6220 cold segment, **stripped of the other core's
attach** and then **folded**, exactly as in `../unit/README.md`:

```sh
unzip -d /tmp/cold ../../router-data/d6220/cold-sweep.zip
python3 ../../reverse-tools/strip_other_core.py \
    /tmp/cold/cold01-ch36-bw20.txt /tmp/cold01-clean.txt
python3 ../../reverse-tools/trace_filter.py --retvals \
    /tmp/cold01-clean.txt /tmp/m01
make run ORACLE=/tmp/m01 TRACE_OUT=/tmp/int.trace
```

The strip matters more here than in `../unit`: the whole of b43 runs, and
`b43_validate_chipaccess` reads `UCODEREV` and `UCODEPATCH`. Without the strip
it would be served wl0's values.

**A capture taken at the bus.** The agcombo captures of `wl-mmio-trap`
(`router-data/agcombo/*.bin`) serve as oracle once decoded, and carry what a
wl-diag segment cannot: the MAC registers (`REG.RD`, served by offset), the
wrapper's IOST and every shared-memory space by its `sel=`. They need no
strip, since the trap sees one core:

```sh
python3 ../../reverse-tools/mmio2ops.py ../../router-data/agcombo/ch36.bin \
    --keep-flush -o /tmp/ch36.m2o
python3 ../../reverse-tools/timeline.py /tmp/ch36.m2o /tmp/ch36.tl
B43_BOARD=agcombo B43_READ_ORACLE=/tmp/ch36.m2o B43_CHANNEL=36 B43_BW=80 \
    B43_TIMELINE=/tmp/ch36.tl B43_TRACE_OUT=/tmp/int36.trace ./b43-trace
```

`ch36` is a cold attach, the trap armed about 160 operations after it began;
`ch100` and `ch149-wep` are hot ups and read no `UCODEREV`, so the probe of a
run on those channels takes its reads from `ch36`. Each file ends with a
down.

**The output.**

- `TRACE_OUT` writes the trace to a file (`B43_TRACE_OUT` for the binary);
  without it, the trace goes to stdout mixed with make's output.
- `make run` also writes `TRACE_OUT.fn`, the same trace with function markers.
- The `b43: ...` lines (`b43info`, `b43err`, the ones that say which gate
  rejected the probe) go to stderr.

**Configuration.** `B43_CHANNEL` and `B43_BW` choose it. They are the same
parameters `../unit` takes as `AC_CHANNEL` and `AC_BW`; `B43_BOARD` picks the
board profile of `../board_profile.h` (chip, chip revision, PCI device, SROM),
as the unit harness's argument does:

```sh
make run ORACLE=/tmp/m05 B43_CHANNEL=52 B43_BW=20 TRACE_OUT=/tmp/t
```

The channel is looked up among those b43 registered in `b43_setup_bands()`,
the same restriction mac80211 applies.

**The environment.** After `start` the suite does what mac80211 does for an AP
interface: `add_interface`, `config(~0)`, the four default `conf_tx`, then
`start_ap` -- on a radar channel after the availability check, with the
release and the retune in between. What follows comes from the capture's
timeline, `B43_TIMELINE`, the file `reverse-tools/timeline.py` writes
(`../unit/gates.sh` leaves it in `GATE_TMP`):

```sh
python3 ../../reverse-tools/timeline.py /tmp/m01 /tmp/tl01
make run ORACLE=/tmp/m01 B43_TIMELINE=/tmp/tl01 TRACE_OUT=/tmp/int.trace
```

The driver's timers are jiffies, advanced only by the timeline: before each
event the delayed works that are due run -- the periodic work, the radar poll.
The vendor's watchdog turns set the scale, one second each, so b43's own
periodic work produces the turns. `NOISE` raises `B43_IRQ_NOISESAMPLE_OK`
through b43's interrupt handlers, `TPL` is `bss_info_changed(BEACON)`, and
`BSS_UP` ends the check. Without a timeline the run stops after `start_ap`.

### Comparing

Use the `../unit` tools with the **`--bus` profile**, and the window that
`../unit/gates.sh` derives for the segment. The window runs from the AC attach's
first `PHY.RD 0x0739` to the last operation, `167:38445` for `cold01`:

```sh
python3 ../unit/cmp_skip.py /tmp/m01 /tmp/int.trace 167:38445 --board d6220 --bus
python3 ../unit/compare.py /tmp/m01 /tmp/int.trace --auto-align --bus
```

On `cold01`, with the timeline, this gives **84.29%** (28494/33804): 341 wrong
values, 1888 missing stock operations, 2740 extra port operations. The blocks
b43 emits elsewhere are moved first, each by its rule in `MOVED`.

**Why `--bus`.** This trace is taken at the MMIO bus, and the bus has none of
the accessor classes:

- a vendor `PHY.MOD` is a `PHY.RD` followed by a `PHY.WR` of the same register;
- a `TBL.WR` is just the words on the data port;
- a `MAC.MHF` is a software shadow.

`tracelib.unfold_bus()` rewrites the vendor side (and the `../unit` harness,
which has the same classes) into that vocabulary. The `RD` carries no value,
because the vendor does not record what a MOD read, and the `WR` is constrained
to the mask's bits. `cmp_skip.py --bus` aligns on (class, register) and judges
values inside the aligned blocks. Without `--bus` the comparison breaks at the
first maskset.

Three details of this measurement weigh more than the driver:

- **The vendor tracer records every radio maskset as a triple**: the `RAD.MOD`,
  its internal `RAD.RD` and its internal `RAD.WR`. On the bus the triple is the
  RD+WR pair with the real values, and `tracelib.unfold_bus_seq()` drops the
  MOD. `PHY.MOD`s have no shadow.
- **The oracle needs a queue slot for each `PHY.MOD`'s read.** The vendor does
  not record the value, but the driver's maskset really reads on the bus.
  Without the placeholder it consumes the next read's value. The placeholder
  returns the last known value of the register, read or written.
- **`MACCONTROL`** has no `RETVAL` in the captures: the tracer records the
  maskset's argument, not the readback. The stub keeps it as a latch.

### What the residual is made of

- **Stock operations b43 does not emit.**
  - The shared-memory configuration block the stock driver writes after the
    core attach and before touching the PHY.
  - The BSS configuration that `bss_info_changed` would write between the
    switch and the TX power adjust.
  - The four `conf_tx` passes.
  - The post-bring-up events (watchdog turns, radar polls, bss-up), which this
    suite does not deliver, because b43 has no wiring for them yet.
- **Port operations the stock driver does not emit.** These are the core's:
  the shared-memory clearing in `b43_upload_microcode`, the AMT, and the key
  material and index block that `src/` and `b43_security_init()` both write. See
  `docs/retrace-todo.md`.
- **LEDs.** Twelve of the missing operations are the stock driver's LEDs on the
  chipcommon, declared in `PERIMETER`. The `REG.WR 0x49c` with which `leds.c`
  drives them from the MAC are in `SOLO_PORT`: same function, different
  register, by b43's structure.

The regulatory ceiling is not a driver knob. `b43_phy_ac_reg_ceiling()` reads
the `max_power` cfg80211 applied to the channel. The D6220 has an empty `ccode`,
so `wl` runs its internal locale; `subsystem_stub.c` applies the same ceilings
at registration as a named table, `wl_default_locale_5g`.

## The stubs

| file | what it provides |
|---|---|
| `bcma_stub.c` | the bus; see the list below |
| `subsystem_stub.c` | mac80211/cfg80211; see the list below |
| `kernel_shim.c` | kernel services in user code |
| `compat.h` | macros of kernels newer than the headers (`kzalloc_obj`) |
| `trace_out.c` | trace output and `b43_test_env_long()` for the environment |

**`bcma_stub.c`** serves the bus:

- MMIO reads come from the oracle, and every read of a MAC register is in the
  trace as `REG.RD`;
- shared-memory accesses are reported at their byte offset, with a 32-bit
  access to SHARED split into the two 16-bit operations the vendor emits; the
  other spaces (the address match table) are one 32-bit operation per word,
  as at the bus;
- `MACCONTROL` is a latch;
- the ucode's `B43_IRQ_MAC_SUSPENDED` answers `b43_upload_microcode()`;
- the agent space is traced as `WRAP.RD/WR`: IOCTL and RESET_CTL are latches,
  RESET_ST is zero, IOST comes from a bus capture or is the D6220's;
- `clk_ctl_st` follows its requests: ALP and HT always available, the
  backplane on HT while FORCEHT is set.

`bcma_core_enable/disable`, `bcma_core_set_clockmode` and `bcma_core_pll_ctl`
are drivers/bcma/core.c itself, fetched with b43 at the same tag.

**`subsystem_stub.c`** stands in for mac80211 and cfg80211:

- the default channel is the first entry of the band b43 registered;
- `hw->conf.power_level` is the channel's `max_power`;
- work items run inline, since the suite is single-threaded;
- `request_firmware_nowait` completes inline, one blob per firmware type;
- the regulatory table.

**`kernel_shim.c`** includes `__sw_hweight32` as assembly with the kernel's
register contract. `KFLAGS` has `-mno-red-zone`, as the kernel does.

**Rule when a gate stops the probe.** Every value b43 reads is board data: take
it from the dumps in `router-data/`, do not invent it. An invented value makes
b43 take the wrong branch silently, the same failure mode as a missing oracle.

## Debugging

The normal build is `-O2`, and b43 inlines half the attach into
`b43_one_core_attach`, so a backtrace says little:

```sh
make clean && make DEBUG=1 b43-trace
B43_READ_ORACLE=/tmp/m01 gdb -batch -ex run -ex bt ./b43-trace
```

`DEBUG=1` adds `-fno-inline -g` and keeps `-O2`, which the kernel headers
require. Run `make clean` afterwards, or the `-fno-inline` objects stay around.

## What this suite answers and `../unit` does not

- **Which entry points b43 actually calls, how many times, and in which
  order.** For example, `switch_analog(dev, true)` is reached from four sites
  and three of them fire on one `ifconfig up`. That is why the cold preamble is
  guarded by `b43_phy_ac_cold_preamble_due()`.
- **Whether a core patch does what its mirror in `../unit/main.c` claims.**

## Open defect found here

**`do_full_init` does not tell cold from hot on b43.** `b43_phy_exit()`
(`phy_common.c`) sets it back to `true`, and `b43_chip_exit()` calls it on every
`ifconfig down`. On the b43 path the flag is therefore true on **every**
bring-up, and the comments in `src/phy_ac.c` that treat it as "cold attach only"
are wrong. Left as is: it depends on the deferred hot work.

## `b43_validate_chipaccess`: the stock driver runs the same test

Both drivers back up SHM `0x0000`/`0x0002`, write `0x55aa`/`0xaa55`, read back,
write the inverse, read back and restore. In the capture the sequence sits in
the attach preamble. It is also why the other core's attach must be stripped:
both cores run the test.
