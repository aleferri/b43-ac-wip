# Op-class coverage of the captures

The `wl-diag` tracer gained hooks over time, so the captures **do not all
contain the same op classes**. A capture taken before a hook existed has none
of that class's operations, and that says nothing about the driver.

**An absence is evidence only if the class is traced in that capture.**

`reverse-tools/check_class_coverage.py` regenerates and checks the tables below.
It reads inside the archives too.

```sh
python3 reverse-tools/check_class_coverage.py                 # audit every capture
python3 reverse-tools/check_class_coverage.py --require FILE  # non-zero exit if incomplete
python3 reverse-tools/check_class_coverage.py --conta A,B,... # per-class counts
```

`test/unit/gates.sh` runs `--require` on every cold segment.

## Complete and incomplete captures

A capture is complete when it traces `OBJ`, `TPL` and `CAL` on top of the
PHY/RAD/TBL/MAC/SI/PMU/GPIO classes every capture has.

| capture | ops | state |
| --- | --- | --- |
| `agcombo/bss-up.zip` | 43411 | complete, + AMT ADDRM OBJ.BULK |
| `agcombo/cold-sweep.zip` | 560563 | complete |
| `agcombo/hot-sweep.zip` | 415476 | complete |
| `agcombo/up-nobss-ioctl.zip` | 28718 | complete, + AMT ADDRM OBJ.BULK |
| `d6220/cold-sweep.zip` | 1577029 | complete, + AMT ADDRM OBJ.BULK |
| `d6220/hot-sweep.zip` | 2451630 | complete, + AMT ADDRM OBJ.BULK |
| `dsl3580l/cold01-ch36-bw20.txt` | 41580 | complete, + AMT RCMTA OBJ.BULK |
| `dsl3580l/out-cold-dsl-decodificata.txt` | 41671 | complete, + AMT RCMTA OBJ.BULK |
| `dsl3580l/out-cold-dsl-merged.txt` | 41671 | complete, + AMT RCMTA OBJ.BULK |
| `tg789vac-v2/cold-sweep.zip` | 2792665 | complete, + AMT ADDRM OBJ.BULK |
| `agcombo/cold-sweep-partial.tar.gz` | 552677 | **missing OBJ** |
| `dsl3580l/full-sweep.zip` | 1707638 | **missing CAL** |

The bus captures (`agcombo/*.bin`, `agcombo/mmio-decoded.zip`,
`archer-t5e/mmiotrace.zip`) have none of the accessor classes by construction:
the audit reports them incomplete, and an absence there means nothing at class
level. Compare them with `--bus` (`test/integration/README.md`).

## Per-class counts

```sh
python3 reverse-tools/check_class_coverage.py \
    --conta AMT.WR,RCMTA.WR,ADDRM.SET,OBJ.BULKW,OBJ.SET,SROMCTL.WR,PHY.WARR,CS.SHM,PHY.FGC
```

| capture | driver | `AMT.WR` | `RCMTA.WR` | `ADDRM.SET` | `OBJ.BULKW` | `OBJ.SET` | `SROMCTL.WR` | `PHY.WARR` | `CS.SHM` | `PHY.FGC` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `agcombo/bss-up.zip` | 7.14.43.21 | 127 | 0 | 60 | 150 | 1 | 0 | 2 | 1 | 94 |
| `agcombo/cold-sweep-partial.tar.gz` | 7.14.43.21 | 0 | 0 | 0 | 0 | 0 | 56 | 0 | 0 | 0 |
| `agcombo/cold-sweep.zip` | 7.14.43.21 | 0 | 0 | 0 | 0 | 0 | 52 | 0 | 26 | 0 |
| `agcombo/hot-sweep.zip` | 7.14.43.21 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 26 | 0 |
| `agcombo/up-nobss-ioctl.zip` | 7.14.43.21 | 245 | 0 | 116 | 253 | 2 | 0 | 0 | 2 | 34 |
| `d6220/cold-sweep.zip` | 7.14.89.14 | 5407 | 0 | 2580 | 6680 | 43 | 0 | 86 | 43 | 2878 |
| `d6220/hot-sweep.zip` | 7.14.89.14 | 10868 | 0 | 5280 | 12236 | 88 | 0 | 0 | 88 | 5824 |
| `dsl3580l/cold01-ch36-bw20.txt` | 6.30.102.7 | 122 | 54 | 0 | 7 | 1 | 2 | 2 | 1 | 0 |
| `dsl3580l/full-sweep.zip` | 6.30.102.7 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `dsl3580l/out-cold-dsl-decodificata.txt` | 6.30.102.7 | 122 | 54 | 0 | 7 | 1 | 4 | 4 | 1 | 0 |
| `dsl3580l/out-cold-dsl-merged.txt` | 6.30.102.7 | 122 | 54 | 0 | 7 | 1 | 4 | 4 | 1 | 0 |
| `tg789vac-v2/cold-sweep.zip` | 7.14.89.14 | 5415 | 0 | 2580 | 7925 | 43 | 86 | 86 | 43 | 3562 |

- **The agcombo sweeps** lack `AMT`, `ADDRM`, `OBJ.BULK*`, `OBJ.SET`,
  `PHY.WARR` and `PHY.FGC`, which the D6220 and TG789vac sweeps (7.14.89) and
  the agcombo's own `bss-up.zip` and `up-nobss-ioctl.zip` carry. **SALAME**: the natural explanation is
  that the agcombo sweeps were taken with an older tracer. Which `hooks[]` each
  archive was taken with is not recorded. The hook plan that `pianifica()` logs
  at insmod is what settles it, and it should be archived with every new
  capture.
- **`SROMCTL.WR`** does not split driver versions: it is absent on the D6220
  sweeps and present on the agcombo (7.14.43), the TG789vac (7.14.89) and the
  DSL (6.30).

## Two legitimate absences

**`RCMTA.WR` cannot appear for the AC core.** Its only caller is
`wlc_set_addrmatch`, which branches on the core revision: below 40 it calls
`wlc_bmac_set_rcmta`, from 40 up `wlc_bmac_write_amt`. That is the threshold of
`brcms_b_set_addrmatch()` in brcmsmac (`D11REV_GE(rev, 40)`). The AC core is
corerev 42. The DSL capture's `RCMTA.WR` belong to the other core.

**`PHY.WARR` has no AC-PHY callers.** None of its 271 call sites in the 7.14
blob is an `*_acphy` function; they are LCN, LCN40, LP, G, A, N and a dozen
generic dispatchers. On the DSL capture all occurrences are on `cpu0`, inside
wl0's attach. The D6220 and TG789vac occurrences have not been attributed to a
core. Do it with the CPU and the named registers, as for the other two-core
boards.

## `PHY.FGC`

`wlc_bmac_phyclk_fgc` (force gated clock) is hooked on `wlc_bmac_phyclk_fgc`
itself, not on the `wlapi_bmac_phyclk_fgc` thunk that all thirteen callers go
through. The stock driver has three AC call sites: `wlc_phy_resetcca_acphy`
once and `wlc_phy_cal_txiqlo_acphy` twice. The port forces the clock only in
`b43_phy_ac_reset_cca()`.

## What the static audit can and cannot say

**The object is enough.** The linked `wl` module extracted from the firmware
(`.chk` → 60-byte Netgear header → big-endian JFFS2 → `wl.ko`) has the same
code as the pre-link object: identical `.text` and `.rodata`, identical
relocations by offset and type. The hook plan comes out identical on both.

**Call sites.** `reverse-tools/audit_hooks.py` replays `pianifica()` on the
object and also counts **call sites**. A hook can be planned on a function
nobody calls: GCC emits the out-of-line copy of a GLOBAL function even when it
inlines it everywhere. On the 7.14 blob:

| symbol | sites | consequence |
| --- | --- | --- |
| `phy_reg_write_wide` | 0 | `PHY.WRW` never appears on this build |
| `wlc_write_amtinfo_by_idx` | 0 | not hooked; its body is the AMT dispatch |

**What the audit cannot say** is which hooks were actually armed on the day of
a capture. Only the insmod log does.

## The `sel` field

`OBJ` operations carry `sel`, the shared-memory window routing:

- the D6220 cold sweep was decoded before the decoder learned the field, so its
  `OBJ` lines lack it;
- the agcombo's all read `sel=0x0000`;
- the agcombo's set of `OBJ` keys is a subset of the D6220's, so on the 385
  shared keys `sel = 0` holds on the D6220 too.

The 480 keys above `0x1000` are shared memory as well (the port zeroes
`0x10f4`–`0x14b2`), with unknown `sel`. Re-decoding restores the field.

The 16 keys at `0x0160`–`0x017f` are `PRSSID`, the probe-response SSID. The
agcombo does not write them because that router does not answer probe
requests, not because of routing.
