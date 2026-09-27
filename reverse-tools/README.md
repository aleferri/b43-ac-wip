# reverse-tools

Tools for reverse-engineering Broadcom's `wl` driver and for checking the b43
AC-PHY port against `wl-diag` captures. The Python tools need the packages in
`requirements.txt` (`pyelftools` for the ELF readers).

## Trace pipeline

The usual order: decode → strip the other core → fold `RETVAL` → compare.
`test/unit/gates.sh` runs the whole chain; none of it needs to be run by hand
for a gate.

- **decode-wl-diag.py** decodes the tracer's binary records (28 bytes,
  big-endian) into text lines (`PHY.WR addr=.. val=..`, …).
- **strip_other_core.py** removes the other wireless core's attach from the head
  of a capture: `wl` attaches every core at load. It matters for the read
  oracle, which starts at the insmod.
- **trace_filter.py** removes instrumentation artefacts. There are three
  composable filters, always applied in the same order:
  - **`--retvals`** folds `RETVAL`/`ARGX` lines into the preceding op. Without
    it every read has `val=UNDEFINED`.
  - **`--mod-reads`** drops the internal read of every `MOD`.
  - **`--collapse`** drops the table-port operations (`0x00d`–`0x011`, gate
    `0x019e`), leaving only the `TBL` header. It hides values too, so never
    compare collapsed traces op-for-op.
- **split_trace.py** cuts a trace into one segment per configuration. The
  boundary is chosen with `--on`:
  - **`mark`** splits on MARK records (labels injected by the capture script,
    `mod COMING`/`GOING`); use it for cold sweeps;
  - **`mod`** splits on module-load boundaries;
  - **`chanspec`** splits on the chanspec write in shared memory, and is how the
    archived hot segments were produced;
  - **`gaps`** splits on time gaps of more than 1.03 s, recognising periodic
    ~1.00 s gaps by the identical op on both sides.

  `--drop-between-runs` removes what lies between a run end and the next
  `mod COMING`.
- **timeline.py** lists the environment's events after the bring-up, with their
  instants: `WD` (a 1 s watchdog turn), `POLL` (the 150 ms radar poll), `TPL` (a
  template reload), `BSS_UP`. It also reads the watchdog phase and the shape of
  the entry turn (`AC_TIMELINE`, `AC_WD_PHASE`, `AC_WD_ENTRY_TURN`).
- **beacon_reloads.py** says on which `conf_tx` passes the stack republished the
  beacon (`AC_BEACON_RELOADS`, `AC_EDCF_RELOADS`).
- **check_class_coverage.py** says which op classes a capture traces. See
  `router-data/CLASS-COVERAGE.md`.
- **reorder_trace.py** reorders a trace on another's order, to align two
  captures.
- **The comparison itself** is `test/unit/compare.py` (positional) and
  `test/unit/cmp_skip.py` (the score).

### Bus-level captures (mmiotrace)

A capture taken with the kernel's mmiotrace on a PCIe card has none of the
accessor classes and none of the config-space traffic, but it has everything
the MAC and the DMA do, which `wl-diag` never traced. Two tools bring it into
the same vocabulary:

- **mmio2ops.py** decodes the raw BAR0 accesses into `PHY.WR/RD`, `RAD.WR/RD`,
  `OBJ.WR/RD` (with `sel=` and `OBJ.BULKW` for auto-increment runs),
  `MAC.MCTRL`, `MAC.MCMD` and `REG.*`, plus `CC.*`, `SROM.RD`, `WRAP.*`,
  `PCIE.*`, `EROM.RD` for what lies outside the D11 core. The sliding window
  moves through PCI config space, which mmiotrace does not see, so the core
  behind it is inferred from unambiguous offsets; `--mark-windows` emits a
  `WIN` line at each inferred move, `--erom` lists the cores, `--srom` writes
  the SROM words in the `wl1_srom.txt` layout. The output goes through
  `split_trace.py --on chanspec` unchanged. Two conventions to keep in mind
  when comparing with a router capture: a 32-bit shared-memory access is
  split low half first (little-endian host; the MIPS stub of
  `test/integration` does the opposite), and the PHY_VER read `wl` makes
  after every PHY write is dropped unless `--keep-flush`.
- **ops_fold.py** `fold` rebuilds the accessor-level ops from their bus
  footprint -- `PHY.MOD`/`RAD.MOD` from an adjacent RD+WR pair (the mask is
  `rd ^ wr`, a lower bound), `MAC.MCTRL val mask`, `TBL.WR/RD id off len`
  headers before the 0xd/0xe writes -- and `report` lists what the bus does
  not show and what can be inferred about it: window moves, polling loops,
  `macintstatus` reasons, DMA ring activity per chanspec segment, pauses.

## Analysis

- **tracelib.py** is the library the others import. It holds:
  - parsing of vendor and port lines, and op normalisation;
  - attribution of each op to its innermost function through the marker stack,
    and spans;
  - capture discovery, archives included, with the syntax
    `archive.zip!inner.txt`.

  Normalisation and attribution live here because they *define* "the same op"
  and "this op belongs to this function".
- **fn_map.py** places each port function inside a capture, in three modes:
  - `coverage` counts the ops found per function and shows the **gaps**;
  - `span` gives the episode range each function accounts for, valid only if the
    port reproduces that capture;
  - `fingerprints` derives sequences from the source literals, for captures the
    harness cannot reproduce.

  Pass the capture **raw**. The `--gap` of `coverage` decides what the result
  means: too tight does not close an interleaved phase, too wide collects
  scattered ops.
- **anchors.py** handles the `#NNNN` references and `[capture-ref: …]` markers
  in the source comments:
  - `resolve` says which captures contain an index;
  - `class` checks the op class at an index;
  - `span` checks the index lies in the function's range;
  - `write` regenerates the markers from `capture-refs.conf`.

  It exits 1 when something does not resolve.
- **phase_absent.py** says which phases the stock driver does not run past a
  threshold, looking at **every** address of the phase. `--witness-only` looks
  at the exclusive one only, which proves the register absent, not the phase.
- **locate_missing_ops.py** says where in the port a missing op goes: it finds
  the neighbouring ops' (address, value) pairs in the source.
- **trova_sorgente.py** says where a written value comes from. It tries every
  read and written value of the segment (and of the previous one) through every
  transformation, and keeps what holds on all cases.
- **find_readback_hardcodes.py** finds registers the stock driver reads and the
  port writes as a literal.
- **macro_order_map.py** maps the macro order of phases between two collapsed
  traces.
- **decorrelate_channels.py** classifies each written key over N segments:
  invariant, same-set, channel-only, width-only, centre-frequency, dynamic.
  "Dynamic" needs two segments with the same (channel, width), and the current
  sweeps have one per configuration.
- **check_channeltab.py** checks the 2069 channel table rows against what the
  stock driver writes on each sweep segment.
- **srom.py** says which values the driver writes are board data rather than
  code. `literals` finds driver constants that match a SROM-declared value.
  `correlate` compares two captures. `verify` applies the consumer's
  transformation and looks for the result.
- **reads.py** handles the reads the port discards:
  - `risk`: values that vary across captures;
  - `intent`: what the stock driver does with them;
  - `consumers`: which write consumes each read;
  - `plan`: emits a C read plan.

  See `docs/read-census.md`.
- **rxiq_points.py** extracts the RX IQ solve points from the sweeps and scores
  the solver on them. **fit_rxiq_solve.py** fits the coefficient formula against
  known ground truth.
- **ppr_invert.py** inverts the PPR chain on a segment, under two models of the
  reference maximum (`--model capped`, `--model srom`).
- **sweep_report.py** reports on a sweep. `score` gives one row per channel;
  `regdiff` gives the registers the port writes differently. Its denominator is
  the stock ops only, **not** comparable with `cmp_skip.py`'s.
- **annotate_enables.py**, **dataflow.py**: debug utilities (enable state per
  line; data flow through table reads).
- **Measurement-only**, not used by the gates:
  - **probe_schedule.py**, the probe-phase schedule;
  - **watchdog_turns.py**, the turns where the stock timer skipped the latch,
    with `--check` for the counter sweep;
  - **latch_placement.py**, where the statistics latch falls relative to the
    first turn.

## Blob analysis

- **audit_hooks.py** replays `wl-diag`'s `pianifica()` offline on a `wl` object,
  so a missing class shows up before a capture is taken. It reports:
  - prologue, detour window, return register and call sites;
  - which classes a capture would contain.

  It also checks the shape of the hook table. Zero call sites is the misleading
  case: the symbol resolves, the prologue hooks, and the class stays at zero
  because the function is inlined everywhere.
- **callsites_pic.py** finds call sites in a MIPS PIC module (`lui`/`addiu` +
  `jalr`, no `jal`), with the three checks a call-site patch needs.
- **mipsdis.py** is a minimal big-endian MIPS32 disassembler that reads hook
  **signatures** from prologues (`--prologo`). Semantics come from the captures,
  not from here.
- **cmp_funcs.py** compares same-name function bodies across `wl` builds, both
  byte-identical and identical up to constants (relocation immediates zeroed).
- **gen_syms.py** builds the `insmod` line of `wl-diag-2630` (`klookup=`) from a
  `/proc/kallsyms` copied off the device.

## Table extractors (one-shot, from the ELF)

- **extract_acphy_tables_from_descriptor.py** follows `acphytbl_info_rev{0,2}`
  and dumps the init tables as C arrays; it generated `tables_phy_ac.c`.
- **extract_acphy_txgain.py** extracts the band-specific `acphy_txgain_*`
  tables.
- **extract_chan_tuning_2069rev4.py** extracts `chan_tuning_2069rev4`.

## Device side

- **wl-diag/**, **wl-diag-2630/**: the inline-detour tracer for kernel 3.4 and
  2.6.30. See `wl-diag/README.md`.
- **cc-dump/**: a ChipCommon PMU state dump module. See `cc-dump/README.md`.
- **capture_cold_init.sh** takes a **cold** sweep, one cycle per channel,
  reloading `wl` each time. `wl_diag` is loaded once and arms itself at the
  target's `MODULE_STATE_COMING`, so the attach falls under the hooks without a
  PCI remove/rescan. Kernels 3.4 and 2.6.30.
- **capture_hot_init.sh** takes a **hot** sweep. The module stays loaded, and
  each cycle is `{chanspec; up; wait; down}`. Split it with
  `split_trace.py --on chanspec`, and compare with the `up`/`switch_channel`
  flows and `AC_FIRST_INIT=0`.
- **capture_profiles.sh** holds the channel lists both scripts share. Copy it
  next to them on the device. The groups are disjoint: `<bw>` for channels
  without radar duty, `<bw>dfs` for those with (CAC ~60 s), `<bw>meteo` for the
  weather-radar band.
- **capture_cold_tg789vac.sh** is the cold cycle on the TG789vac v2, where
  reloading `wl` alone crashes the router; see
  `router-data/tg789vac-v2/README.md`.
- **mempeek.c**, **pcicfg.c**, **memfind.c** are static userspace helpers over
  `/dev/mem` and `/proc/bus/pci`:
  - `mempeek` reads MMIO;
  - `pcicfg` repoints `BAR0_WIN` onto ChipCommon;
  - `memfind` finds a kernel variable by value.
- **wl_diag_3580.ko** is the tracer built for the DSL-3580L.

## Checks on C sources

- **csanity.py** checks C files without a compiler:
  - `*/` inside comment prose, unterminated comments or literals, unbalanced
    brackets;
  - use of a file-`static` symbol before its declaration;
  - declaration after statement (C90).

  Use the C90 check only on the `wl_diag.c` files, since the device kernels are
  C90 and `-Werror`. `src/` targets modern mainline, where the rest applies.
