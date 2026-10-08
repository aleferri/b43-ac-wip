# wl_mmio_trap

Traces the register accesses `wl-diag` cannot see: the inline `R_REG`/`W_REG`
loads and stores through `wl`'s own register pointer, which have no function
to hook. It makes `wl`'s register window fault on every access and emulates
the trapped instruction. It finds the window by itself, so it can be loaded
before `wl`, after it, or between two `ifconfig up`. The agcombo captures in
`router-data/agcombo/*.bin` were taken with it.

## How it works

The kernel facts below were checked against the v3.4 sources; the 2.6.30
differences are listed under "Kernel 2.6.30".

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
`CONFIG_PCI=y`, not `CONFIG_KPROBES`. The same sources build for 3.4 and for
2.6.30, see below. `tools/` compiles the real encoders, emulator, record ring
and TX descriptor walker against `tools/shim/` and tests them natively; it
says nothing about a real exception.

## Kernel 2.6.30

The DSL-3580L runs 2.6.30; point `KDIR` at that tree with
`CROSS_COMPILE=mips-linux-`. `compat.h` holds the version tests. What differs:

- **`kallsyms_lookup_name` is not exported** (it is from 2.6.33). Pass its
  address as `klookup=0x...`, from `/proc/kallsyms`; the module calls it to
  resolve `fixup_exception`, the icache and TLB flushers and
  `__compute_return_epc`. Without it the module refuses to load.
- **There is no `DIE_BREAK`** (2.6.36). `do_bp()` reaches the die chain
  through `do_trap_or_bp()` as `DIE_TRAP`, and the notifier matches on the
  break's address as before. The break code is 515 where the kernel does not
  define `BRK_KPROBE_BP`.
- **`__compute_return_epc_for_insn` does not exist** (3.3). The unexported
  `__compute_return_epc()` is used instead; it reads the branch at `cp0_epc`,
  which is the word the emulator has just fetched.
- `init_mm` is exported by the 2.6.30 sources, but the module does not link
  against it: the vendor kernel may not export it. If the scan does not find
  it, pass `init_mm_addr=` from `System.map`.

On the DSL-3580L the radio is `0000:02:00.0` with BAR0 at `0xa0000000`, above
the 512 MB that `__ioremap()` reaches through CKSEG1, so `wl` maps it with
ptes and `redirect=1` is not needed.

```sh
grep ' kallsyms_lookup_name$' /proc/kallsyms
insmod wl_mmio_trap.ko autoarm=0 klookup=0x<address>
```

The build was checked against the vanilla 2.6.30 headers; it has not been run
on the router's own kernel. Before loading, look in `/proc/kallsyms` for
`fixup_exception`, `r4k_flush_icache_range`, `flush_tlb_kernel_range` and
`__compute_return_epc`.

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

### TX descriptors

The d11 TX header of a frame and the DMA descriptor that points at it never
cross the window: the engine reads them from memory. The write of a TX
channel's index does cross it, and at that write the module reads, out of
memory, the descriptors posted since the previous index and the start of
each buffer (`dma_dd.c`). Off by default:

```sh
echo 168 > /sys/module/wl_mmio_trap/parameters/dd_len     # bytes per buffer
echo 256 > /sys/module/wl_mmio_trap/parameters/dd_budget  # descriptors to record
```

168 bytes cover the 4-byte TX offload header, the 124-byte header and an
802.11 header. `dd_budget` counts down per descriptor and is written again
for more; with the rings tracked all along, switching on mid-traffic records
only what is posted from then on. A ring set up before the trap was engaged
has its address register read back at the first index write, and that
first write records only the slot before it (`guess`).

- Channel `n` is at window offset `0x200 + 0x40 * n`: index `+0x04`, ring
  address `+0x08`. Those offsets are the d11 core's only while the window
  points at it; a write there to another core is taken for a post and reads
  nothing unless it points at RAM and at something shaped like a descriptor.
- The index is the bus address of the descriptor after the last posted
  (an offset into the ring below `0x2000`), and the walk wraps at the
  descriptor with `EOT`. The descriptor words are taken in whichever byte
  order gives a valid byte count (`be` when big-endian).
- A bus address is taken as the physical one minus `dd_bus_off` (0, the
  BCM63xx PCIe inbound window's identity), and read uncached through CKSEG1
  only inside RAM below 512 MB. **SALAME**: that the identity holds on the
  DSL-3580L's BCM63168 is not checked; a wrong offset reads RAM that is not
  the frame, which the decoder shows as a layout it does not recognise.
- Reading happens inside the trapped write, after `wl` has written its
  caches back to post the frame, so it is what the engine reads.

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
`--base`, `--hex` for the raw bytes of the TX buffers). It recognises the
d11 AC TX header, with or without the TX offload header in front, from the
`frame_len` that matches the descriptor's byte count, and prints its fields,
the rate blocks with their PHY TX control words, the PLCP read as L-SIG,
HT-SIG or VHT-SIG-A by the frame type, and the 802.11 header. For register-level decoding give the binary to
`../reverse-tools/mmio2ops.py`, which reads it directly. The offsets are the
CPU side; on this big-endian host a 16-bit register is at `offset ^ 2`, which
mmio2ops applies.

Record: `struct wl_mmio_rec`, packed, `u64 ts_ns; u32 seq; u32 addr; u32 val;
u32 aux; u8 op; u8 cpu; u16 pad`. `op` is 60 read, 61 write, 62 mark (label in
`addr`/`val`/`aux`), 63 TX descriptor (`addr` its bus address, `val` the
buffer's, `aux` channel in 31:24, flags in 23:16, buffer bytes in 15:0), 64
data (twelve bytes in `addr`/`val`/`aux`: the descriptor's 16 as in memory,
then the buffer's, queued right after their 63), 255 drop count (in `aux`);
these numbers are free in `wl_diag`'s enum, so the streams can be merged. `addr` is the offset into the
window; `aux` carries the width in its low byte and bit 8 for a delay-slot
access.

## Limitations

- `lwl`/`lwr`/`swl`/`swr` are counted and refused.
- A delay slot behind a COP1 condition branch or `bposge32` is refused.
- The window is at least a page, and `wl` repoints it at attach and on every
  chanspec change: accesses to another core land here as plain offsets, and
  `wl_diag`'s `SI.COREREG` records say which core it was.
