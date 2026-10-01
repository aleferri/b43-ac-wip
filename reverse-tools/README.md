# reverse-tools

Tools for reverse-engineering Broadcom's `wl` driver and checking the b43
AC-PHY port against the captures. The ELF readers need `pyelftools`
(`requirements.txt`). Each script's `--help` and header say more.

## Trace pipeline

`test/unit/gates.sh` runs the whole chain; none of it is needed by hand for a
gate. The comparison itself is `test/unit/compare.py` (positional) and
`test/unit/cmp_skip.py` (the score).

- **`../wl-diag/decode-wl-diag.py`** decodes the tracer's 28-byte big-endian
  records into text lines.
- **strip_other_core.py** removes the other wireless core's attach from the
  head of a capture (`wl` attaches every core at load).
- **trace_filter.py** removes instrumentation artefacts, always in this order:
  `--retvals` folds `RETVAL`/`ARGX` into the preceding op (without it every read
  is `val=UNDEFINED`), `--mod-reads` drops the internal read of every `MOD`,
  `--collapse` keeps only the `TBL` headers (it hides values: never compare
  collapsed traces op for op).
- **split_trace.py** cuts a trace into segments `--on mark` (cold sweeps),
  `mod`, `chanspec` (hot sweeps) or `gaps`; `--drop-between-runs` drops what
  lies between a run end and the next `mod COMING`.
- **timeline.py** lists the events after the bring-up (`WD`, `POLL`, `TPL`,
  `BSS_UP`, `NOISE`) with the watchdog phase and entry-turn shape.
- **beacon_reloads.py** says on which `conf_tx` passes the stack republished the
  beacon.
- **check_class_coverage.py** says which op classes a capture traces
  (`router-data/CLASS-COVERAGE.md`).
- **compress_turns.py** shortens a weather-radar segment by removing whole
  blocks of 60 steady watchdog turns (a whole number of every period the driver
  counts, and an even number of beacon reloads); a TG789vac weather segment goes
  from ten minutes of gate to about ninety seconds.

### Bus-level captures

- **mmio2ops.py** decodes raw BAR0 accesses, an x86 mmiotrace, the binary
  records of `../wl-mmio-trap/` or the bpftrace and ftrace kprobe captures of
  the hybrid `wl`'s accessors, into the same vocabulary (`PHY.*`, `RAD.*`,
  `OBJ.*` with `sel=`, `MAC.*`, `REG.*`, plus `CC.*`, `SROM.RD`, `WRAP.*`,
  `PCIE.*`, `EROM.RD`). The window's core is inferred from unambiguous offsets
  (`--mark-windows`), except in the ftrace captures, which record the PCI
  config writes: there it is known, and the other cores' accesses are
  `CORE.*`. `--erom` lists the cores, `--srom` writes the SROM words.
  A 32-bit shared-memory access is split low half first, and the PHY_VER read
  after every PHY write is dropped unless `--keep-flush`.
- **ops_fold.py** `fold` rebuilds accessor-level ops from their bus footprint
  (the MOD mask as `rd ^ wr` is a lower bound); `report` lists window moves,
  polls, interrupt reasons, DMA activity and pauses.

## Analysis

- **tracelib.py**: parsing, op normalisation, attribution of each op to its
  function through the marker stack, capture discovery inside archives
  (`archive.zip!inner.txt`). It defines "the same op".
- **fn_map.py**: `coverage` (ops found per port function, and the gaps), `span`,
  `fingerprints`. Pass the capture raw.
- **anchors.py**: resolves the `#NNNN` and `[capture-ref: …]` references in the
  source comments against `capture-refs.conf`; exits 1 when one does not
  resolve.
- **phase_absent.py**: which phases the stock driver does not run, over every
  address of the phase.
- **locate_missing_ops.py**: where in the port a missing op goes.
- **trova_sorgente.py**: where a written value comes from, trying every value
  read or written through every transformation.
- **find_readback_hardcodes.py**: registers the stock driver reads and the port
  writes as a literal.
- **reads.py**: `risk`, `intent`, `consumers`, `plan` for the reads the port
  discards (method in `docs/retrace-todo.md`, "Discarded reads").
- **srom.py**: which written values are board data (`literals`, `correlate`,
  `verify`).
- **band_delta.py**: what the stock driver writes differently on 2.4 GHz,
  from two hops per band of one capture (folded with `ops_fold.py`); `--also`
  runs a second capture and marks each line same, other values or absent.
- **decorrelate_channels.py**: classifies each written key over N segments
  (invariant, channel-only, width-only, centre frequency, dynamic).
- **check_channeltab.py**: the 2069 channel table against every sweep segment.
- **rxiq_points.py**: extracts the RX IQ solve points and scores the solver.
- **ppr_invert.py**: inverts the PPR chain on a segment.
- **sweep_report.py**: per-channel score and register diff. Its denominator is
  the stock ops only, not comparable with `cmp_skip.py`'s.
- **probe_schedule.py**, **watchdog_turns.py**, **latch_placement.py**:
  measurement only, not used by the gates.
- **annotate_enables.py**, **dataflow.py**: debug utilities.

## Blob analysis

- **audit_hooks.py** replays `wl-diag`'s `pianifica()` offline on a `wl` object:
  prologue, detour window, return register, call sites, classes a capture would
  contain. Zero call sites is the misleading case: the hook arms, and the class
  stays empty because the function is inlined everywhere.
- **callsites_pic.py** finds call sites in a MIPS PIC module (`lui`/`addiu` +
  `jalr`).
- **mipsdis.py** is a minimal big-endian MIPS32 disassembler that reads hook
  signatures from prologues (`--prologo`).
- **cmp_funcs.py** compares same-name function bodies across `wl` builds.
- **gen_syms.py** builds the `klookup=` insmod line of `wl-diag/2-6-30`.
- **extract_acphy_tables_from_descriptor.py** (it generated
  `tables_phy_ac.c`), **extract_acphy_txgain.py**,
  **extract_chan_tuning_2069rev4.py**: one-shot table extractors.

## Device side

- **mempeek.c**, **pcicfg.c**, **memfind.c**: static helpers over `/dev/mem` and
  `/proc/bus/pci` (read MMIO, repoint `BAR0_WIN` onto ChipCommon, find a kernel
  variable by value).
- `../wl-capture-scripts/`: `capture_cold_init.sh` (one `wl` reload per
  channel), `capture_hot_init.sh` (`{chanspec; up; wait; down}`, split with
  `--on chanspec`), `capture_profiles.sh` (the shared channel lists, copied next
  to them; `<bw>`, `<bw>dfs`, `<bw>meteo`), `capture_cold_tg789vac.sh` (see
  `../router-data/tg789vac-v2/README.md`).

## Checks on C sources

**csanity.py** checks C files without a compiler: `*/` in comments,
unterminated comments or literals, unbalanced brackets, a file-`static` symbol
used before its declaration, and (only for the `wl_diag.c` files, whose kernels
are C90 and `-Werror`) declaration after statement.
