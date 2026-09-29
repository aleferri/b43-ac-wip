# cc-dump

Reads the BCM4352/4360 **ChipCommon PMU** state from a running device and
prints it to `dmesg`. It captures the vendor-initialised `max_res_mask` /
`min_res_mask`, to compare with what the b43+bcma port programs in
`bcma_pmu_resources_init()` (`patches/0007`, `max = 0x7ff`).

**Results on hardware.**

- `max_res_mask` reads back `0x7ff` on a BCM4360 (agcombo) and a BCM4352
  (DSL-3580L, `../router-data/dsl3580l/dsl3580l_pmu-trace.txt`).
- `min_res_mask` reads `0x7fb`.
- The port's `max` write therefore confirms the ROM value rather than
  overriding it.

The module is deliberately tiny and uses raw `ioremap`: the stock firmware has
no bcma.

## Build

Same as wl-diag, out of tree against the device's kernel:

    make KDIR=/path/to/kernel-3.4-rt ARCH=mips CROSS_COMPILE=mips-linux-gnu-

For the DSL-3580L, point `KDIR` at the 2.6.30 tree; the `pr_warn` shim compiles
in automatically.

## Run

On the D6220, with the stock `wl` loaded and associated:

    insmod cc_dump.ko base=0x<chipcommon_phys>
    dmesg | tail -20
    rmmod cc_dump          # re-insmod to dump again

On the DSL-3580L the radio is PCIe `0000:02:00.0`, BAR0 `0xa0000000`
(`/proc/iomem`), and the host is big-endian:

    insmod cc_dump.ko base=0xa0000000 bswap=1

`chipid` must read back as `0x4352xxxx` / `0x4360xxxx`. If it is garbled, adjust
`base` or `bswap`: ChipCommon may sit behind the backplane window rather than at
BAR0+0.

### Finding `base`

`base` is the physical address where the **AC radio's** ChipCommon is mapped,
not necessarily the host SoC's:

- **an integrated SI core** is often at the SI enumeration base `0x18000000`
  (the default); check `/proc/iomem`;
- **a discrete chip on PCIe** is at its BAR0 (`lspci -v`, or
  `/sys/bus/pci/devices/<dev>/resource`). ChipCommon sits at `BAR0 + 0` when the
  backplane window points at core 0.

## Parameters

- `base` (default `0x18000000`): ChipCommon physical address.
- `len` (default `0x1000`): ioremap length.
- `bswap` (default `0`): byte-swap accesses (big-endian host).
- `indirect` (default `0`): also walk the res-dep / regctl / pllctl indirect
  tables. **This writes the shared `*_ADDR` select ports**, so enable it only
  when the chip is idle. The res-dep table shows which PMU resources exist.
- `nres` / `nreg` / `npll`: number of indirect entries to walk (16 / 8 / 16).

Direct register reads have no side effects; only `indirect=1` writes.
