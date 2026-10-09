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
| template RAM | `TPL.*`; with `tpldump` set, the content of each `TPL.RAMW` in `TPL.DATA` |
| MAC | `MAC.MCTRL`, `MAC.MHF`, `MAC.BW`, `AMT.WR`, `ADDRM.SET`, `RCMTA.WR`, `PHY.FGC` |
| PMU, GPIO, core registers | `PMU.*`, `GPIO.*`, `SI.COREREG` |
| OTP, SROM control | `OTP.*`, `SROMCTL.*` |
| chanspec | `CS.SHM`, `CHANSPEC` |
| userspace commands | `IOVAR.SET`, `IOCTL` (hook on `wlc_ioctl`) |
| frames posted to a TX ring | `TX.PKT` + `TX.DATA` (hooks on `dma64_txfast`, `dma64_txunframed`, or `wlc_txfifo` where those do not resolve), off until `txdump` is set |
| frames received | `RX.PKT` + `RX.DATA` (hook on `wlc_recv`), off until `rxdump` is set |
| TX statuses | `TXS` + `TXS.DATA` (hook on `wlc_dotxstatus`), off until `txsdump` is set |

Reads carry their value through a return trampoline (`retcap` hooks), emitted
as a `RETVAL` record after the read. Inline I/O through the `R_REG`/`W_REG`
macros cannot be hooked; `../wl-mmio-trap/` traces it. The noise sample the CRS calibration reads arrives
through object memory
(`wlc_phy_noise_read_shmem` → `wlc_bmac_read_shm` → `read_objmem[16]`), so
`OBJ.RD` carries it.

## Hooking

- **Return register.** The stub re-executes the displaced words and jumps back
  through `$t9`, or `$t8` when the displaced words write `$t9` (a thunk
  prologue `lui`/`addiu $t9`); if both are written the hook is dropped. Getting
  it wrong shows as an unaligned access with `$t9 == epc`.
- **Unhookable prologues** (a branch inside the detour window) are handled by
  patching the call sites (`lui`/`addiu` + `jalr` in this `-mabicalls` module,
  after checking address, register and that the `addiu` is not shared;
  `wlc_bmac_mhf_get` is hooked this way), or on 3.4 by a `break` and a
  `DIE_BREAK` notifier.
- **Tail calls.** On 7.14.89 the SHM thunks tail-call 16-bit accessors with no
  symbol; the saved `j` itself is diverted, and `tail_aux_src` takes the
  selector from `a2`/`a3`.
- **One hook per op.** `ripiego_di` drops a thunk when the accessor below it
  hooked, or every SHM access would give two records.
- **Names differ between versions** (`read_objmem` on 6.30, `read_objmem16` on
  7.14; `wlc_bmac_set_addrmatch` against `wlc_set_addrmatch`). Hooking a
  missing name gives no error, the class just stays empty; `LOCAL` symbols
  still resolve through kallsyms, a stripped blob (the TG789vac's) is what hides
  them.
- **Signatures come from the prologue**, not the name:
  `../reverse-tools/mipsdis.py <object> --prologo <symbol>`
  (`phy_reg_write_array(pi, array, n)` takes a pointer and is only a marker;
  `wlc_bmac_write_ihr(hw, off, val)`, `wlc_bmac_set_shm(hw, off, val, len)`).
- **Op codes** are the same numbers in both tracers (2.6.30 has 1-50, 55-59
  and 65-66, 3.4 1-59 and 65-66); a new op goes at the end of both enums.
  60-64 belong to `../wl-mmio-trap/`, whose records merge into the same
  streams: the next op here is 67. Hook-table fields use
  designated initializers: a positional field once shifted `retcap` to false
  for every hook.

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
| `txdump` | `0` | bytes of each frame posted to a TX ring to record, up to 256; 168 cover the TX offload header, the d11 TX header and an 802.11 header. Writable at run time |
| `txbudget` | `256` | frames left to record, counting down; write it again for more |
| `tpldump` | `0` | bytes of each template RAM write to record, up to 1024; a beacon template is up to 512. Writable at run time |
| `tplbudget` | `256` | template RAM writes left to record, counting down |
| `rxdump` | `0` | bytes of each received frame to record, up to 256; 96 cover the RX header, the PLCP and an 802.11 header. Writable at run time |
| `rxbudget` | `256` | RX frames left to record, counting down |
| `txsdump` | `0` | bytes of each TX status structure to record, up to 128; 64 to start with, its size differs between versions. Writable at run time |
| `txsbudget` | `256` | TX statuses left to record, counting down |
| `klookup` | `0` | 2.6.30 only: address of `kallsyms_lookup_name` from `/proc/kallsyms`, which that kernel does not export to modules. `../reverse-tools/gen_syms.py` builds the `insmod` line |
| `bump_ptr`, `restore_alloc` | — | 3.4 only: rewind the reserved-module allocator on the TG789vac v2 (see `../router-data/tg789vac-v2/README.md`) |

## TX frames

`dma64_txfast(di, p0, commit)` and `dma64_txunframed(di, buf, len, commit)`
are where hnddma posts a frame to a TX ring, with the d11 TX header pushed in
front: the PHY TX control words and PLCP of data frames reach the hardware
only there, never through a register. At their entry `pkt_rec()` reads the
first `txdump` bytes of the frame, from the linear part of the `sk_buff` or
from the buffer, and queues them as one group: a `TX.PKT` record (length
posted, bytes following, 1 in `aux` for a raw buffer) and the bytes in
`TX.DATA` records, twelve each, packed like `MARK`. The decoder reads the header with
`../reverse-tools/d11ac_txh.py`, which `../wl-mmio-trap/` shares.

```sh
echo 168 > /sys/module/wl_diag/parameters/txdump
echo 256 > /sys/module/wl_diag/parameters/txbudget
```

Unlike the trap at a TX index write, this costs one detour per frame and
`wl` keeps its speed, so the rate control runs as it does without a tracer.
Both hnddma functions are LOCAL and reached through its function table: the
entry detour or the break path takes them, call sites cannot, and where the
module keeps no local symbols they do not resolve at all. That is the case
of the 7.14.43.21 `wl_vd625.ko`, whose symbol table has no local function:
`wlc_bmac_read/write_objmem16` and the two above come out "not found", every
GLOBAL hook resolves. There the fallback is
`wlc_txfifo(wlc, fifo, p, ...)`, GLOBAL, where wlc hands a frame with its
header to the fifo; it is dropped when `dma64_txfast` hooks. In the table
`addr_src` names the argument that holds the packet, `val_src` the length
of a raw buffer.

That the packet is an `sk_buff` and sits in a1 (hnddma) or a2 (`wlc_txfifo`,
as in brcmsmac's `brcms_c_txfifo`) comes from GPL sources, not from these
blobs; the decoder checks the header's `frame_len` against the length, so if
it is wrong the output says "layout non riconosciuto" instead of printing
plausible fields, and `addr_src` is what to change. On Broadcom's CPE
kernels a packet can also be an FkBuff behind a pointer tagged in its low
bits (this `wl` imports `fkb_xlate` and `fkb_free`): an unaligned packet
pointer is recorded with 2 in `aux` and no bytes, so the decoder says how
many frames came that way instead of reading them as an `sk_buff`.

## RX frames

`wlc_recv(wlc, p)` is where the bmac layer hands a received frame to wlc,
the d11 RX header still in front of it. With `rxdump` set, `pkt_rec()`, the
same function as for TX, records its start as `RX.PKT` + `RX.DATA`, and the
decoder reads the header with `../reverse-tools/d11ac_rxh.py` at the offsets
`b43_rx()` uses: frame length (checked against the packet's), PHY and MAC
RX status, the two power bytes, the padding flag, the PLCP read by the frame
type in PHY status 0, the 802.11 header, and the 16 bytes b43 does not read
in raw. `p` in a1 comes from brcmsmac's `brcms_c_recv`; on the agcombo's
`rxtx-1s-ht20-40-80.zip` and `rxtx-2s-ht20-40-80.zip` every frame's length
checks against the packet's.

## TX statuses

`wlc_dotxstatus(wlc, txs, ...)` is where wlc takes the status of a frame the
microcode has finished with: acknowledged or not, attempts, suppression.
With `txsdump` set, `txs_rec()` records `TXS` (`a2` at the entry, the bytes
that follow) and the start of the `tx_status` structure in `TXS.DATA`
records, packed like `TX.DATA`. The structure is read raw because its
layout differs between versions; on 6.30 the function masks byte 3 with 7
for the fifo, the low byte of the frame ID. The decoder prints it in hex,
in the driver's byte order, and the offset where a 16-bit word equals the
frame ID of a frame recorded at the TX post, with that `TX.PKT`'s sequence
number: a status is tied to its descriptor, and the match places the
fields.

On the D6220's 7.14.89 (`wlD6220.o_save`) the five TX and RX hooks take the
4-word detour (`audit_hooks.py`), and the caller, `wlc_bmac_txstatus()`,
builds `txs` on its stack: the frame ID at +2, the low half of `XMITSTAT_0`
at +32, `XMITSTAT_2`/`3` at +34/+38, the second package at +42-+57, and
`XMITSTAT_1` left in a2. `decode-wl-diag.py --txs-714` rebuilds the eight
words from there in the format of b43's `TX status` warning, with the
reading of `b43_txstatus_read_ac()`. 6.30 keeps them 16 bytes lower.

## One capture for templates, TX and RX

`../wl-capture-scripts/capture_txrx.sh` takes all three in one stream: for
each chanspec, template RAM content through the bring-up, then TX and RX
headers, and the TX statuses where the module has them, while a station
passes traffic both ways, cut by `MARK` records
(`tpl <chanspec>`, `txrx <chanspec>`, `end`) for `split_trace.py --on mark`.
`VHTMODE=0` sets `wl vhtmode 0` while the interface is down, so a VHT
station joins as HT and the frames are HT (frame type 2); 80 MHz needs VHT,
so that run takes the 20 and 40 MHz chanspecs:

```sh
VHTMODE=0 sh capture_txrx.sh wl1 test-ap "5g36/20 5g40/40"
VHTMODE=1 sh capture_txrx.sh wl1 test-ap "5g36/20 5g40/40 5g44/80"
```

## Template RAM content

`wlc_bmac_write_template_ram(hw, offset, len, buf)` writes the beacon and
probe response templates, with whatever the microcode expects in front of
the frame, and also the PHY's tone waveforms. With `tpldump` set, the
`TPL.RAMW` record keeps its offset and length and carries in `aux` the number
of bytes of the write that follow in `TPL.DATA` records, one group as for
`TX.PKT`; with it at 0, `aux` is 0 and nothing follows. `tplbudget` counts
the writes left.

```sh
echo 512 > /sys/module/wl_diag/parameters/tpldump
echo 64  > /sys/module/wl_diag/parameters/tplbudget
```

The decoder prints the `TPL.RAMW` line as before and the content under it:
`../reverse-tools/d11_template.py` finds a beacon or probe response by its
frame control and the SSID element that opens the body, prints the bytes in
front of it raw, then header, fixed fields and elements; anything else, the
waveforms included, comes out in hex. The tone waveforms of a calibration
are many writes, so arm it after the bring-up, or with a budget that covers
it.

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

`wl_diag` is loaded once and arms by itself on the target's
`MODULE_STATE_COMING` (after relocation and kallsyms, before `mod->init`, so the
probe and attach are under the hooks without a PCI rescan) and disarms on
`GOING` (after `mod->exit()`, text still mapped), on both kernels. It holds no
reference on `wl`, since `rmmod wl` is the core step of a cold capture.

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

- Repeated `** DROP **` records mean the reader is slower than the writer on
  average; a larger FIFO only absorbs bursts.
- `WLC_GET_VAR` is not recorded: its value exists only on return.
- The device scripts use shell builtins, `wl` and `sleep` only: many busybox
  builds lack `head` and `awk`.
- `.gitattributes` forces `eol=lf`; a script checked out with CRLF fails with
  `: not found` or `unexpected end of file`.
