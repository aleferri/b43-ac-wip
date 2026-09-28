# wl-diag — inline-detour tracer for the `wl` driver

A kernel module that hooks the stock Broadcom `wl` driver's hardware accessors
with an entry detour (no kprobes) and exposes the records through
`/proc/wl_diag`. `3-4-11/` targets kernel 3.4 with `wl` 7.14; `2-6-30/`
targets 2.6.30 with `wl` 6.30. The two share the record format and
`decode-wl-diag.py`. The head of `3-4-11/wl_diag.c` documents the mechanism
and its limits: MIPS32R1, module memory written in place,
`flush_icache_range`; the head of `2-6-30/wl_diag.c` lists what the 2.6.30
variant leaves out.

## What it traces

| space | classes |
|---|---|
| PHY registers, radio, PHY tables | `PHY.*` (and/or distinct), `RAD.*`, `TBL.*`, `PHY.RDW`/`WRW`, `PHY.WARR` |
| object memory / SHM | `OBJ.RD`/`OBJ.WR`, `OBJ.BULKR`/`BULKW`, `OBJ.SET` |
| template RAM | `TPL.*` |
| MAC | `MAC.MCTRL`, `MAC.MHF`, `MAC.BW`, `AMT.WR`, `ADDRM.SET`, `RCMTA.WR`, `PHY.FGC` |
| PMU, GPIO, core registers | `PMU.*`, `GPIO.*`, `SI.COREREG` |
| OTP, SROM control | `OTP.*`, `SROMCTL.*` |
| chanspec | `CS.SHM`, `CHANSPEC` |
| userspace commands | `IOVAR.SET`, `IOCTL` (hook on `wlc_ioctl`) |

Reads carry their value through a return trampoline (`retcap` hooks), emitted
as a `RETVAL` record after the read. Inline I/O through the `R_REG`/`W_REG`
macros cannot be hooked. The noise sample the CRS calibration reads arrives
through object memory
(`wlc_phy_noise_read_shmem` → `wlc_bmac_read_shm` → `read_objmem[16]`), so
`OBJ.RD` carries it.

## Hooking

**The return register.** The stub re-executes the displaced words and then
jumps back through a register. On 3.4 it uses `$t9` unless the displaced words
write it, `$t8` otherwise; if both are written the hook is dropped at planning time.
A thunk prologue `lui $t9` / `addiu $t9` is the case that forces this. The
symptom of getting it wrong is unmistakable: an unaligned access with
`$t9 == epc`, and `ra` pointing at the caller.

**Unhookable prologues** have a branch inside the detour window. There are two
ways around them:

- **patching the call sites** (preferred). The module is `-mabicalls`, so calls
  are `lui`/`addiu` + `jalr` (or `jr $t9` for tail calls). The pair is rewritten
  to load the stub, after three runtime checks: exact address, jump on the same
  register, `addiu` not shared. On 3.4 a fourth drops a site whose `lui` feeds
  more than one epilogue. `wlc_bmac_mhf_get` is hooked this way.
- **the `break` path**, 3.4 only: a die notifier on `DIE_BREAK`.

**Tail calls.** On 7.14.89 the SHM thunks `wlc_bmac_read/write_shm` tail-call
16-bit accessors that have no symbol. There the tail call itself is diverted:
the stub leaves the argument set-up in place and exits by re-executing the
saved `j`. `tail_aux_src` takes the selector from `a2`/`a3`, so `sel` carries
the real value.

**One hook per op** (3.4). When a build has two ways to the same op, only the
first that resolves and hooks is armed. On both kernels `ripiego_di` drops a
thunk when the accessor below it hooked, otherwise every SHM access would
produce two records.

**Names differ between versions** (`read_objmem` / `read_objmem16`, and so on).
Hooking a missing name gives no error; the class just stays empty.

| hook | 6.30 | 7.14 |
| --- | --- | --- |
| `wlc_bmac_read/write_objmem` | `LOCAL` | absent |
| `wlc_bmac_read/write_objmem16` | absent | `LOCAL` |
| `wlc_bmac_read/write_shm` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_copyfrom/copyto_objmem` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_template*_reg` | absent | `GLOBAL` |
| `wlc_bmac_write_template_ram` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_set_addrmatch` | `GLOBAL` | absent |
| `wlc_set_addrmatch` | absent | `GLOBAL` |
| `wlc_bmac_write_amt`, `wlc_bmac_set_rcmta` | `GLOBAL` | `GLOBAL` |
| `phy_reg_write_array`, `phy_reg_read/write_wide` | `GLOBAL` | `GLOBAL` |
| `wlc_bmac_write_ihr`, `wlc_bmac_set_shm` | `GLOBAL` | `GLOBAL` |

`LOCAL` functions in `.text` still resolve through `kallsyms` (`is_core_symbol()`
filters on section flags, not binding). A poor symbol table in the blob, as in
the TG789vac v2's `wl.ko`, is what really hides them.

**Signatures come from the prologue, not the name.** Read them with
`../reverse-tools/mipsdis.py <object> --prologo <symbol>`. Some examples:

| function | signature read from the prologue |
| --- | --- |
| `phy_reg_write_array(pi, array, n)` | `a1` is a **pointer**, `a2` the count; a marker, the writes arrive through the 16-bit hooks |
| `phy_reg_write_wide(pi, val)` | fixed register, value in `a1` |
| `phy_reg_read_wide(pi)` | value in the `RETVAL` |
| `wlc_bmac_write_ihr(hw, off, val)` | `off=a1`, `val=a2` |
| `wlc_bmac_set_shm(hw, off, val, len)` | `off=a1`, `val=a2`, `len=a3` |
| `wlc_bmac_set_addrmatch(hw, idx, addr)` | `idx=a1`, `a2` a pointer; branch at word 2 → short-j |

Op codes are the same numbers in both tracers (2.6.30 stops at 50, 3.4 goes on
to 54), and `decode-wl-diag.py` does not tell the versions apart. A new op goes
at the end of both enums with the same value.

**Hook-table fields** are set with designated initializers (`.retcap = true`).
A field inserted into positional initializers shifts every value after it.
That is how `retcap` once silently went false for every hook, and no
`RETVAL` arrived.

## Checking the plan before flashing

`pianifica()` decides on the device and logs to dmesg. A hook that does not
attach gives **no error**: the capture just lacks that class. The same
decisions can be made offline from the pre-link object:

```sh
python3 ../reverse-tools/audit_hooks.py wlD6220.o 3-4-11/wl_diag.c
```

It prints one line per hook (`detour`, `short-j`, `sites`, `dropped`,
`absent`), the classes a capture would contain, and checks the shape of the
hook table. The verdict on prologue and return register is definitive; the
call-site count is a minimum. Whether the hooked function is called on the path
of interest is `../reverse-tools/callsites_pic.py`'s question.

## Parameters

| param | default | effect |
|-------|---------|---------|
| `arm` | `0` | `0` = dry run, log the hook plan only; `1` = apply the patches |
| `target` | `wl` | module to hook. Hooks arm at its `MODULE_STATE_COMING` and disarm at `GOING`; on 3.4, symbols resolved in other modules are discarded |
| `delay` | `0` | `1` = also hook `osl_delay` (noisy) |
| `fifo_recs` | `131072` on 3.4, `8192` on 2.6.30 | queue records, 28 bytes each. On 3.4 the queue is allocated with `vmalloc` and the default is 3.5 MB, ~25 s of margin |
| `skipphyrd` | empty | **PHY register** reads not to record, e.g. `"0x253,0x254"` |
| `klookup` | `0` | 2.6.30 only: address of `kallsyms_lookup_name` from `/proc/kallsyms`, which that kernel does not export to modules. `../reverse-tools/gen_syms.py` builds the `insmod` line |
| `bump_ptr`, `restore_alloc` | — | 3.4 only: rewind the reserved-module allocator on the TG789vac v2 (see `../router-data/tg789vac-v2/README.md`) |

## Build

Out of tree, from the variant's directory and against the device kernel (same
`.config` and `Module.symvers`, or vermagic/CRC will not match):

```sh
cd 3-4-11 && make KDIR=/path/to/kernel-3.4-rt ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j
cd 2-6-30 && make KDIR=/path/to/kernel-2.6.30 ARCH=mips CROSS_COMPILE=mips-linux- -j
```

Copy `wl_diag.ko` to the device and `decode-wl-diag.py` to the collecting host.
Run `../reverse-tools/csanity.py` on `wl_diag.c` first.

## Capture workflow

### Arming and disarming

`wl_diag` arms by itself on the target's `MODULE_STATE_COMING` and disarms on
`GOING`, on both kernels. It is loaded once, and `rmmod wl` / `insmod wl` in a
loop do not concern it.

**The notification order.** It allows this on both kernels, checked on
`kernel/module.c` of v2.6.30 and v3.4:

- **`COMING`** comes after `load_module()` (relocations applied, kallsyms
  added) and before `mod->init`. The probe and the attach fall under the hooks,
  with no PCI remove/rescan.
- **`GOING`** comes after `mod->exit()` and before `free_module()`, so the
  prologues are restored with the text still mapped.
- **Disarming** restores the words and then calls `synchronize_sched()`. The
  tracer holds no reference on the target, because `rmmod wl` is the core step
  of a cold capture.

### The steps

1. **Start the host listener first.** Use TCP (busybox `nc` has no `-u`):

   ```sh
   ncat -l 5555 | python3 decode-wl-diag.py | tee trace.txt
   ```

   On Windows, write a raw `.bin` with `ncat` or a PowerShell `TcpListener`, and
   decode it afterwards: `python decode-wl-diag.py < trace.bin > trace.txt`.
2. **Check the plan** on the device: `insmod wl_diag.ko` (dry run; on 2.6.30
   add `klookup=`), `dmesg | grep wl_diag`, `rmmod wl_diag`.
3. **Arm and start the pipe**:

   ```sh
   insmod /tmp/wl_diag.ko arm=1
   cat /proc/wl_diag | nc <HOST> 5555 &
   ```

4. **Run the capture script** from `../wl-capture-scripts/`:
   `capture_cold_init.sh` for cold (one reload per channel),
   `capture_hot_init.sh` for hot. Both read their channel lists from
   `capture_profiles.sh`, which must sit next to them on the device.
5. **Close the reader before `rmmod wl_diag`**: the fops have
   `.owner = THIS_MODULE`, so an open reader makes the unload fail with
   `-EBUSY`.

**Cycle boundaries** are MARK records. Writing to the buffer
(`echo "ch36 bw20" > /proc/wl_diag`, twelve characters) injects a label.
`wl_diag` adds `mod COMING` / `mod GOING` itself. MARK records are queued with
the operations around them, and `../reverse-tools/split_trace.py --on mark`
cuts on them.

**Bringing the BSS up.** Setting the SSID before `up` and running `bss up` is
what makes the stock driver program the per-core tables. `wl up` alone attaches
without a BSS:

```sh
wl -i wl1 down; wl -i wl1 up; wl -i wl1 ssid <SSID>; wl -i wl1 bss up
```

**Rejected configurations.** The driver refuses configurations whose block
includes channels 120, 124 and 128 (the TDWR band, 5600–5650 MHz) until their
availability check completes.

**DFS channels.** The radar detector polls `PHY.RD 0x0253` and `0x0254`
continuously, up to 85% of all PHY reads. For bulk sweeps, filter them before
the FIFO with `skipphyrd="0x253,0x254"`:

- the filter applies to `OP_PHY_R` only, since offsets `0x252`/`0x254` also
  occur as object memory;
- `0x251`/`0x252` are kept;
- filtered records have their own counter, separate from `OP_DROP`.

## Notes

- **Buffer size.** A large FIFO absorbs bursts, not a writer that is faster on
  average than the reader. Repeated `** DROP **` records mean the reader is too
  slow.
- **Userspace commands.** `IOVAR.SET` / `IOCTL` record commands such as
  `wl phycal_tempdelta 40`. `WLC_GET_VAR` is not recorded, since its value exists
  only on return.
- **Device scripts.** Many busybox builds lack `head`, `awk` and similar, so the
  device-side scripts use shell builtins plus `wl` and `sleep` only.
- **Line endings.** `.gitattributes` forces `eol=lf`. A script checked out with
  CRLF fails with messages like `: not found` or
  `unexpected end of file (expecting "done")`; strip the `\r` before running it.
