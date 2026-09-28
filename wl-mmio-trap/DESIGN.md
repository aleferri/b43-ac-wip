# Design notes: wl_mmio_trap

The reasoning trail, not the operating instructions — those are in
`README.md`. Every kernel claim below cites the v3.4 source it was checked
against, because the previous round of this package rested on three that
turned out to be wrong.

## 1. The problem

`wl_diag.c` hooks named accessor functions in the `wl` blob. Its own README
says the one thing it cannot see is inline I/O through the `R_REG`/`W_REG`
macros: direct loads and stores through wl's register pointer, with no call
to hook. The concrete case on record (`docs/crs-min-power.md`) is the noise
sample the `crs_min_pwr` calibration reads, which has never appeared in any
capture and may be exactly this.

## 2. Why the fault has to be caught at `fixup_exception()`

The natural design is a page-fault notifier. It does not exist here.

`arch/mips/mm/fault.c`, `do_page_fault()`:

```c
#ifdef CONFIG_KPROBES
	if (notify_die(DIE_PAGE_FAULT, "page fault", regs, -1,
		       (regs->cp0_cause >> 2) & 0x1f, SIGSEGV) == NOTIFY_STOP)
		return;
#endif
```

`wl_diag.c`'s header records `CONFIG_KPROBES=n` on this build, so
`register_die_notifier()` on that path is dead code. The fallback is worse:
an address in the vmalloc range with a non-present pte goes
`vmalloc_fault` → `no_context` → `fixup_exception()` → `die("Oops", regs)`,
and `die()` is `__noreturn` (`arch/mips/kernel/traps.c`). `NOTIFY_STOP` on
`DIE_OOPS` sets `sig = 0` and then `do_exit()` runs anyway.

What does exist is `do_bp()` in the same file:

```c
	case BRK_KPROBE_BP:
		if (notify_die(DIE_BREAK, "debug", regs, bcode,
			       regs_to_trapnr(regs), SIGTRAP) == NOTIFY_STOP)
			return;
```

outside every `#ifdef`, and returning rather than signalling. `wl_diag.c`
already uses this for prologues whose fourth word is a branch. So the
instrumentation point is a `break`, and the only question is where to put
it.

`do_page_fault()` would work but is hit by every demand fault in the
system, userspace included. `fixup_exception()` is reached only from
`no_context`, i.e. from kernel faults that nothing else claimed — in
practice only `uaccess` faults — and it is on the exact path our own faults
take. At its first word `$ra` still holds the caller's return address and
no frame has been pushed, so a handler that wants it to "return 1" only has
to set `v0` and jump to `$ra`.

A one-word patch also disposes of the question the previous round of this
package stopped at. A single aligned 32-bit store has no intermediate
state: a concurrent caller sees either the original word or the break.
There is no half-written prologue for a preempted thread to resume into,
and no need for `stop_machine()` at all.

The cost is one restriction: the displaced word is replayed from a
trampoline (`orig ; j addr+4 ; nop`) when the handler decides the fault is
not its business, so it must not be a branch. `bp_hook_add()` checks and
refuses. The `j` keeps `PC[31:28]`, so the trampoline has to sit in the
same 256 MB region as the target; `kmalloc` returns KSEG0, the same region
as kernel text and as the vendor loader's `wl`, which is the same reason
`wl_diag.c` uses `kmalloc` for its stub pool rather than a static array.

## 3. Why decode-and-emulate instead of single-step

MIPS had no `arch_has_single_step()` at all until 2021, nine years after
this kernel. That commit's own log records trying `CP0_DEBUG.SSt` on a
Loongson 3A4000 — far newer than anything in a 2012 router — and finding
`NoSSt=1`. On the balance of that evidence there is no usable hardware
single-step here.

So `mips_mmio_emulate.c` decodes the one faulting instruction
(`lb/lbu/lh/lhu/lw/sb/sh/sw`), performs the bus access itself with
`__raw_read/write` (never `readl`/`writel`, which on a big-endian target
may swap in a way a bare `lw`/`sw` never would), patches the GPR and
advances the resume PC.

Delay slots are handled now rather than counted:
`__compute_return_epc_for_insn()` is `EXPORT_SYMBOL_GPL`
(`arch/mips/kernel/branch.c`), so the branch in `cp0_epc`'s word can be
evaluated properly. Two families are refused up front instead: the COP1
condition branches, which read `fcr31` out of the current thread's FPU
state, and DSP `bposge32`, which calls `force_sig()` when the ASE is
absent. Neither can come out of an `R_REG`/`W_REG` sequence.

The resume address is computed **before** the access, not after. A linking
branch writes `$ra` when it executes, before its delay slot runs, so doing
it the other way round would clobber a delay-slot load into `$ra`. It also
means an uncomputable branch leaves nothing half done.

`lwl/lwr/swl/swr` stay recognised and refused. The left/right merge is
endian-mirrored and easy to get subtly wrong, for a case that comes from
unaligned bulk copies rather than single-register pokes.

## 4. Why an alias, and why the bracket is gone

The previous round made wl's own pte valid for the duration of each
emulated access and invalid again afterwards, with a
`flush_tlb_kernel_range()` on every transition, on the grounds that a
second mapping of the same physical range would reproduce the DSL-3580L
crash.

That reading of the crash does not hold up. `cc_dump` killed the router by
writing `bar0win` (PCI config 0x80) and repointing the shared window at
ChipCommon while wl was mid-transaction with D11. An `ioremap` on its own
writes nothing to the device; it is the *window move* that was fatal, not
the *mapping*. A second, read-only-in-the-sense-of-never-touching-bar0win
alias of the same physical pages cannot move anything.

Taking the alias buys three things:

- the per-access `flush_tlb_kernel_range()` disappears. It was an
  `on_each_cpu(..., wait=1)` IPI issued from inside an exception handler,
  which deadlocks if the faulting context had interrupts off;
- the completeness hole closes. While cpu 0 had a page momentarily valid
  for its own emulated access, cpu 1's access to that page executed
  natively and was never seen. That is precisely the property the tool
  exists to provide, and the old design lost it in the one window it
  opened itself;
- wl's instruction never executes. Which is what makes the bracket
  unnecessary.

The bracket existed to stop the trap from splitting "point the window" from
"use the window" at instruction granularity across the ten `si_*`/`otp_*`
functions. With permanent trapping nothing is split: the access is
performed inside the same exception that trapped it, at the same physical
address, with the window still wherever wl last pointed it. The residual is
latency between wl's own window move and its own access — which is
wl-internal and already serialised by wl, since natively those are two
separate instructions too.

That leaves `mips_opcode.c` (still needed for the trampolines) and drops
`wlmmio_stub.c`, `wlmmio_pool.c` and `window_bracket.c` entirely, along
with the unwritten patch-commit function they were waiting on. If the
latency does turn out to matter, the bracket comes back as
`mmio_pte_open()`/`mmio_pte_close()` around those calls — but only from
process context, never from the break handler, for the IPI reason above.

**This section rests on two things not disassembled from this blob:** that
`si_corereg` moves the window through PCI config space rather than through
the trapped BAR window itself, and that wl serialises its own window moves.
Both are how `bcma`/`b43` behave in-tree. If either is false the argument
for dropping the bracket weakens and the latency residual becomes a real
hazard.

## 5. Why the window has to be found, not passed in

The previous round took `win_base`/`win_len` as insmod parameters "on
purpose", with the README describing a manual procedure to find them. That
procedure cannot work as written: the module was also required to load
*before* wl, and before wl's `ioremap` there is no mapping to point at.

Discovery splits cleanly in two, because the two facts come from different
places and at different times. The PCI layer knows the BAR's physical
address as soon as the device is enumerated, whether or not wl is loaded.
The virtual address only exists once wl has ioremapped it, and the only
way to find it from a module is to walk the kernel page tables over the
vmalloc range looking for the pte that carries that PFN — `vmlist` is not
exported on 3.4 and is a data symbol besides, so `KALLSYMS_ALL` would be
needed for it. The scan costs a few thousand pte reads, which is cheap
enough to repeat from a poll until the mapping appears.

The scan is also the answer to a question section 5 of the previous
DESIGN could only reason about: whether the window is TLB-backed at all.
`__ioremap()` in `arch/mips/mm/ioremap.c` returns a bare `CKSEG1ADDR` for
any uncached mapping inside the low 512 MB of physical space — no pte, and
so no trap possible. The DSL-3580L's BAR0 at `0xa0000000` is above that and
gets a real mapping; a board with a low BAR does not, and the module
reports that instead of arming half of it. (The similar shortcut in
`asm/io.h`'s `__ioremap_mode()` never fires from a driver, since it is
guarded on `__builtin_constant_p` of all three arguments.)

## 6. Why the invalid pte keeps _PAGE_GLOBAL and drops _PAGE_PRESENT

The MIPS TLB covers an even/odd page pair in one entry and its G bit is the
AND of the two ptes'. Clearing `_PAGE_GLOBAL` on one page of a pair would
make the entry ASID-tagged and the *neighbouring* page fault spuriously
from other contexts. The kernel's own `pte_clear()` copes with this by
copying G from the buddy; keeping the original pte's own G is simpler and
has the same effect.

`_PAGE_PRESENT` is cleared alongside `_PAGE_VALID` on purpose. Clearing
only `_PAGE_VALID` would leave `pte_present()` true, so a fault this module
failed to catch would fall into `vmalloc_fault`, find the pte "present",
return, and re-execute the same instruction forever. A silent livelock is
worse to debug than an Oops.

## 7. What the window still does move, and what that means for the trace

Checked against captures already in the repo rather than assumed:

- `router-data/dsl3580l/full-sweep.zip`, phase `20b` (BCM4352, 111.8 s,
  ten `chanspec` switches): all 280 `SI.COREREG` hits are `core=0x0000`
  (ChipCommon), each in a ~1 s burst correlated with an `OBJ.WR addr=0x00a0`
  timestamp — PLL/PMU retuning for the channel change. Between switches
  (~11 s on one channel): zero.
- `router-data/agcombo/bss-up.zip` (BCM4360, ~218 s, one continuous session
  with a BSS up on a DFS channel): 12 `SI.COREREG`, all in the first 0.4 s,
  then 174 s of nothing. No further GPIO output either, so the LED is
  blinking autonomously in hardware after a one-time setup.

So the window moves at two identifiable moments — attach, and each
`chanspec` change — both already captured by `wl_diag.c`'s hooks with real
values. Accesses made while it is pointed elsewhere land in this stream as
plain offsets, with `wl_diag.c`'s `SI.COREREG` records saying which core
that was. Nothing is lost; the two streams have to be read together, which
they had to be anyway.
