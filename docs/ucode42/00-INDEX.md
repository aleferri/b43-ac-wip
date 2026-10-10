# d11ucode42 dossier: index

What has been understood and checked about the D11 PSM microcode (corerev
42, the AC-PHY BCM4352/4360 family) extracted from four proprietary
Broadcom `wl` drivers, cross-checked against
[aleferri/b43-ac-wip](https://github.com/aleferri/b43-ac-wip) (reverse
engineering from real hardware traces),
[aleferri/b43-tools](https://github.com/aleferri/b43-tools) (assembler,
disassembler and the `interpreter/`) and
[OpenFWWF](https://github.com/fullstory/openfwwf) (free microcode for
corerev 5, which runs on real hardware).

Confidence convention: **CONFIRMED** (checked against source or data),
**HIGH CONFIDENCE** (checked empirically against real traces), **MODEL**
(a declared modelling choice, not a known semantic), **HYPOTHESIS** (a
plausible reading, not checked), **OPEN**. Where nothing is said, a
statement is CONFIRMED. The numbers were regenerated on the blobs and the
captures with the `b43-tools` tree described in `09`.

| file | contents |
|---|---|
| `01-OVERVIEW.md` | the four files, the chip, the reference projects |
| `02-EXTRACTION.md` | formats of ucode42, initvals42, bsinitvals42 |
| `03-FILE-COMPARISON.md` | how alike the four files are, ucode and initvals |
| `04-DISASSEMBLER-AND-DECODER.md` | b43-tools on corerev 42, the interpreter's decoder |
| `05-SHM-SPR-MAP.md` | addressing, known and unknown addresses, SPRs, offset registers |
| `06-DATAFLOW.md` | SHM→SHM chains inside the microcode |
| `07-SUBSYSTEMS.md` | CAC/DFS, the "AMPDU" block, suspend, boot, TBTT and beacon, PHY TX error, TX core selection |
| `08-PSM-INTERPRETER.md` | interpreter, co-simulation, hardware model (TX engine, TSF, TBTT), `condfuzz.py`, comparison with the oracle |
| `09-PATCHES.md` | the b43-tools patches of this revision |
| `10-OPEN-QUESTIONS.md` | what we do not know |
| `11-TIMETABLE.md` | host↔ucode delays in PSM instructions, TSF and PSM clock, beacon |
| `spr_shm_reference.md` | SHM/SPR tables and correlation with the traces |
| `dataflow_chains.md` | table of the dataflow chains |
