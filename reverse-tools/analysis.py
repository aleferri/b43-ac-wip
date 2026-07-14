#!/usr/bin/env python3
"""Three-way analysis of d6220 ch36 trace vs scratch driver code."""
import re, os, sys, glob, csv
from collections import defaultdict, Counter

ROOT = "/home/claude/b43-add-bcm43xx-ac"
TRACE = os.path.join(ROOT, "router-data/d6220/wl-diag-wl1-attach-to-bss-ch36.txt")
SRCDIR = os.path.join(ROOT, "src")
CORR = os.path.join(ROOT, "reverse-output/correlation.csv")

# ── helpers ──────────────────────────────────────────────────────────
sym = {}
for h in glob.glob(SRCDIR+"/*.h") + glob.glob(SRCDIR+"/*.c"):
    for l in open(h, errors="ignore"):
        m = re.match(r"\s*#define\s+(\w+)\s+(0x[0-9a-fA-F]+)\b", l)
        if m: sym.setdefault(m.group(1), int(m.group(2), 16))

def val(t):
    t = t.strip()
    m = re.match(r"^\(u16\)\s*~\s*(.+)$", t)
    if m:
        inner = val(m.group(1))
        return None if inner is None else (~inner) & 0xffff
    if t.startswith("~"):
        inner = val(t[1:])
        return None if inner is None else (~inner) & 0xffff
    if re.fullmatch(r"0x[0-9a-fA-F]+", t): return int(t, 16)
    if re.fullmatch(r"\d+", t): return int(t)
    return sym.get(t)

def split_args(s):
    args = []; depth = 0; cur = ""
    for ch in s:
        if ch == '(': depth += 1; cur += ch
        elif ch == ')':
            if depth == 0: args.append(cur); return args
            depth -= 1; cur += ch
        elif ch == ',' and depth == 0: args.append(cur); cur = ""
        else: cur += ch
    args.append(cur); return args

# ── parse trace ──────────────────────────────────────────────────────
trace_ops = []  # (seq, op, {kv})
TBL_ops = []    # (seq, rw, id, off, len, following_data_writes)
lp = re.compile(r'#(\d+)\s+cpu\d+\s+(\S+)\s+(.*)')
for line in open(TRACE, errors="ignore"):
    m = lp.search(line)
    if not m: continue
    seq, op, rest = int(m.group(1)), m.group(2), m.group(3)
    kv = dict(re.findall(r'(\w+)=(\S+)', rest))
    trace_ops.append((seq, op, kv))

# index trace ops by seq for fast lookup
seq_idx = {t[0]: i for i, t in enumerate(trace_ops)}

# ── parse source: ordered ops per function ───────────────────────────
CALL = re.compile(r"\bb43_(phy|radio)_(write|read|read_log|set|mask|maskset)\s*\(")
SIG = re.compile(r"^[A-Za-z].*\b(b43_[a-z0-9_]+)\s*\([^;]*$")
func_ops = defaultdict(list)  # func -> [(fam, kind, addr, val_or_none)]
for cf in sorted(glob.glob(SRCDIR + "/*.c")):
    func = "?"
    for line in open(cf, errors="ignore"):
        fm = SIG.match(line)
        if fm and "(" in line and ";" not in line:
            func = fm.group(1)
        for m in CALL.finditer(line):
            fam = "PHY" if m.group(1) == "phy" else "RAD"
            kind = m.group(2)
            a = [x.strip() for x in split_args(line[m.end():])]
            if len(a) < 2: continue
            addr = val(a[1])
            if addr is None: continue
            v = None
            if kind == "write" and len(a) >= 3: v = val(a[2])
            elif kind == "set" and len(a) >= 3: v = val(a[2])
            elif kind == "mask" and len(a) >= 3:
                k = val(a[2])
                v = k  # mask = AND value
            elif kind == "maskset" and len(a) >= 4:
                v = val(a[3])
            func_ops[func].append((fam, kind, addr, v))

# ══════════════════════════════════════════════════════════════════════
# ANALYSIS 1: Intra-function order verification
# ══════════════════════════════════════════════════════════════════════
print("=" * 72)
print("ANALYSIS 1: Intra-function write-order verification")
print("=" * 72)

# Build trace write sequence: (seq, fam, addr, val)
trace_writes = []
for seq, op, kv in trace_ops:
    if op in ("PHY.WR", "RAD.WR") and "addr" in kv and kv.get("val") != "UNDEFINED":
        fam = op.split("src")[0]
        trace_writes.append((seq, fam, int(kv["addr"], 16), int(kv["val"], 16)))

# Index by (fam, addr, val) -> list of seq positions
tw_idx = defaultdict(list)
for i, (seq, fam, addr, v) in enumerate(trace_writes):
    tw_idx[(fam, addr, v)].append(i)

# For each function with >= 5 literal writes, find ordered subsequence in trace
inversions_found = []
for func, ops in sorted(func_ops.items()):
    writes = [(fam, addr, v) for fam, kind, addr, v in ops
              if kind == "write" and v is not None]
    if len(writes) < 5: continue

    # greedy forward match
    pos = 0
    matched = []
    for fam, addr, v in writes:
        candidates = [j for j in tw_idx.get((fam, addr, v), []) if j >= pos]
        if candidates:
            matched.append((candidates[0], fam, addr, v))
            pos = candidates[0] + 1

    if len(matched) < 3: continue

    # check for ordering violations (any matched pair where code order != trace order)
    for i in range(len(matched) - 1):
        if matched[i][0] > matched[i+1][0]:
            inversions_found.append((func, matched[i], matched[i+1]))

if inversions_found:
    print(f"\nFOUND {len(inversions_found)} order inversions:")
    for func, a, b in inversions_found:
        print(f"  {func}: trace #{a[0]} ({a[1]} 0x{a[2]:04x}=0x{a[3]:04x}) "
              f"AFTER #{b[0]} ({b[1]} 0x{b[2]:04x}=0x{b[3]:04x}) but code has opposite order")
else:
    print("\nNo write-order inversions found in any function with >= 5 literal writes.")

# count functions checked
checked = sum(1 for f, ops in func_ops.items()
              if sum(1 for _,k,_,v in ops if k == "write" and v is not None) >= 5)
print(f"  ({checked} functions checked)")


# ══════════════════════════════════════════════════════════════════════
# ANALYSIS 2: PHY table content verification
# ══════════════════════════════════════════════════════════════════════
print("\n" + "=" * 72)
print("ANALYSIS 2: PHY table content — trace vs driver tables")
print("=" * 72)

# Extract TBL.WR sequences from trace. After each TBL.WR header, the actual
# data is written through PHY table data registers (0x00d=id, 0x00e=offset,
# 0x00f/0x010/0x011=data). We look for PHY.WR to 0x00f..0x011 following a
# TBL.WR to capture the written values.
tbl_writes = []  # (tbl_id, offset, width_bytes, values[])
i = 0
while i < len(trace_ops):
    seq, op, kv = trace_ops[i]
    if op == "TBL.WR" and "id" in kv and "off" in kv:
        tbl_id = int(kv["id"], 16)
        tbl_off = int(kv["off"], 16)
        tbl_len = int(kv.get("len", "1"))
        # collect data writes that follow (0x00f, 0x010, 0x011)
        vals = []
        j = i + 1
        while j < len(trace_ops):
            s2, o2, k2 = trace_ops[j]
            if o2 == "PHY.WR" and k2.get("addr") in ("0x000f", "0x0010", "0x0011") and k2.get("val") != "UNDEFINED":
                vals.append((int(k2["addr"], 16), int(k2["val"], 16)))
                j += 1
            elif o2 == "PHY.RD":
                j += 1  # skip gate reads
            elif o2 in ("PHY.WR",) and k2.get("addr") in ("0x000d", "0x000e"):
                j += 1  # skip id/offset setup for next entry
            elif o2 == "TBL.WR":
                break  # next table op
            elif o2 == "PHY.MOD" and k2.get("addr") == "0x019e":
                j += 1  # gate ops
            else:
                break
        tbl_writes.append((tbl_id, tbl_off, tbl_len, vals))
        i = j
    else:
        i += 1

# Parse driver table arrays from tables_phy_ac.c
tbl_file = os.path.join(SRCDIR, "tables_phy_ac.c")
driver_tables = {}  # name -> list of int values
if os.path.exists(tbl_file):
    txt = open(tbl_file, errors="ignore").read()
    # Match array definitions: static const uNN name[] = { ... };
    for m in re.finditer(r'static\s+const\s+(?:u8|u16|u32|u64)\s+(\w+)\[\]\s*=\s*\{([^}]+)\}', txt):
        name = m.group(1)
        body = m.group(2)
        values = []
        for v in re.findall(r'0x[0-9a-fA-F]+|\d+', body):
            values.append(int(v, 16) if v.startswith("0x") else int(v))
        driver_tables[name] = values

print(f"\nTrace TBL.WR operations: {len(tbl_writes)}")
print(f"Driver table arrays parsed: {len(driver_tables)}")

# For each trace TBL.WR with data, check if we can find the corresponding
# driver table and compare values
# The mapping from PHY table ID to driver array name is in the table descriptors
# Let's just compare by collecting all (id, offset) -> value from trace
trace_tbl_vals = {}  # (id, offset) -> value (reconstructed from data register writes)
for tbl_id, tbl_off, tbl_len, vals in tbl_writes:
    if not vals: continue
    # Single-entry write: value comes from the data registers
    # Width 8: single 0x011 write; Width 16: single 0x00f write
    # Width 32: 0x00f (lo) + 0x010 (hi); Width 48: 0x00f + 0x010 + 0x011
    for entry_idx in range(tbl_len):
        off = tbl_off + entry_idx
        # Simplified: just record raw data register values per (id, off)
        if entry_idx == 0 and vals:
            trace_tbl_vals[(tbl_id, off)] = vals

print(f"Trace table entries with data captured: {len(trace_tbl_vals)}")

# ══════════════════════════════════════════════════════════════════════
# ANALYSIS 3: Readback-dependent paths (hardcoded vs read)
# ══════════════════════════════════════════════════════════════════════
print("\n" + "=" * 72)
print("ANALYSIS 3: Readback-dependent values — hardcoded risk points")
print("=" * 72)

# Find all reads in the trace
reads = []  # (seq, fam, addr)
for seq, op, kv in trace_ops:
    if op in ("PHY.RD", "RAD.RD") and "addr" in kv:
        fam = op.split("src")[0]
        reads.append((seq, fam, int(kv["addr"], 16)))

print(f"\nTotal reads in trace: {len(reads)}")

# Find reads that are followed by a write to the SAME register (read-modify-write)
# or where the read value feeds a subsequent write to a DIFFERENT register
# Focus: reads NOT followed by a MOD/WR to the same addr (= value consumed elsewhere)
standalone_reads = []  # reads whose value is used for computation, not just RMW
for idx, (seq, fam, addr) in enumerate(reads):
    # check next 3 trace ops for a write to the same addr
    ti = seq_idx.get(seq, -1)
    if ti < 0: continue
    is_rmw = False
    for j in range(ti+1, min(ti+5, len(trace_ops))):
        s2, o2, k2 = trace_ops[j]
        if o2.startswith(fam) and o2.endswith((".WR", ".MOD")) and k2.get("addr") == f"0x{addr:04x}":
            is_rmw = True; break
        if o2.startswith(fam) and o2.endswith(".WR") and k2.get("addr") != f"0x{addr:04x}":
            break  # next write is to different addr
    if not is_rmw:
        standalone_reads.append((seq, fam, addr))

print(f"Standalone reads (value consumed, not simple RMW): {len(standalone_reads)}")

# Now check which of these the driver replaces with a literal
# Parse all read calls from driver code, index by (fam, addr)
driver_reads = set()  # (fam, addr)
READ_CALL = re.compile(r"\bb43_(phy|radio)_(read|read_log)\s*\(")
for cf in sorted(glob.glob(SRCDIR + "/*.c")):
    for line in open(cf, errors="ignore"):
        for m in READ_CALL.finditer(line):
            fam = "PHY" if m.group(1) == "phy" else "RAD"
            a = [x.strip() for x in split_args(line[m.end():])]
            if len(a) >= 2:
                addr = val(a[1])
                if addr is not None:
                    driver_reads.add((fam, addr))

# Driver writes that use a literal where the vendor reads first
trace_read_addrs = Counter((fam, addr) for _, fam, addr in standalone_reads)
hardcoded_risk = []
for (fam, addr), count in trace_read_addrs.most_common():
    has_read = (fam, addr) in driver_reads
    # Check if driver has a write to this addr with a literal value
    has_literal_write = False
    for func, ops in func_ops.items():
        for f2, kind, a2, v2 in ops:
            if f2 == fam and a2 == addr and kind == "write" and v2 is not None:
                has_literal_write = True; break
        if has_literal_write: break

    if not has_read and has_literal_write:
        hardcoded_risk.append((fam, addr, count))
    elif not has_read and not has_literal_write:
        # vendor reads but driver neither reads nor writes
        pass  # might be in an unported section

if hardcoded_risk:
    print(f"\nHARDCODED RISK: vendor reads the register, driver writes a literal instead:")
    for fam, addr, count in sorted(hardcoded_risk, key=lambda x: -x[2]):
        print(f"  {fam} 0x{addr:04x}  (vendor reads {count}x, driver uses literal write)")
else:
    print("\nNo hardcoded-risk registers found (all vendor reads are also read by driver).")

# Also report: vendor reads, driver reads too (= correct readback path)
correct_readback = []
for (fam, addr), count in trace_read_addrs.most_common():
    if (fam, addr) in driver_reads:
        correct_readback.append((fam, addr, count))

print(f"\nCorrect readback paths (vendor reads, driver also reads): {len(correct_readback)}")
for fam, addr, count in correct_readback[:15]:
    print(f"  {fam} 0x{addr:04x}  ({count}x)")

# Vendor reads with NO driver counterpart (read or write) = unported
unported = []
for (fam, addr), count in trace_read_addrs.most_common():
    if (fam, addr) not in driver_reads:
        has_any_write = any(f2 == fam and a2 == addr
                           for func, ops in func_ops.items()
                           for f2, _, a2, _ in ops)
        if not has_any_write:
            unported.append((fam, addr, count))

if unported:
    print(f"\nVendor reads with NO driver counterpart at all (unported): {len(unported)}")
    for fam, addr, count in unported[:15]:
        print(f"  {fam} 0x{addr:04x}  ({count}x)")
