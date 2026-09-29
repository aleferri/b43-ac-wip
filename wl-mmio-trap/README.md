# wl_mmio_trap

Traces the register accesses `wl-diag` cannot see: the inline `R_REG`/`W_REG`
loads and stores through `wl`'s own register pointer, which have no function
to hook. It makes `wl`'s register window fault on every access and emulates
the trapped instruction. It finds the window by itself, so it can be loaded
before `wl`, after it, or between two `ifconfig up`. The agcombo captures in
`router-data/agcombo/*.bin` were taken with it.

## How it works

The kernel facts below were checked against the v3.4 sources.

1. **The hook is a `break` at the first word of `fixup_exception()`.** On this
   build `do_page_fault()` notifies nobody (`notify_die(DIE_PAGE_FAULT)` is under
   `CONFIG_KPROBES`, off), and a fault that reaches `die()` cannot be resumed.
   `do_bp()` notifies `DIE_BREAK` unconditionally and returns on `NOTIFY_STOP`.
   `fixup_exception()` is reached only from `no_context`, so it taxes no
   userspace fault, and at its first word no frame is pushed: to make it
   return 1 the handler sets `v0` and jumps to `$ra`. A single aligned store has
   no intermediate state, so no `stop_machine()`. The displaced word is replayed
   from a `kmalloc`ed trampoline (same 256 MB region as the text) for faults
   that are not ours, so it must not be a branch.
2. **Finding the window.** The PCI layer gives the BAR's physical address; a
   walk of the kernel page tables over the vmalloc range finds the pte that maps
   it, repeated from a poll until `wl` has ioremapped it (`vmlist` is not
   exported on 3.4).
3. **Making it fault.** Every page of the mapping loses `_PAGE_VALID` and
   `_PAGE_PRESENT` and keeps its PFN and `_PAGE_GLOBAL`: the MIPS TLB ANDs the
   G bits of an even/odd pair, so dropping G would make the buddy page fault
   from other contexts, and a still-"present" pte would livelock in
   `vmalloc_fault`.
4. **Emulating.** The handler decodes the instruction (`lb/lbu/lh/lhu/lw/sb/sh/sw`),
   computes the resume PC first (a linking branch writes `$ra` before its delay
   slot; `__compute_return_epc_for_insn()` is exported), performs the access with
   `__raw_read/write` through its own uncached alias of the same pages, and
   patches the GPR. `wl`'s instruction never runs, so the access happens inside
   the exception at the address `wl` last pointed the window to, and another
   CPU's accesses are never let through unseen. An alias writes nothing to the
   device; what once crashed the DSL-3580L was `cc_dump` moving the window, not
   a second mapping.

`init_mm`, `flush_icache_range` and `flush_tlb_kernel_range` are not exported on
3.4 and are resolved by name (`kern_syms.c`). **SALAME**: the design assumes
`si_corereg` moves the window through PCI config space and that `wl` serialises
its own window moves, as bcma/b43 do; neither was disassembled from the blob.

## Build and host checks

```sh
make KDIR=/path/to/kernel-3.4-rt ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j
make -C tools check
```

Same `KDIR` requirements as `wl-diag`; needs `CONFIG_KALLSYMS=y` and
`CONFIG_PCI=y`, not `CONFIG_KPROBES`. 3.4 only: the 2.6.30 tree does not export
`kallsyms_lookup_name`. `tools/` compiles the real encoders and emulator against
`tools/shim/` and tests them natively; it says nothing about a real exception.

## Capture

```sh
insmod wl_mmio_trap.ko autoarm=0     # dry run: the self-test must pass
modprobe wl                          # "window found, autoarm=0: waiting for 'on'"
cat /proc/wl_mmio_trap > /tmp/mmio.bin &   # or | nc HOST PORT, on the wired LAN
echo "mark ifup" > /proc/wl_mmio_trap
echo on          > /proc/wl_mmio_trap
ifconfig wlan1 up
echo off         > /proc/wl_mmio_trap
echo "mark done" > /proc/wl_mmio_trap
echo status      > /proc/wl_mmio_trap; dmesg | tail -1
```

- If the self-test line is missing, the module refused to arm and nothing was
  patched. An Oops in the `insmod` means the breakpoint route is wrong on that
  build.
- If the device is not found, the module lists every PCI function with its BAR;
  a window that is not a PCI function is passed as `win_phys=`/`win_phys_len=`.
- `drops=` non-zero means the queue overflowed. A bring-up burst runs at
  150–230k accesses/s on a BCM63168 and no reader keeps up, so size `fifo_recs`
  for the whole burst. By default the trap stops at the first overflow
  (`stop_on_full=0` disables it). `unhandled:` counters that move mean refused
  accesses, each a real fault.
- Every access is an exception, so `wl`'s `SPINWAIT` loops can time out: arm
  after the phase that times out, or read the failed attach anyway, or split
  the run on several marks.
- Tear down with `off`, the reader killed, `ifconfig down`, `rmmod wl`,
  `rmmod wl_mmio_trap`, which restores the ptes and the breakpoint.

It can run alongside `wl_diag`: each `DIE_BREAK` handler ignores addresses that
are not its own, and both stamp records with `sched_clock()`.

### A window in KSEG1

`__ioremap()` returns a bare `CKSEG1ADDR`, with no pte to trap, for an uncached
mapping below 512 MB of physical space, and the BMIPS4350 has no watch
registers. Its shortcut tests `flags == _CACHE_UNCACHED`, so asking for
`_CACHE_UNCACHED | _PAGE_GLOBAL` takes the mapping path and yields a real,
still uncached pte. `ioremap_watch=1` only logs the driver's `__ioremap()`
calls; `redirect=1 pci_device=0x4360 init_mm_addr=0x...` answers the matching
call with that mapping, built from the COMING notifier, so `wl` must be loaded
after the module. `iounmap()` of the redirected window is refused while the
module holds its ptes. Off by default.

## Reading the capture

`decode-wl-mmio.py` is a stdin filter (`--since LABEL`, `--until LABEL`,
`--base`). For register-level decoding give the binary to
`../reverse-tools/mmio2ops.py`, which reads it directly. The offsets are the
CPU side; on this big-endian host a 16-bit register is at `offset ^ 2`, which
mmio2ops applies.

Record: `struct wl_mmio_rec`, packed, `u64 ts_ns; u32 seq; u32 addr; u32 val;
u32 aux; u8 op; u8 cpu; u16 pad`. `op` is 60 read, 61 write, 62 mark (label in
`addr`/`val`/`aux`), 255 drop count (in `aux`); these numbers are free in
`wl_diag`'s enum, so the streams can be merged. `addr` is the offset into the
window; `aux` carries the width in its low byte and bit 8 for a delay-slot
access.

## Limitations

- `lwl`/`lwr`/`swl`/`swr` are counted and refused.
- A delay slot behind a COP1 condition branch or `bposge32` is refused.
- The window is at least a page, and `wl` repoints it at attach and on every
  chanspec change: accesses to another core land here as plain offsets, and
  `wl_diag`'s `SI.COREREG` records say which core it was.
