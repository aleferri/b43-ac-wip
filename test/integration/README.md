# test/integration — the whole of b43 on AC

Compiles real b43 (`main.c`, `phy_common.c`, `leds.c`, `rfkill.c` and the
rest, at the tag of the installed kernel headers) with `b43/` copied over it
and the ssb headers of `bcma/`, stubs the bus and subsystem layers, and runs the
probe, `ifconfig up` and the capture's timeline. The trace is taken at the
MMIO bus.

On `cold01` the probe and `start` return 0; the score is in the top-level
README.

## Running it

Prerequisites: `gcc`, `binutils`, `make`, `patch`, `curl`, `python3`, `unzip`
(`gdb` for backtraces); kernel headers in `/usr/src`
(`apt-get install linux-headers-generic`), which decide the b43 tag
(`6.8.0-142` → `v6.8`; with several installed, pass `KVER=` to every `make`);
network access to `raw.githubusercontent.com` for `make fetch`.

```sh
make fetch          # vanilla b43 at the headers' tag + patches/: "applicate 5, saltate 5"
make check          # every file of b43 and of the port: must say "0 errori"
make b43-trace      # expects "kernel 0" and only libc symbols unresolved
```

The five bcma/ssb patches have nothing to apply here except their
`include/linux/ssb/` hunks, which go into `kinc/`. If a patch fails, remove
`b43-upstream kinc` before retrying: a half-patched tree still compiles.

### A run against a cold segment

Without an oracle every read returns zero and the trace measures nothing. The
segment is stripped of the other core's attach (`b43_validate_chipaccess` would
otherwise get wl0's `UCODEREV`) and folded:

```sh
unzip -d /tmp/cold ../../router-data/d6220/cold-sweep.zip
python3 ../../reverse-tools/strip_other_core.py \
    /tmp/cold/cold01-ch36-bw20.txt /tmp/cold01-clean.txt
python3 ../../reverse-tools/trace_filter.py --retvals /tmp/cold01-clean.txt /tmp/m01
python3 ../../reverse-tools/timeline.py /tmp/m01 /tmp/tl01
make run ORACLE=/tmp/m01 B43_TIMELINE=/tmp/tl01 TRACE_OUT=/tmp/int.trace

python3 ../unit/cmp_skip.py /tmp/m01 /tmp/int.trace 167:38445 --board d6220 --bus \
    --min-block 8 --gap-tol 2
python3 ../unit/compare.py /tmp/m01 /tmp/int.trace --auto-align --bus
```

On this run b43 reads firmware version 43605.21930, `0xaa55.0x55aa`, not the
segment's 928. `b43_validate_chipaccess()` writes the two patterns in the
opposite order to `wl`, and the shared-memory model consumes a queued vendor
read only from the head of its queue, so one pattern read per cell stays
queued and `b43_upload_microcode()` gets it. The template layout is chosen
by that revision: with an unknown one the AP is refused and no beacon is
written. Dropping `#156` and `#158` from the oracle gives the driver 928.

`B43_CHANNEL`, `B43_BW` and `B43_BOARD` choose the configuration and the
profile of `../board_profile.h`; the channel must be one b43 registered.
`make run` also writes `TRACE_OUT.fn` with function markers, and the `b43: ...`
lines go to stderr. The window is the one `../unit/gates.sh` derives, from the
AC attach's first `PHY.RD 0x0739` to the last operation.

After `start` the suite does what mac80211 does for an AP (`add_interface`,
`config(~0)`, four `conf_tx`, `start_ap`, on a radar channel after the check
with the release and the retune in between), then plays the timeline. The
driver's timers are jiffies advanced only by the timeline, one second per
vendor watchdog turn; `NOISE` raises `B43_IRQ_NOISESAMPLE_OK`, `TPL` is
`bss_info_changed(BEACON)`, `BSS_UP` ends the check, `IRQ` raises the reason
the capture has in `GEN_IRQ_REASON` and the DMA channel 0 status the vendor
acknowledged (a bus capture only: the wl-diag ones have no MAC registers).
`dma.c` is compiled with `dma_stub.c` under it, so the interrupt handler
programs the rings, acknowledges the channels and, on `RX_DONE`, walks the
receive ring up to the slot the oracle reports; the buffers hold the poison
`dma.c` puts there, so every frame is dropped and recycled, which is the same
register traffic as a received frame with b43's ring geometry. Interrupts the
vendor took during its bring-up are delivered together when the environment
starts, since here the bring-up is one call.

**A capture taken at the bus.** The agcombo's `wl-mmio-trap` captures
(`router-data/agcombo/*.bin`) also carry the MAC registers, the wrapper and
every shared-memory space, and need no strip:

```sh
python3 ../../reverse-tools/mmio2ops.py ../../router-data/agcombo/ch36.bin \
    --keep-flush -o /tmp/ch36.m2o
python3 ../../reverse-tools/timeline.py /tmp/ch36.m2o /tmp/ch36.tl
B43_BOARD=agcombo B43_READ_ORACLE=/tmp/ch36.m2o B43_CHANNEL=36 B43_BW=80 \
    B43_TIMELINE=/tmp/ch36.tl B43_TRACE_OUT=/tmp/int36.trace ./b43-trace
python3 ../unit/cmp_skip.py /tmp/ch36.m2o /tmp/int36.trace \
    1:$(grep -c . /tmp/ch36.m2o) --board agcombo --bus --min-block 8 --gap-tol 2
```

With `B43_FW_DIR` the harness loads the ucode and the initvals from a
directory instead of the minimal blob, so that b43 writes the init lists the
stock driver writes. A bus capture of the stock `up` holds both:

```sh
python3 ../../reverse-tools/fw_from_capture.py \
    ../../router-data/agcombo/ch36.bin -o /tmp/fw-agcombo
B43_FW_DIR=/tmp/fw-agcombo B43_BOARD=agcombo ... ./b43-trace
```

None of the three holds the attach, which ran before the capture. `ch36` is
the first `up` after the module load; `ch100` and `ch149-wep` are later ups
and read no `UCODEREV`, so their probe takes its reads from `ch36`.

### Why `--bus`

The bus has none of the accessor classes: a `PHY.MOD` is a `PHY.RD` plus a
`PHY.WR`, a `TBL.WR` is words on the data port, a `MAC.MHF` is a software
shadow. `tracelib.unfold_bus()` rewrites the vendor side into that vocabulary
and `cmp_skip.py --bus` aligns on (class, register). Five details matter more
than the driver:

- the vendor tracer records every radio maskset as `RAD.MOD` plus its internal
  `RAD.RD` and `RAD.WR`, and `unfold_bus_seq()` drops the MOD;
- the oracle keeps a queue slot for each `PHY.MOD`'s read, returning the last
  known value, or the driver's maskset consumes the next read's value;
- `MACCONTROL` has no `RETVAL` in the captures, so the stub keeps it as a
  latch;
- the table-write gate, PHY `0x019e`, reads back the last value written on
  every capture, so after the driver's first write the oracle answers it
  from that value instead of its queue (`oracle_latch` in `trace_out.c`): a
  read more or less than the stock driver would otherwise shift every later
  read of the gate;
- reads of the table data ports (PHY `0x000f`-`0x0011`) are queued by the
  table address the read starts at, id in `0x000d` and offset in `0x000e`,
  not by port: a table the port reads and the stock driver does not would
  otherwise hand its values on to every table read after it.

The regulatory ceiling is not a knob: the D6220 has an empty `ccode`, and
`subsystem_stub.c` applies the same ceilings `wl`'s internal locale does as
`wl_default_locale_5g`.

## The stubs

| file | what it provides |
|---|---|
| `bcma_stub.c` | the bus: MMIO reads from the oracle (`REG.RD`), shared memory at its byte offset, `MACCONTROL` as a latch, the ucode's `MAC_SUSPENDED`, the agent space as `WRAP.RD/WR`, `clk_ctl_st` |
| `subsystem_stub.c` | mac80211/cfg80211: default channel, `power_level`, inline work items and firmware requests, the regulatory table |
| `kernel_shim.c` | kernel services in user code, `__sw_hweight32` with the kernel's register contract |
| `compat.h` | macros of kernels newer than the headers |
| `trace_out.c` | trace output and the environment |

`bcma_core_enable/disable`, `bcma_core_set_clockmode` and `bcma_core_pll_ctl`
are `drivers/bcma/core.c` itself, fetched with b43, and so are bcma's
ChipCommon, PMU and PCIe2 drivers (the port's `driver_chipcommon_pmu.c`),
which the harness runs before the probe as `bcma_bus_register()` does. Their
accesses go out as `CC.*` and `PCIE.*`; the ChipCommon reset words come from
`../board_profile.h` (`cc_caps`, `cc_capext`, `cc_chipstatus`, `pmu_caps`),
and with them at zero the PMU init does not run.

**When a gate stops the probe**, take the value b43 reads from the dumps in
`router-data/`; an invented value makes b43 take the wrong branch silently.

## Debugging

```sh
make clean && make DEBUG=1 b43-trace
B43_READ_ORACLE=/tmp/m01 gdb -batch -ex run -ex bt ./b43-trace
```

`DEBUG=1` adds `-fno-inline -g` and keeps the `-O2` the headers require; run
`make clean` afterwards.
