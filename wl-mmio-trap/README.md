# wl_mmio_trap

Standalone tool. No change to `wl_diag.c` or to anything else in
`b43-ac-wip`.

It traces the register accesses `wl_diag.c` cannot see: the inline
`R_REG`/`W_REG` loads and stores through wl's own register pointer, which
have no function call to hook. It does that by making wl's register window
fault on every access and decoding the trapped instruction, one at a time.

It finds the window by itself. There is nothing to look up beforehand and
no load order to respect: `insmod` before wl, after wl, or between two
`ifconfig up` cycles all work.

## Facts about the target kernel this is built on

Everything below was checked against the v3.4 sources, not assumed. The
first two rule out the obvious design and are the reason this package looks
the way it does.

- **`do_page_fault()` does not notify anybody unless `CONFIG_KPROBES` is
  on.** In `arch/mips/mm/fault.c` the `notify_die(DIE_PAGE_FAULT, ...)`
  call sits inside `#ifdef CONFIG_KPROBES`. `wl_diag.c`'s own header
  records that KPROBES is off on this build, so a `register_die_notifier()`
  on the page-fault path is never called at all.
- **The Oops path cannot be resumed from either.** A kernel fault that
  gets past `fixup_exception()` reaches `die()`, which is `__noreturn`:
  `NOTIFY_STOP` on `DIE_OOPS` only suppresses the signal.
- **`do_bp()` does notify, unconditionally.** In
  `arch/mips/kernel/traps.c` the `notify_die(DIE_BREAK, ...)` for
  `BRK_KPROBE_BP` is outside every `#ifdef`, and `NOTIFY_STOP` makes
  `do_bp()` return. That is the one instrumentation point on this kernel
  from which a handler can resume at an address it chooses.
  `wl_diag.c` already relies on it for prologues it cannot detour.
- **`__ioremap()` short-circuits low physical addresses to CKSEG1.** Any
  uncached mapping of something in the low 512 MB of physical space comes
  back as a bare `CKSEG1ADDR`, with no page table entry at all. On a board
  whose BAR sits below 512 MB this mechanism cannot work, and the module
  says so instead of arming half of it. (The `__IS_LOW512` shortcut in
  `asm/io.h` needs compile-time constant arguments, so it never fires for a
  driver passing `pci_resource_start()`; the one in `__ioremap()` itself
  always does.)
- **`__compute_return_epc_for_insn()` is `EXPORT_SYMBOL_GPL`.** Loads and
  stores in a branch delay slot are therefore handled, not just counted.
- **Three things a module needs here are not exported on 3.4:** `init_mm`
  (reached through `init_task`, which *is* exported), `flush_icache_range`
  (a `.bss` function pointer; the R4K back end behind it is resolved by
  name, same route `wl_diag.c` takes), and `flush_tlb_kernel_range` (also
  by name). `module_alloc` is not exported either, which is why the
  trampolines come from `kmalloc`, exactly as `wl_diag.c`'s stubs do.
- **Op numbers 60 and 61 are free** in `wl_diag.c`'s `enum wldiag_op`,
  which runs to `OP_IOVAR_SET = 54` plus `OP_DROP = 255`. The two streams
  can be merged by a decoder without renumbering.

## How it works

1. `fixup_exception()` gets a one-word `break BRK_KPROBE_BP`. It is the
   right place rather than `do_page_fault()` because it is only reached
   from `no_context`, so instrumenting it does not tax every userspace
   fault in the system. A single aligned 32-bit store has no intermediate
   state, so there is no patch-ordering problem and no `stop_machine()`.
   The displaced word is relocated into a three-word trampoline for the
   faults that turn out not to be ours.
2. The PCI layer gives the device's BAR: a physical address and a length.
   A walk of the kernel page tables over the vmalloc range gives the
   virtual address whose pte carries that physical page — which is also
   the check that the window is TLB-mapped at all. If wl has not
   ioremapped it yet, the search repeats until it appears.
3. Every page of that mapping is made permanently absent, keeping the PFN
   and `_PAGE_GLOBAL` (the MIPS TLB covers an even/odd page pair and ANDs
   their G bits, so dropping it would make the neighbouring page fault
   spuriously from other contexts).
4. On each fault the module decodes wl's instruction, performs the access
   itself through **its own alias** of the same physical pages, patches the
   GPR and the resume PC, and makes `fixup_exception()` return 1 without
   running. wl's own instruction never executes.

That last point is what removes the need for the old bracket around
`si_corereg` and friends. Nothing is ever split into "point the window" and
"use the window at some later time": the access happens inside the same
exception that trapped it, at the same physical address, and the window is
wherever wl last pointed it. It also closes a hole the per-access
pte-flipping design had: while one cpu made a page valid for its own
emulated access, the other cpu's access to that page went through unseen.

## Build

```
make KDIR=/path/to/kernel-3.4-rt ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j
```

Same `KDIR` requirements as `wl-diag`/`wl-cc-dump` (matching `.config`,
`Module.symvers`). Needs `CONFIG_KALLSYMS=y` — but not `KALLSYMS_ALL`, since
only text symbols are resolved by name — and `CONFIG_PCI=y` for the
discovery. Does **not** need `CONFIG_KPROBES`.

On the DSL-3580L's 2.6.30 tree `kallsyms_lookup_name` is not exported to
modules, the same fact `wl-diag/2-6-30`'s Makefile notes. This package
targets the 3.4.x tree only; the address-parameter fallback that module
uses is not ported here.

## Host-side checks

```
make -C tools check
```

Compiles the two pure-logic units — the instruction encoders and the
load/store decoder/emulator — natively, against `tools/shim/`, and runs
them. `tools/` builds the real files, not copies, so there is nothing to
drift.

`test_opcodes` checks every encoder against disassembler-derived machine
words, including the `break 515` encoding and the 256 MB region rule for
`j`. `test_emulate` drives the decoder: widths, sign extension, effective
address agreement, `$0` discarding, stores taking their value from `rt`,
`cp0_epc` advancing by one instruction on the straight path and coming
from the branch evaluator on the delay-slot path, and every refusal
leaving registers, `cp0_epc` and the device untouched.

Neither says anything about behaviour under a real exception.

## Capture, step by step

**0. Before anything.** Serial console attached, on a unit you can recover
without opening it. `wl` not yet loaded.

**1. Dry run, nothing armed.**

```
insmod wl_mmio_trap.ko autoarm=0
dmesg | tail -20
```

Four things have to be in `dmesg`, in this order:

```
wl_mmio_trap: r4k_flush_icache_range -> 8xxxxxxx
wl_mmio_trap: flush_tlb_kernel_range -> 8xxxxxxx
wl_mmio_trap: fixup_exception -> 8xxxxxxx, init_mm -> 8xxxxxxx
wl_mmio_trap: breakpoint armed at 8xxxxxxx (orig ........, trampoline 8xxxxxxx)
wl_mmio_trap: selftest passed: trapped, decoded, resumed
```

If the self-test line is missing the module has already refused to arm and
`insmod` has failed; nothing has been patched into wl. If instead the box
Oopses in the `insmod` process, the breakpoint route is wrong on this build
and the rest of the procedure does not apply — send the Oops.

**2. Let it find the window.**

```
modprobe wl
dmesg | grep wl_mmio_trap
```

Expect a BAR line, then the mapping line:

```
wl_mmio_trap: 0000:01:00.0 [14e4:43b3] BAR0 = a0000000 +8192
wl_mmio_trap: phys a0000000 is mapped at c1xxxxxx, 2/2 pages contiguous
wl_mmio_trap: window c1xxxxxx +8192 (phys a0000000), alias c1yyyyyy
wl_mmio_trap: window found, autoarm=0: waiting for 'on'
```

If the device is not found, the module dumps every PCI function the kernel
knows about with its BAR, so the failure says what *is* there. When the
radio is not a PCI function at all, read its window out of
`/proc/bus/pci/devices` (field 4 is BAR0's base, field 11 its size) or from
the driver's own boot messages and pass it in:

```
insmod wl_mmio_trap.ko autoarm=0 win_phys=0x18001000 win_phys_len=8192
```

`no mapping of the BAR appeared` after the poll window means either wl never
ioremapped it, or the window sits below 512 MB of physical space and came
back as a bare CKSEG1 address with no pte. The second is not recoverable
here: CKSEG1 is a fixed translation with no page table entry to invalidate.

**3. Start the reader before arming.** It blocks when the queue is empty, so
it stays attached for the whole run:

```
cat /proc/wl_mmio_trap > /tmp/mmio.bin &
READER=$!
```

On a box with little RAM, stream it off instead:

```
cat /proc/wl_mmio_trap | nc 192.168.1.100 9000 &
```

(and `nc -l -p 9000 > mmio.bin` on the other end). Use the wired LAN, not
the radio being traced.

**4. Arm around the phase you want, and label it.**

```
echo "mark ifup" > /proc/wl_mmio_trap
echo on         > /proc/wl_mmio_trap
ifconfig wlan1 up
echo off        > /proc/wl_mmio_trap
echo "mark done" > /proc/wl_mmio_trap
```

A `mark` is a record in the queue like any other, so it is ordered against
the accesses around it, not against the clock of the shell that wrote it.

**5. Check what it cost.**

```
echo status > /proc/wl_mmio_trap
dmesg | tail -1
```

`drops=` non-zero means the queue overflowed. That is not the reader being
slow in general: a bring-up burst runs at 150-230k accesses per second on a
BCM63168 and no reader drains that on a cpu already spending its cycles
taking exceptions. The queue has to absorb the whole burst. Two captures
from that board each lost everything between the moment the queue filled
and the moment the burst subsided -- in one case 110 793 records over 5.5
seconds, which is where the channel tuning was. Size `fifo_recs` for the
burst you expect: 5.5 s at 20k records/s is about 128k records, 3.5 MB.

By default the trap now stops the first time the queue overflows, so what
you get is one contiguous run and the driver goes back to full speed
instead of crawling while its accesses are thrown away. `stop_on_full=0`
restores the old behaviour.
`unhandled:` counters non-zero mean accesses this module refused to emulate
— each of those surfaced as a real fault, so if any of them moved, look for
an Oops in the same `dmesg`.

**6. Tear down, in this order.**

```
echo off > /proc/wl_mmio_trap
kill $READER
ifconfig wlan1 down
rmmod wl
rmmod wl_mmio_trap
```

`rmmod wl_mmio_trap` puts the ptes back and takes the breakpoint out by
itself, and the module's GOING notifier already released the window when
`wl` went, so the order above is belt and braces rather than a requirement.

### Expect the first attempt to time out

Every register access is a full exception plus a decode. wl's `SPINWAIT`
loops slow down by orders of magnitude, so `ifconfig up` with the trap on
from the start can fail where it normally works. Two ways round it, in
increasing order of desperation:

- arm *after* the phase that times out and trace the next one;
- arm, let it fail, and read the trace anyway — a failed attach up to the
  timeout is still the sequence up to the timeout;
- split one run into several, each with its own `mark`, and stitch them
  with `tools/decode.py --since`.

### Alongside wl_diag

The two are independent and can be loaded together: `wl_diag` patches
functions inside `wl`, this patches `fixup_exception` in the kernel, and
each `DIE_BREAK` handler ignores addresses that are not its own. Both stamp
records with `sched_clock()`, so the two streams share a time base and can
be merged on timestamp. Load `wl_diag` first if you want its own arming to
happen on `wl`'s COMING as usual.

### Reading the capture

`decode-wl-mmio.py` is a filter, like `decode-wl-diag.py`: stdin by default,
one line per record, flushed as it goes, so it sits in a pipe from the
device.

```
nc -u -l -p 5555 | wl-mmio-trap/decode-wl-mmio.py
wl-mmio-trap/decode-wl-mmio.py < mmio.bin
wl-mmio-trap/decode-wl-mmio.py mmio.bin --since ifup --until done
```

`--base` adds the window base back to the offsets when absolute addresses
are wanted. The record layout is the one `wl_diag` uses and the op numbers
come from the free part of its enum, so the two streams can be merged on
the timestamp: both stamp with `sched_clock()`.

For the register-level decoding -- object memory, PHY and radio ports,
window moves -- feed the capture to `reverse-tools/mmio2ops.py`, which was
written for the x86 mmiotrace of the same family and reads the same shape of
data.

## Record format

`struct wl_mmio_rec`, packed: `u64 ts_ns; u32 seq; u32 addr; u32 val;
u32 aux; u8 op; u8 cpu; u16 pad`.

`op` is 60 for a read, 61 for a write, 62 for a mark (the twelve bytes of
`addr`/`val`/`aux` carry the label, NUL-padded, the same shape `wl_diag.c`'s
own MARK uses) and 255 for a drop count (`aux` carries the number). `addr` is the **offset into the window**, not a kernel VA: the
VA changes from boot to boot and tells a decoder nothing. `aux` carries the
access width in its low byte and bit 8 when the access sat in a branch
delay slot.

## When the window is in KSEG1

On a board whose BAR sits in the low 512 MB of physical space, `__ioremap()`
returns a bare `CKSEG1ADDR` and the driver's window has no page table entry.
Nothing here can trap it: there is no pte to invalidate, and the CP0 Watch
registers -- the other way to fault on a fixed-translation address -- are
absent on cores that report `hardware watchpoint : no` in `/proc/cpuinfo`,
BMIPS4350 among them.

What is left is to stop the window from being in KSEG1 in the first place.
The way round it is `__ioremap()` itself. Its shortcut tests `flags` for
*equality* with `_CACHE_UNCACHED`, while the mapping path it is avoiding ORs
`flags` into `_PAGE_GLOBAL | _PAGE_PRESENT | __READABLE | __WRITEABLE`.
Asking for `_CACHE_UNCACHED | _PAGE_GLOBAL` fails the equality, takes the
mapping path, and lands on exactly the pte the shortcut was avoiding. One
exported call, no page tables built by hand. (`ioremap_page_range()` would
have been the obvious tool and is not there: `lib/ioremap.o` is in `lib-y`
and MIPS never refers to it, so the linker drops the object and its export
with it.)

The mapping stays uncached, and that is not a detail. A cached mapping of a
device window is broken with or without a tracer watching: reads come from a
line instead of the register, writes sit in a line instead of reaching the
device, and a line fill drags in the neighbouring registers as a side
effect. The same rule is why every alias in this module is uncached -- the
found window's is `ioremap_nocache()`, the redirected window's and the
self-test's are CKSEG1 addresses.

Check first that the driver reaches the window through that function and
with what arguments. `ioremap_watch` only logs; it changes nothing and
returns every call to the real one:

```
rmmod wl
insmod wl_mmio_trap.ko ioremap_watch=1
modprobe wl
dmesg | grep __ioremap
```

`rmmod wl` first, or the mapping has already happened and there is nothing
left to watch. On a BCM63168 board this prints, among others:

```
__ioremap(11000000, 16384, 200) from c2b955fc -- returns CKSEG1, no pte, untrappable
```

which is the 4360's window: 16 KB of its 32 KB BAR, uncached, and reached
through this function. `redirect=1` then answers that call with a mapping
of this module's own:

```
rmmod wl
insmod wl_mmio_trap.ko redirect=1 pci_device=0x4360 init_mm_addr=0x...
modprobe wl
```

The mapping is built from the COMING notifier, before the driver's init
runs, because that is the last moment at which it can exist before the
driver asks. The driver has to be loaded *after* this module: one that is
already up is holding a CKSEG1 pointer and will not ask again.

`pci_device=` matters here. These boards carry two radios that both match
the network class, and the redirected window should be the one being
traced.

An `iounmap()` of the redirected window is refused, because taking the
area down would take the page tables with it and this module is holding
saved ptes inside them. The mapping goes when the module does.

This is the one place where the module gives the driver something it did
not ask for. If the pointer were wrong the driver would write through it
regardless, which is why it is off by default.

## Known limitations

- `lwl`/`lwr`/`swl`/`swr` are recognised and refused, not emulated. The
  left/right merge is endian-mirrored and easy to get subtly wrong, and
  `R_REG`/`W_REG`-style pokes do not compile to these. Counted; if the
  counter ever moves, that is the signal to write it.
- A delay slot behind a COP1 condition branch or a DSP `bposge32` is
  refused: the first needs FPU state that may not belong to the faulting
  context, the second calls `force_sig()` when the ASE is absent. Every
  other branch is handled.
- Unloading while wl is live and engaged is handled — the module puts the
  ptes back and takes the breakpoint out in that order, and a `synchronize_sched()`
  covers handlers in flight — but the safest sequence is still `wl` down
  first.
- The window is a whole page at the smallest. When wl repoints it (at
  attach and on each `chanspec` change, per
  `router-data/dsl3580l/full-sweep.zip` phase 20b and
  `router-data/agcombo/bss-up.zip`), accesses to whatever core it is
  pointed at land in this stream too, as plain offsets. `wl_diag.c`'s
  `SI.COREREG` records are what say which core that was.
- Nothing here has been compiled against a real 3.4 MIPS tree or run on
  hardware. The host checks cover the two units that can be checked on a
  host and nothing else.

## Files

- `wl_mmio_trap_main.c` — discovery poll, self-test, the fault handler,
  engage/disengage, `/proc/wl_mmio_trap`.
- `bp_hook.[ch]` — one-word breakpoint sites and the `DIE_BREAK` dispatch.
- `mmio_pte.[ch]` — the page-table walk and the open/close of the window.
- `win_find.[ch]` — BAR lookup and the vmalloc-range scan for its mapping.
- `mips_mmio_emulate.[ch]` — the decoder and emulator.
- `mips_opcode.[ch]` — instruction encoders, host-checked.
- `kern_syms.[ch]` — the four kernel facilities that are not exported.
- `tools/` — host checks, the header shims they need, and `decode.py`.
- `DESIGN.md` — why each piece looks the way it does.
