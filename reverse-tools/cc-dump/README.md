# cc-dump

Reads the BCM4352/4360 **ChipCommon PMU** state from a running device and prints
it to `dmesg`. Purpose: capture the *vendor-initialised* `max_res_mask` /
`min_res_mask` so they can be compared against what our b43+bcma port writes in
`bcma_pmu_resources_init()` (patch 0007), which programs `max = 0x7ff`.

The vendor `si_pmu_res_init()` appears to *preserve* the ROM resource masks
rather than set a literal, so this tells us whether `0x7ff` merely confirms the
ROM value or overrides it. Confirmed on hardware: `max_res_mask` reads back
`0x7ff` on both a BCM4360 (agcombo) and a BCM4352 (DSL-3580L, see
`../../router-data/dsl3580l/dsl3580l_pmu-trace.txt`); `min_res_mask` reads
`0x7fb`. See `../wl-diag` for the tracer that produced the attach dump; this
module is deliberately tiny and uses raw `ioremap` (the stock firmware has no
bcma).

## Build

Same as wl-diag — out-of-tree against the device's kernel-3.4-rt tree:

    make KDIR=/path/to/kernel-3.4-rt ARCH=mips CROSS_COMPILE=mips-linux-gnu-

## Run (on the D6220, with vendor `wl` loaded and associated)

    insmod cc_dump.ko base=0x<chipcommon_phys>
    dmesg | tail -20
    rmmod cc_dump          # re-insmod to re-dump

## Run (on the DSL-3580L, 2.6.30)

Build with `KDIR` pointing at the 2.6.30 tree; the `pr_warn` shim compiles in
automatically. Here the radio is the PCIe device `0000:02:00.0`, whose BAR0 is
`0xa0000000` per the device's `/proc/iomem` (`a0000000-a01fffff : 0000:02:00.0`),
and the host is big-endian:

    insmod cc_dump.ko base=0xa0000000 bswap=1
    dmesg | tail -20

Confirm `chipid` reads back as `0x4352xxxx`; if garbled, drop `bswap` or adjust
`base` (ChipCommon may sit behind the backplane window rather than at BAR0+0).

### Finding `base`

`base` must be the physical address where the **AC radio's** ChipCommon core is
mapped — not necessarily the host SoC's.

- Integrated SI core: often the SI enum base `0x18000000` (the default). Check
  `/proc/iomem`.
- Discrete BCM4360 on PCIe: use the device's BAR0 physical address
  (`lspci -v`, or `/sys/bus/pci/devices/<dev>/resource`); ChipCommon is at
  `BAR0 + 0` when the backplane window points at core 0. Pass `base=<BAR0>`.

Sanity check: after load, `chipid` in dmesg should contain `0x4360`/`0x4352`. If
it is garbled or byte-swapped, adjust `base`, or set `bswap=1` on a big-endian
host.

## Parameters

- `base` (default `0x18000000`) — ChipCommon phys addr.
- `len` (default `0x1000`) — ioremap length.
- `bswap` (default `0`) — byte-swap accesses (big-endian host).
- `indirect` (default `0`) — also walk the res-dep / regctl / pllctl indirect
  tables. **This writes the shared `*_ADDR` select ports**, so only enable it
  when the chip is idle (not mid-operation under `wl`). The res-dep table shows
  which PMU resources exist, i.e. *why* the max mask would be `0x7ff`.
- `nres` / `nreg` / `npll` — number of indirect entries to walk (16 / 8 / 16).

## Workflow

1. `insmod cc_dump.ko base=0x…` on the vendor-initialised D6220 → note
   `max_res_mask` / `min_res_mask` (and, with `indirect=1`, the res-dep table).
2. Boot our b43+bcma driver → dmesg shows `PMU res mask pre-write: min=… max=…`
   (the prelog from patch 0007) — the value the chip holds before our write.
3. Compare: the vendor `max_res_mask` reads `0x7ff`, which matches the driver's
   `max_msk`, so that write is confirmed (not an override). `min_res_mask` reads
   `0x7fb`; the driver leaves `min_msk = 0` (no write), relying on the ROM value
   already in the register at bring-up — check the pre-write prelog to confirm
   it is `0x7fb` there too.

Direct-register reads are side-effect-free; only `indirect=1` writes anything.
