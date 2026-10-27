# wl_mmio_trap, ARM variant (kernel 4.4, Northstar)

The ARM build of the MMIO trap, for the Broadcom Northstar routers (BCM4709,
Cortex-A9, 2-level MMU, little-endian). It traces the register accesses
`wl-diag` cannot see -- the inline `readl`/`writel` and the `R_REG`/`W_REG`
loads and stores through `wl`'s own register pointer -- by faulting them. The
mechanism and the record format are the parent directory's; see `../README.md`
for the capture workflow and the decoder. Only what differs on ARM is here.

Nothing here has run on Northstar hardware; it is built against v4.4 and the
load/store emulator is checked natively (`tools/`). The self-test (below) is
the first thing to read on the device.

## How the hook differs

The MIPS build hangs a `break` off `fixup_exception()` because its
`do_page_fault()` notifies nobody. ARM has no such gap: the data abort runs
`__do_kernel_fault()` and `fixup_exception()` straight, so there is nothing to
divert there. Instead this build takes the dispatch itself.

`do_DataAbort()` routes a fault by its FSR code through `fsr_info[fsr_fs(fsr)].fn`.
Clearing a pte's `L_PTE_PRESENT`/`L_PTE_VALID` while keeping its PFN makes an
access raise FSR 7, "page translation fault", whose row is `fsr_info[7]`. The
module swaps that row's `fn` for its own: a fault in the window is decoded and
emulated through the module's uncached alias and resumed (return 0); any other
fault chains to the handler the slot held. No kprobes and no breakpoint -- so,
unlike the MIPS build, this one does **not** depend on `CONFIG_KPROBES` being
off. It does depend on the 2-level MMU: `CONFIG_ARM_LPAE` renumbers the fault
codes and is refused at load.

The alias is a plain `ioremap()`, which is uncached device memory on ARM, in
place of the MIPS `CKSEG1ADDR`; there is no 512 MB reach limit, so no
`redirect=` path.

## Resolving fsr_info

`fsr_info` is a static array, so its symbol is in kallsyms only with
`CONFIG_KALLSYMS_ALL`. With it, the module finds the slot itself. Without it,
pass the slot's address:

```
grep ' fsr_info$' System.map          # base of the array
# fsr_info[7].fn = base + 7 * sizeof(struct fsr_info); the struct is
# { fn; sig; code; name; } = 16 bytes on a 32-bit build, so + 0x70.
insmod wl_mmio_trap.ko fsr_info_fn7=0x<base + 0x70>
```

The module checks the slot before using it only in that it must resolve;
getting the offset wrong points the trap at the wrong handler, so prefer
`CONFIG_KALLSYMS_ALL` where the build allows it.

## Self-test

At load, before `wl`'s window is touched, the module traps, decodes and
resumes a load it issues itself on a fresh `vmalloc` page, through the same
pte-invalidation and alias path the real window uses. It must print

```
wl_mmio_trap: selftest passed: trapped, decoded, resumed
```

A failure there means the pte invalidation, the TLB flush or the dispatch
hook is not effective on this build, and the module refuses to load without
having touched the device. An Oops at `insmod` means the dispatch slot was
wrong (see above).

## Build and host checks

```sh
make KDIR=/path/to/kernel-4.4 ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j
make -C tools check    # the ARM load/store emulator, natively
```

Kernel configuration, in the Makefile's head: `CONFIG_KALLSYMS=y` (and
`KALLSYMS_ALL` to skip `fsr_info_fn7`), `CONFIG_PCI=y` (or `win_phys=`), not
`CONFIG_ARM_LPAE`, not `CONFIG_THUMB2_KERNEL` (the emulator decodes A32; the
Northstar `wl` is A32). `win_find.c` and `wl_ring.c` are shared with the MIPS
build one level up, compiled through the one-line `*_shared.c` wrappers.

## Capture

The workflow is `../README.md`'s, with these differences:

- load it once `wl` is up and the interface has been brought up at least
  once, so the BAR is mapped; this build does not watch `wl`'s `ioremap` the
  way the MIPS one can, it polls `init_mm` for the mapping at load and gives
  up after `poll_secs`;
- `autoarm`, `on`/`off`, `mark <label>` and `status` on `/proc/wl_mmio_trap`
  work as on MIPS; `redirect`, `klookup`, `init_mm_addr` and the `dd_*` TX
  descriptor parameters do not exist here.

```sh
insmod wl_mmio_trap.ko autoarm=0     # dry run: the self-test must pass
cat /proc/wl_mmio_trap | nc <host> <port> &
echo "mark ifup" > /proc/wl_mmio_trap
echo on          > /proc/wl_mmio_trap
ip link set <if> up
echo off         > /proc/wl_mmio_trap
echo status      > /proc/wl_mmio_trap; dmesg | tail -1
```

Decode on the host with `python3 ../decode-wl-mmio.py < trace.bin`.

## Not yet here

- **TX descriptors.** The MIPS build reads the DMA descriptors and buffers at
  a TX index write (`dma_dd.c`, the `dd_*` parameters). This version records
  register accesses only.
- **Thumb.** The emulator decodes A32. A Thumb faulting instruction is
  refused by status, not mis-decoded, so the module says so rather than
  guessing.
- Every access is an exception, as on MIPS, so `wl`'s `SPINWAIT` loops can
  time out under the trap: arm after the phase that times out, or split the
  run on marks.
