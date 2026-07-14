#!/usr/bin/env python3
"""Data-flow analysis through table reads: for each TBL.RD, track what the
vendor writes afterwards (to tables or registers), building input→output
equations.  The 'input' is the table read location (whose value we can
recover with `wl phytable` on live hardware); the 'output' is the written
value captured in the trace."""
import re, sys, os
from collections import defaultdict, Counter

TRACE = sys.argv[1] if len(sys.argv) > 1 else \
    "router-data/d6220/wl-diag-wl1-attach-to-bss-ch36.txt"

lp = re.compile(r'#(\d+)\s+cpu\d+\s+(\S+)\s+(.*)')
ops = []
for line in open(TRACE, errors="ignore"):
    m = lp.search(line)
    if m:
        seq, op, rest = int(m.group(1)), m.group(2), m.group(3)
        kv = dict(re.findall(r'(\w+)=(\S+)', rest))
        ops.append((seq, op, kv))

# ── phase 1: extract TBL.RD clusters ────────────────────────────────
# A cluster = contiguous group of TBL.RD (possibly interleaved with
# PHY.RD / PHY.WR to data regs 0x00d-0x011 which are the table access
# mechanism, and gate ops on 0x019e).
TABLE_ACCESS = {"0x000d", "0x000e", "0x000f", "0x0010", "0x0011", "0x019e"}

clusters = []  # [(start_idx, end_idx, [(seq, id, off)])]
i = 0
while i < len(ops):
    seq, op, kv = ops[i]
    if op == "TBL.RD":
        reads = []
        j = i
        while j < len(ops):
            s, o, k = ops[j]
            if o == "TBL.RD":
                reads.append((s, int(k["id"], 16), int(k["off"], 16)))
                j += 1
            elif o in ("PHY.RD", "PHY.WR", "PHY.MOD") and k.get("addr", "") in TABLE_ACCESS:
                j += 1  # table mechanism ops
            else:
                break
        if reads:
            clusters.append((i, j, reads))
        i = j
    else:
        i += 1

print(f"TBL.RD clusters found: {len(clusters)}")
print()

# ── phase 2: for each cluster, collect writes in the window AFTER ────
# Window = next 50 ops after the cluster ends (enough for the computation
# that uses the read values).

WRITE_OPS = {"PHY.WR", "RAD.WR", "PHY.MOD", "RAD.MOD", "PHY.OR", "PHY.AND",
             "TBL.WR"}
equations = []  # (read_list, write_list)

for start_idx, end_idx, reads in clusters:
    writes = []
    # Collect writes in window after cluster
    window = 80
    j = end_idx
    tbl_wr_data = []  # for TBL.WR, collect subsequent data reg writes
    while j < min(end_idx + window, len(ops)):
        s, o, k = ops[j]
        if o == "TBL.RD":
            break  # next read cluster starts
        if o == "TBL.WR":
            tid = int(k.get("id", "0"), 16)
            off = int(k.get("off", "0"), 16)
            length = int(k.get("len", "1"))
            # collect data from following PHY.WR to data regs
            data_vals = []
            jj = j + 1
            while jj < len(ops) and len(data_vals) < length:
                ss, oo, kk = ops[jj]
                if oo == "PHY.WR" and kk.get("addr") in ("0x000f", "0x0010", "0x0011") \
                   and kk.get("val") != "UNDEFINED":
                    data_vals.append((int(kk["addr"], 16), int(kk["val"], 16)))
                    jj += 1
                elif oo == "PHY.RD" or (oo == "PHY.WR" and kk.get("addr") in ("0x000d", "0x000e")):
                    jj += 1
                elif oo == "PHY.MOD" and kk.get("addr") == "0x019e":
                    jj += 1
                else:
                    break
            writes.append(("TBL", s, tid, off, length, data_vals))
            j = jj
        elif o in ("PHY.WR", "RAD.WR") and k.get("val") != "UNDEFINED" \
             and k.get("addr", "") not in TABLE_ACCESS:
            fam = o.split(".")[0]
            addr = int(k["addr"], 16)
            v = int(k["val"], 16)
            writes.append(("REG", s, fam, addr, v))
            j += 1
        elif o in ("PHY.MOD", "RAD.MOD") and k.get("val") != "UNDEFINED" \
             and k.get("addr", "") not in TABLE_ACCESS:
            fam = o.split(".")[0]
            addr = int(k["addr"], 16)
            v = int(k["val"], 16)
            mask = int(k.get("mask", "0"), 16)
            writes.append(("MOD", s, fam, addr, v, mask))
            j += 1
        else:
            j += 1

    if writes:
        equations.append((reads, writes))

# ── phase 3: report ──────────────────────────────────────────────────
print("=" * 72)
print("DATA FLOW: TBL.RD → subsequent writes (input→output equations)")
print("=" * 72)

for reads, writes in equations:
    # Summarize reads
    read_ids = Counter(tid for _, tid, _ in reads)
    read_summary = ", ".join(f"tbl 0x{tid:02x}({n})" for tid, n in read_ids.most_common())
    seq_range = f"#{reads[0][0]}-#{reads[-1][0]}"

    print(f"\nREAD {seq_range}: {read_summary}")

    # Detail: unique (id, off) reads
    for seq, tid, off in reads[:6]:
        print(f"  <- TBL.RD  id=0x{tid:02x} off=0x{off:04x}  (#{seq})")
    if len(reads) > 6:
        print(f"  ... ({len(reads) - 6} more reads)")

    # Writes
    for w in writes[:12]:
        if w[0] == "REG":
            _, s, fam, addr, v = w
            print(f"  -> {fam}.WR  addr=0x{addr:04x} val=0x{v:04x}  (#{s})")
        elif w[0] == "MOD":
            _, s, fam, addr, v, mask = w
            print(f"  -> {fam}.MOD addr=0x{addr:04x} val=0x{v:04x} mask=0x{mask:04x}  (#{s})")
        elif w[0] == "TBL":
            _, s, tid, off, length, data = w
            if data:
                val_str = " ".join(f"0x{v:04x}" for _, v in data[:4])
                if len(data) > 4: val_str += f" ...({len(data)} total)"
            else:
                val_str = "(no data captured)"
            print(f"  -> TBL.WR  id=0x{tid:02x} off=0x{off:04x} len={length}  [{val_str}]  (#{s})")
    if len(writes) > 12:
        print(f"  ... ({len(writes) - 12} more writes)")

# ── phase 4: cross-reference read→write value pairs ─────────────────
# Where a TBL.RD at (id, off) is followed by a write to the SAME table
# at a DIFFERENT offset, the written value likely derives from the read.
print()
print("=" * 72)
print("SAME-TABLE READ→WRITE pairs (strongest FN candidates)")
print("=" * 72)

for reads, writes in equations:
    read_ids_set = set(tid for _, tid, _ in reads)
    for w in writes:
        if w[0] != "TBL": continue
        _, s, tid, off, length, data = w
        if tid in read_ids_set:
            read_offs = [(seq, roff) for seq, rtid, roff in reads if rtid == tid]
            if data:
                val_str = " ".join(f"0x{v:04x}" for _, v in data[:3])
            else:
                val_str = "?"
            print(f"  tbl 0x{tid:02x}: read offs {','.join(f'0x{o:04x}' for _,o in read_offs[:4])}"
                  f" → write off 0x{off:04x} len={length} val=[{val_str}]")

# ── phase 5: register-only chains (RD reg → WR reg) ─────────────────
print()
print("=" * 72)
print("REG READ→WRITE chains (vendor reads reg, computes, writes result)")
print("=" * 72)

# Find PHY.RD/RAD.RD that are NOT table-mechanism and NOT simple RMW,
# followed by writes within 10 ops
reg_chains = []
for i, (seq, op, kv) in enumerate(ops):
    if op not in ("PHY.RD", "RAD.RD"): continue
    if kv.get("addr", "") in TABLE_ACCESS: continue
    fam = op.split(".")[0]
    addr = int(kv["addr"], 16)

    # Look ahead for writes (skip same-addr RMW)
    chain_writes = []
    for j in range(i+1, min(i+15, len(ops))):
        s2, o2, k2 = ops[j]
        if o2.endswith(".RD"): continue  # more reads
        if o2 in ("PHY.WR", "RAD.WR") and k2.get("val") != "UNDEFINED" \
           and k2.get("addr", "") not in TABLE_ACCESS:
            wa = int(k2["addr"], 16)
            wv = int(k2["val"], 16)
            wf = o2.split(".")[0]
            if wf == fam and wa == addr:
                break  # RMW, not interesting
            chain_writes.append((s2, wf, wa, wv))
        elif o2 in ("PHY.MOD", "RAD.MOD") and k2.get("addr", "") not in TABLE_ACCESS:
            wa = int(k2["addr"], 16)
            wf = o2.split(".")[0]
            if wf == fam and wa == addr:
                break  # RMW
            # MOD to different addr = interesting
            wv = int(k2.get("val", "0"), 16)
            chain_writes.append((s2, wf, wa, wv))
        elif o2.startswith("TBL."):
            break  # table op = different context
        if len(chain_writes) >= 3:
            break

    if chain_writes:
        reg_chains.append((seq, fam, addr, chain_writes))

# Deduplicate and show unique patterns
seen = set()
for seq, fam, addr, chain in reg_chains:
    key = (fam, addr, tuple((wf, wa) for _, wf, wa, _ in chain))
    if key in seen: continue
    seen.add(key)
    count = sum(1 for s, f, a, c in reg_chains
                if f == fam and a == addr and
                tuple((wf, wa) for _, wf, wa, _ in c) == tuple((wf, wa) for _, wf, wa, _ in chain))
    wr_str = " → ".join(f"{wf} 0x{wa:04x}=0x{wv:04x}" for _, wf, wa, wv in chain[:3])
    print(f"  {fam} 0x{addr:04x} read → {wr_str}  ({count}x)")
