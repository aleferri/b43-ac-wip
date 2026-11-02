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

## Resolving init_mm

The window's ptes are found by walking `init_mm`'s page tables. `init_mm` is
not exported, and, being data, it is in kallsyms only with
`CONFIG_KALLSYMS_ALL`. `init_task.active_mm` is not a way in: `init_task`
is CPU 0's idle thread, which borrows the previous task's mm on every switch
(lazy TLB), and a user pgd lacks the L1 entry of a fresh `vmalloc` page, so
the self-test stops at "no simple pte behind a fresh vmalloc page". Without
`KALLSYMS_ALL`, pass the address from `System.map`:

```
grep ' init_mm$' System.map
insmod wl_mmio_trap.ko init_mm_addr=0x<address> ...
```

The module refuses to load with neither, and with both when they differ.
Before following the address it checks that it lies in the linear map, that
its `pgd` does too, and that the `pgd`'s L1 entry for `PAGE_OFFSET` is the
one the insmod process's own `pgd` carries (read from TTBR0, so the check
does not depend on the layout of `task_struct`), since every `pgd` copies
the kernel half of `init_mm`'s.

## A stock DD-WRT firmware, without its System.map

Taken on an RT-AC66U B1 with DD-WRT r66396 (`4.4.302-rt232-st79`, built
2026-10-24), whose own build tree and System.map are not published:

- **Kernel tree**: `src/linux/universal/linux-4.4` of the DD-WRT sources with
  `.config_northstar_smp`, `olddefconfig` and `modules_prepare` (the
  `rt3352`, `rt5592`, `rt7612` and `rt7620` Kconfig sources it names live
  outside the public tree; empty files in their place). DD-WRT's modules
  carry no `vermagic` and no CRCs, and the kernel checks neither; its
  `struct module` (`.gnu.linkonce.this_module`, 0x160 bytes) was the same
  in this build and in the firmware's modules.
- **printk**: that tree's Makefile builds every module with
  `-DCONFIG_NOPRINTK`, which leaves the module silent. Build with
  `make ... KBUILD_CFLAGS_MODULE=-DMODULE`.
- **fsr_info**: `do_DataAbort` (in `/proc/kallsyms`) loads the table's
  address with a `movw`/`movt` pair; its text is read through `/dev/mem`
  (Kernel code in `/proc/iomem`, physical = virtual − `PAGE_OFFSET`,
  0x80000000 on this VMSPLIT_2G build). `do_PrefetchAbort` loads
  `ifsr_info` the same way, whose row 7 is `do_page_fault` too, so take
  `do_DataAbort`'s: `fsr_info_fn7` = that + 0x70. A cross-check: in
  `fsr_info` row 1 is `do_alignment` (SIGBUS), row 2 SIGKILL, row 4 and 5
  `do_translation_fault`.
- **init_mm**: the word in kernel data that holds `swapper_pg_dir`
  (`PAGE_OFFSET` + 0x4000) at `offsetof(struct mm_struct, pgd)` (36 in
  that tree), with `mm_users` 2 and `mm_count` 1 after it; one match.

```sh
insmod wl_mmio_trap.ko autoarm=0 win_phys=0x08000000 win_phys_len=0x8000 \
    fsr_info_fn7=0x805702cc init_mm_addr=0x80576c68 fifo_recs=2097152 poll_secs=10
```

(r66396 on the RT-AC66U B1; `win_phys=0x20000000` for `eth2`.)

## Self-test

At load, before `wl`'s window is touched, the module traps, decodes and
resumes a load it issues itself on a fresh `vmalloc` page, through the same
pte-invalidation and alias path the real window uses (the test page's alias
is a second `vmap()` of it: ARM's `ioremap()` refuses RAM). It must print

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

The window's ptes are written with the processor's `set_pte_ext`, not
`set_pte_at`: the latter references `__sync_icache_dcache`, which the kernel
does not export (it is for user exec pages, a branch a kernel address never
takes), so a module calling `set_pte_at` fails `modpost`. `arm_syms` resolves
`cpu_v7_set_pte_ext` by name, like `flush_tlb_kernel_range`, which works on
both the single-CPU DD-WRT build and a `CONFIG_MULTI_CPU` one.

## Capture

The workflow is `../README.md`'s, with these differences:

- load it once `wl` is up and the interface has been brought up at least
  once, so the BAR is mapped; this build does not watch `wl`'s `ioremap` the
  way the MIPS one can, it polls `init_mm` for the mapping at load and gives
  up after `poll_secs`;
- `autoarm`, `on`/`off`, `mark <label>` and `status` on `/proc/wl_mmio_trap`
  work as on MIPS, and so do the `dd_*` TX descriptor parameters (below);
  `redirect` and `klookup` do not exist here; `init_mm_addr` does (above).

```sh
insmod wl_mmio_trap.ko autoarm=0     # dry run: the self-test must pass
cat /proc/wl_mmio_trap | nc <host> <port> &
echo "mark ifup" > /proc/wl_mmio_trap
echo on          > /proc/wl_mmio_trap
ip link set <if> up
echo off         > /proc/wl_mmio_trap
echo status      > /proc/wl_mmio_trap; dmesg | tail -1
```

Decode on the host with `python3 ../decode-wl-mmio.py < trace.bin`, or into
op classes with `../../reverse-tools/mmio2ops.py trace.bin`.

The records carry the offset the device sees: on this little-endian host a
16-bit access reaches the half it names, where the MIPS records carry the
CPU side and mmio2ops moves 16-bit accesses to `offset ^ 2`. Each access
record sets `0x200` in `aux` to tell the two apart. Captures taken before
that flag (`router-data/r7000/mmio-*.zip`) need
`mmio2ops.py --format wl-mmio-trap-arm`.

`/proc/wl_mmio_trap` is a byte stream, so any reader works (busybox `cat`,
`dd` with any block size); a reader stopped in the middle of a record loses
only that record, which the sequence numbers show, and the next reader
starts on a record.

## TX descriptors

Like the MIPS build, at a TX channel's index write the module walks the DMA
descriptors posted since the previous index out of RAM and records each with
up to `dd_len` bytes of its buffer (`dma_dd.c`, shared). Off until `dd_len`
is set:

```sh
echo 168 > /sys/module/wl_mmio_trap/parameters/dd_len
echo 256 > /sys/module/wl_mmio_trap/parameters/dd_budget
```

The one ARM difference is the memory read. The MIPS build reads RAM uncached
through `CKSEG1`; on ARM `ioremap()` refuses RAM, so the read is through the
cached linear map (`phys_to_virt`) after a clean+invalidate of the range
(`__cpuc_flush_dcache_area`), so it reflects what `wl` wrote before posting
the frame. A frame buffer in highmem has no linear address and is refused
(the descriptor is still recorded, without its buffer); the ring and the
descriptors are in lowmem. On a board whose DMA bus addresses are not the
identity of the physical ones, pass `dd_bus_off=`.

**SALAME**: on a Cortex-A9 with an L2C-310 outer cache, cleaning the inner
cache alone does not prove the descriptor reached DRAM; `wl` flushes it for
DMA before the index write, so it is there by the time this runs, but that
ordering is assumed from how a DMA driver must behave, not read from the
blob.

## Not yet here

- **Thumb.** The emulator decodes A32. A Thumb faulting instruction is
  refused by status, not mis-decoded, so the module says so rather than
  guessing.
- Every access is an exception, as on MIPS, so `wl`'s `SPINWAIT` loops can
  time out under the trap: arm after the phase that times out, or split the
  run on marks.
