#!/usr/bin/env python3
"""
Generate todo_leftovers_X_Y() C stub functions for every contiguous run of
unported (NONE) operations in the attach-to-bss ch36 trace.

Primary input : reverse-output/correlation-ch36.csv  (correct AND/OR in MODs)
Secondary     : reverse-output/correlation.csv        (down-to-bss-up, used
                only to extract the 16 WR-only init ops absent from ch36)

Writes: src/todo_leftovers.c
        src/todo_leftovers.h
        src/wiring_guide.txt
        src/wiring_harness.c

Each stub transcribes the exact register operations from the d6220/BCM4352
ch36 capture as b43_* API calls.
"""

import csv
import os
import sys
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
CSV_PATH = os.path.join(ROOT, "reverse-output/correlation-ch36.csv")
OUT_C = os.path.join(ROOT, "src/todo_leftovers.c")
OUT_H = os.path.join(ROOT, "src/todo_leftovers.h")
OUT_WIRE = os.path.join(ROOT, "src/wiring_guide.txt")

# ---------------------------------------------------------------------------
# 1. Parse CSV
# ---------------------------------------------------------------------------
rows = list(csv.DictReader(open(CSV_PATH)))
print(f"Loaded {len(rows)} trace ops from correlation.csv")

# ---------------------------------------------------------------------------
# 1b. Reclassify false-positives
#
#     The correlator only tracks b43_phy_*/b43_radio_* call sites.
#     Three classes of trace ops land on NONE despite being handled:
#
#     A. MAC.MCTRL / MAC.MHF — b43_mac_suspend/enable and b43_mhf(),
#        managed by the b43 core, not the PHY driver. ALL of them are
#        reclassified; the driver already calls b43_mac_suspend/enable
#        in set_channel.
#
#     B. PMU/GPIO ops implemented via bcma_chipco_* — identified by
#        matching (op, addr, val, mask) against phy_ac.c lines 2040-2154.
#
#     C. No-ops (mask=0) and bcma_pmu_pll_init-covered PLL writes.
#
#     Genuinely unported: PMU.RC clear bit 1 at startup (seq varies per
#     trace; identified by op=PMU.RC val=0 mask=0x02).
# ---------------------------------------------------------------------------
n_reclass = 0
for r in rows:
    if r["match"] != "NONE":
        continue
    op = r["op"]
    val = r["val"]
    mask = r["mask"]
    addr = r["addr"]

    # A. All MAC ops → b43 core
    if op.startswith("MAC."):
        r["match"] = "B43_CORE"
        r["driver_site(s)"] = "b43_mac_suspend_enable"
        n_reclass += 1
        continue

    # B. PMU/GPIO already ported in phy_ac.c
    if op == "PMU.RC" and addr == "0x0000":
        if mask == "0x01f00000":
            # phy_ac.c:2040 regctl_maskset
            r["match"] = "BCMA_OK"
            r["driver_site(s)"] = "bcma_chipco_already_ported"
            n_reclass += 1; continue
        if val == "0x00000002" and mask == "0x00000002":
            # phy_ac.c:2154 enable strobe
            r["match"] = "BCMA_OK"
            r["driver_site(s)"] = "bcma_chipco_already_ported"
            n_reclass += 1; continue
        if val == "0x00000000" and mask == "0x00000002":
            # phy_ac.c: clear bit 1 in channel_switch_prep (ch36 #32404)
            r["match"] = "BCMA_OK"
            r["driver_site(s)"] = "bcma_chipco_already_ported"
            n_reclass += 1; continue

    if op == "GPIO.CTL":
        if mask == "0x0000ffff" or mask == "0x00000000":
            # mask=0xffff → phy_ac.c:2042; mask=0 → no-op
            tag = "bcma_chipco_already_ported" if mask == "0x0000ffff" else "noop_mask_zero"
            r["match"] = "BCMA_OK"
            r["driver_site(s)"] = tag
            n_reclass += 1; continue

    if op == "GPIO.OUT":
        if mask in ("0x00000004", "0x00000400"):
            # phy_ac.c:2112 (fase1) or 2131 (fase2)
            r["match"] = "BCMA_OK"
            r["driver_site(s)"] = "bcma_chipco_already_ported"
            n_reclass += 1; continue

    # C. PMU.PLL regs 2/3 → bcma_pmu_pll_init
    if op == "PMU.PLL" and addr in ("0x0002", "0x0003"):
        r["match"] = "BCMA_OK"
        r["driver_site(s)"] = "bcma_pmu_pll_init"
        n_reclass += 1; continue

print(f"Reclassified {n_reclass} MAC/PMU/GPIO false-positives")

# ---------------------------------------------------------------------------
# 1c. Reclassify RMW read-aheads
#
#     b43_*_maskset does: read, modify, write. The trace logs three
#     separate ops. The correlator tags the write-back (RMW-WB) but not
#     the read-ahead. A NONE read followed within 3 rows by a matched
#     MOD on the same (family, address) — possibly with an intervening
#     RMW-WB of the previous maskset — is the read half of that maskset.
# ---------------------------------------------------------------------------
MATCHED = {"EXACT", "UNIQUE", "MULTI", "RMW-WB"}
n_rmw_ra = 0
for k in range(len(rows)):
    r = rows[k]
    if r["match"] != "NONE" or not r["op"].endswith(".RD"):
        continue
    fam_r = r["op"].split(".")[0]
    # Look ahead up to 3 rows for a matched MOD on same (fam, addr)
    for lookahead in range(1, min(4, len(rows) - k)):
        nxt = rows[k + lookahead]
        fam_n = nxt["op"].split(".")[0]
        if (fam_r == fam_n and nxt["op"].endswith(".MOD")
                and nxt["match"] in MATCHED and r["addr"] == nxt["addr"]):
            r["match"] = "RMW-RA"
            r["driver_site(s)"] = nxt.get("driver_site(s)", "")
            n_rmw_ra += 1
            break
        # Skip past RMW-WB and matched WR on the same family
        if nxt["match"] in (MATCHED | {"RMW-WB", "NONE"}) and nxt["op"].endswith(".WR"):
            continue
        # Anything else breaks the chain
        break

print(f"Reclassified {n_rmw_ra} RMW read-aheads")

# ---------------------------------------------------------------------------
# 1d. [removed] dtbu-only injection no longer needed
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 2. Build timeline: alternating OK / NONE segments
# ---------------------------------------------------------------------------
def dominant_func(chunk):
    """Extract the dominant driver function from a matched chunk."""
    funcs = Counter()
    for r in chunk:
        site = r.get("driver_site(s)", "")
        if not site:
            continue
        for s in site.split(";"):
            s = s.strip()
            # format: "file.c:123 func_name" or just "func_name"
            parts = s.rsplit(" ", 1)
            fn = parts[-1] if parts else s
            loc = parts[0] if len(parts) > 1 else ""
            funcs[(fn, loc)] += 1
    if not funcs:
        return ("?", "")
    best = funcs.most_common(1)[0][0]
    return best

timeline = []  # list of (kind, info, chunk)
# kind = "OK" | "NONE"
# info = (func_name, location) for OK, None for NONE
i = 0
while i < len(rows):
    r = rows[i]
    is_none = (r["match"] == "NONE")
    j = i
    while j < len(rows) and (rows[j]["match"] == "NONE") == is_none:
        j += 1
    chunk = rows[i:j]
    if is_none:
        timeline.append(("NONE", None, chunk))
    else:
        timeline.append(("OK", dominant_func(chunk), chunk))
    i = j

none_runs = [(idx, t) for idx, t in enumerate(timeline) if t[0] == "NONE"]
print(f"Timeline: {len(timeline)} segments, {len(none_runs)} NONE runs "
      f"({sum(len(t[2]) for _, t in none_runs)} ops)")

# ---------------------------------------------------------------------------
# 3. C code emitters for each trace op type
# ---------------------------------------------------------------------------
def hex16(s):
    """Normalise a hex string to 0xNNNN (4 digits)."""
    if not s or s == "UNDEFINED":
        return None
    v = int(s, 16) if s.startswith("0x") else int(s)
    return f"0x{v:04x}"

def hex32(s):
    if not s or s == "UNDEFINED":
        return None
    v = int(s, 16) if s.startswith("0x") else int(s)
    return f"0x{v:08x}"

def emit_op(r):
    """Return a C statement string for one trace op, or a comment if
    the op type is not directly translatable."""
    op = r["op"]
    addr = r["addr"]
    val = r["val"]
    mask = r["mask"]
    seq = r["trace_seq"]

    if op == "PHY.WR":
        a, v = hex16(addr), hex16(val)
        if a and v:
            return f"\tb43_phy_write(dev, {a}, {v});"
        return f"\t/* PHY.WR #{seq}: addr={addr} val={val} (incomplete) */"

    if op == "PHY.RD":
        a = hex16(addr)
        if a:
            return f"\t(void)b43_phy_read(dev, {a}); /* RD #{seq} */"
        return f"\t/* PHY.RD #{seq}: addr={addr} (no addr) */"

    if op == "PHY.MOD":
        a, v, m = hex16(addr), hex16(val), hex16(mask)
        if a and v and m:
            return f"\tb43_phy_maskset(dev, {a}, (u16)~{m}, {v});"
        return f"\t/* PHY.MOD #{seq}: addr={addr} val={val} mask={mask} (incomplete) */"

    if op == "RAD.WR":
        a, v = hex16(addr), hex16(val)
        if a and v:
            return f"\tb43_radio_write(dev, {a}, {v});"
        return f"\t/* RAD.WR #{seq}: addr={addr} val={val} (incomplete) */"

    if op == "RAD.RD":
        a = hex16(addr)
        if a:
            return f"\t(void)b43_radio_read(dev, {a}); /* RD #{seq} */"
        return f"\t/* RAD.RD #{seq}: addr={addr} (no addr) */"

    if op == "RAD.MOD":
        a, v, m = hex16(addr), hex16(val), hex16(mask)
        if a and v and m:
            return f"\tb43_radio_maskset(dev, {a}, (u16)~{m}, {v});"
        return f"\t/* RAD.MOD #{seq}: addr={addr} val={val} mask={mask} (incomplete) */"

    if op == "PMU.RC":
        a, v, m = hex16(addr), hex32(val), hex32(mask)
        if a and v and m:
            return (f"\tbcma_chipco_regctl_maskset("
                    f"&dev->dev->bdev->bus->drv_cc, "
                    f"{a}, ~{m}, {v});")
        return f"\t/* PMU.RC #{seq}: addr={addr} val={val} mask={mask} */"

    if op == "PMU.PLL":
        a, v, m = hex16(addr), hex32(val), hex32(mask)
        return f"\t/* PMU.PLL #{seq}: addr={a or addr} val={v or val} mask={m or mask} */"

    if op == "GPIO.CTL":
        v, m = hex32(val), hex32(mask)
        if v is not None and m is not None:
            return (f"\tbcma_chipco_gpio_control("
                    f"&dev->dev->bdev->bus->drv_cc, {m}, {v});")
        return f"\t/* GPIO.CTL #{seq}: val={val} mask={mask} */"

    if op == "GPIO.OUT":
        v, m = hex32(val), hex32(mask)
        if v is not None and m is not None:
            return (f"\tbcma_chipco_gpio_out("
                    f"&dev->dev->bdev->bus->drv_cc, {m}, {v});")
        return f"\t/* GPIO.OUT #{seq}: val={val} mask={mask} */"

    if op == "GPIO.OE":
        v, m = hex32(val), hex32(mask)
        if v is not None and m is not None:
            return (f"\tbcma_chipco_gpio_outen("
                    f"&dev->dev->bdev->bus->drv_cc, {m}, {v});")
        return f"\t/* GPIO.OE #{seq}: val={val} mask={mask} */"

    return f"\t/* {op} #{seq}: addr={addr} val={val} mask={mask} */"

# ---------------------------------------------------------------------------
# 4. Classify each NONE run: determine context and per-core patterns
# ---------------------------------------------------------------------------
def addr_norm(addr_str):
    """Normalise a hex address mod 0x200 for per-core detection."""
    try:
        return int(addr_str, 16) % 0x200
    except (ValueError, TypeError):
        return None

def classify_run(chunk):
    """Return a brief tag: DETERMINISTIC or DATA-DEP(cal)."""
    rd = sum(1 for r in chunk if r["op"].endswith(".RD"))
    wm = sum(1 for r in chunk if r["op"].endswith((".WR", ".MOD")))
    if rd <= max(2, 0.10 * (rd + wm)):
        return "DET"
    return "CAL"

def top_regs(chunk, n=4):
    """Return the top N normalised register addresses."""
    c = Counter()
    for r in chunk:
        a = addr_norm(r["addr"])
        if a is not None:
            fam = r["op"].split(".")[0]
            c[(fam, a)] += 1
    return [f"{f}{a:03x}" for (f, a), _ in c.most_common(n)]

# ---------------------------------------------------------------------------
# 5. Generate the files
# ---------------------------------------------------------------------------
c_lines = []
h_lines = []
w_lines = []

# C file header
c_lines.append("/* SPDX-License-Identifier: GPL-2.0-or-later")
c_lines.append("/*")
c_lines.append(" * Auto-generated leftover stubs for the down->bss-up trace.")
c_lines.append(" * Each function transcribes one contiguous NONE run from")
c_lines.append(" * correlation.csv (d6220 BCM4352, ch36, 5 GHz).")
c_lines.append(" *")
c_lines.append(" * Values are deterministic snapshots; DATA-DEP(cal) stubs")
c_lines.append(" * will need algorithmic replacement once the calibration")
c_lines.append(" * routines are ported.")
c_lines.append(" *")
c_lines.append(" * Generated by generate_leftovers.py — do not edit by hand.")
c_lines.append(" */")
c_lines.append("")
c_lines.append('#include "b43.h"')
c_lines.append('#include "phy_ac.h"')
c_lines.append('#include "radio_2069.h"')
c_lines.append('#include "todo_leftovers.h"')
c_lines.append("")

# H file header
h_lines.append("/* SPDX-License-Identifier: GPL-2.0 */")
h_lines.append("#ifndef B43_TODO_LEFTOVERS_H_")
h_lines.append("#define B43_TODO_LEFTOVERS_H_")
h_lines.append("")
h_lines.append("struct b43_wldev;")
h_lines.append("")

# Wiring guide header
w_lines.append("=" * 76)
w_lines.append("WIRING GUIDE — todo_leftovers_X_Y() insertion points")
w_lines.append("=" * 76)
w_lines.append("")
w_lines.append("Each line: stub name, # ops, classification, preceding function,")
w_lines.append("           following function.")
w_lines.append("When prev_fn == next_fn, the stub belongs INSIDE that function,")
w_lines.append("between the matched ops before and after.")
w_lines.append("")

stats = Counter()  # classification stats

for run_idx, (tl_idx, (_, _, chunk)) in enumerate(none_runs):
    first_seq = chunk[0]["trace_seq"]
    last_seq = chunk[-1]["trace_seq"]
    n_ops = len(chunk)
    cls = classify_run(chunk)
    regs = top_regs(chunk)
    stats[cls] += 1

    # Context: preceding and following OK segments
    prev_fn = "START"
    prev_loc = ""
    if tl_idx > 0 and timeline[tl_idx - 1][0] == "OK":
        prev_fn, prev_loc = timeline[tl_idx - 1][1]

    next_fn = "END"
    next_loc = ""
    if tl_idx < len(timeline) - 1 and timeline[tl_idx + 1][0] == "OK":
        next_fn, next_loc = timeline[tl_idx + 1][1]

    fname = f"todo_leftovers_{first_seq}_{last_seq}"

    # Wiring guide entry
    w_lines.append(f"{fname}()  [{cls}] {n_ops} ops  regs=[{','.join(regs)}]")
    w_lines.append(f"    prev: {prev_fn} {prev_loc}")
    w_lines.append(f"    next: {next_fn} {next_loc}")
    if prev_fn == next_fn and prev_fn != "START" and prev_fn != "END":
        w_lines.append(f"    WIRE: inside {prev_fn}")
    else:
        w_lines.append(f"    WIRE: between {prev_fn} -> {next_fn}")
    w_lines.append("")

    # H declaration
    h_lines.append(f"void {fname}(struct b43_wldev *dev);")

    # C function
    c_lines.append(f"/* [{cls}] {n_ops} ops, regs ~{','.join(regs)}")
    c_lines.append(f" * prev: {prev_fn}")
    c_lines.append(f" * next: {next_fn} */")
    c_lines.append(f"void {fname}(struct b43_wldev *dev)")
    c_lines.append("{")
    for r in chunk:
        c_lines.append(emit_op(r))
    c_lines.append("}")
    c_lines.append("")

# H file footer
h_lines.append("")
h_lines.append("#endif /* B43_TODO_LEFTOVERS_H_ */")

# Wiring guide footer
w_lines.append("=" * 76)
w_lines.append(f"TOTALS: {len(none_runs)} stubs, "
               f"{sum(len(t[2]) for _, t in none_runs)} ops")
for cls, n in stats.most_common():
    w_lines.append(f"  {cls}: {n} stubs")
w_lines.append("=" * 76)

# ---------------------------------------------------------------------------
# 5b. Generate trace-order harness (wiring_harness.c)
#     Groups the flat timeline into major sections matching the high-level
#     call flow of set_channel / rfkill, emitting calls to existing functions
#     interleaved with stub calls.
# ---------------------------------------------------------------------------
OUT_HARNESS = os.path.join(ROOT, "src/wiring_harness.c")
har = []
har.append("/* SPDX-License-Identifier: GPL-2.0-or-later")
har.append("/*")
har.append(" * Trace-order call harness: every OK segment and NONE stub in the exact")
har.append(" * sequence observed in the d6220/BCM4352 down->bss-up trace.")
har.append(" *")
har.append(" * This is NOT meant to compile as-is; it is a roadmap showing")
har.append(" * the interleaving of existing functions and leftover stubs.")
har.append(" * Each line carries its trace_seq range for cross-referencing.")
har.append(" *")
har.append(" * Generated by generate_leftovers.py")
har.append(" */")
har.append("")
har.append('#include "todo_leftovers.h"')
har.append("")
har.append("/* --- trace-order harness --- */")
har.append("void todo_harness_full_sequence(struct b43_wldev *dev)")
har.append("{")

for kind, info, chunk in timeline:
    first = chunk[0]["trace_seq"]
    last = chunk[-1]["trace_seq"]
    n = len(chunk)
    if kind == "NONE":
        fname = f"todo_leftovers_{first}_{last}"
        # Classify
        rd = sum(1 for r in chunk if r["op"].endswith(".RD"))
        wm = sum(1 for r in chunk if r["op"].endswith((".WR", ".MOD")))
        cls_tag = "DET" if rd <= max(2, 0.10 * (rd + wm)) else "CAL"
        har.append(f"\t{fname}(dev);  "
                   f"/* #{first}-{last} [{cls_tag}] {n} ops */")
    else:
        fn = info[0] if isinstance(info, tuple) else info
        har.append(f"\t/* OK #{first}-{last}: {fn}() [{n} ops] */")

har.append("}")
har.append("")

# Also produce per-function wiring: group consecutive timeline entries
# that share the same high-level context into sections.
def nearest_hlfn(tl_idx, direction):
    """Walk from tl_idx looking for a non-trivial matched function."""
    step = -1 if direction == "prev" else 1
    k = tl_idx + step
    trivial = {"?", "b43_phy_ac_op_radio_write"}
    while 0 <= k < len(timeline):
        if timeline[k][0] == "OK":
            fn = timeline[k][1]
            if isinstance(fn, tuple):
                fn = fn[0]
            if fn not in trivial:
                return fn
        k += step
    return "START" if direction == "prev" else "END"

# Group into sections delimited by high-level function transitions
sections = OrderedDict()  # (prev_hl, next_hl) -> list of timeline entries
for tl_idx, entry in enumerate(timeline):
    prev_hl = nearest_hlfn(tl_idx, "prev")
    next_hl = nearest_hlfn(tl_idx, "next")
    key = (prev_hl, next_hl)
    sections.setdefault(key, []).append(entry)

har.append("")
har.append("/*")
har.append(" * === SECTION SUMMARY ===")
har.append(" * Groups by nearest high-level function context,")
har.append(" * for targeted wiring into each sub-function.")
har.append(" */")
har.append("")

for (prev_hl, next_hl), entries in sections.items():
    none_entries = [e for e in entries if e[0] == "NONE"]
    if not none_entries:
        continue
    n_stubs = len(none_entries)
    n_ops = sum(len(e[2]) for e in none_entries)
    section_name = (prev_hl if prev_hl == next_hl
                    else f"{prev_hl}_to_{next_hl}")
    har.append(f"/* SECTION: {section_name}")
    har.append(f" * {n_stubs} stubs, {n_ops} NONE ops */")
    for entry in entries:
        kind = entry[0]
        chunk = entry[2]
        first = chunk[0]["trace_seq"]
        last = chunk[-1]["trace_seq"]
        n = len(chunk)
        if kind == "NONE":
            fname = f"todo_leftovers_{first}_{last}"
            har.append(f"/*  STUB  {fname}(dev);  [{n} ops] */")
        else:
            fn = entry[1]
            if isinstance(fn, tuple):
                fn = fn[0]
            har.append(f"/*  OK    {fn}()  [{n} ops] */")
    har.append("")

with open(OUT_HARNESS, "w") as f:
    f.write("\n".join(har) + "\n")
print(f"Written {OUT_HARNESS} ({len(har)} lines)")

# ---------------------------------------------------------------------------
# 6. Write files
# ---------------------------------------------------------------------------
with open(OUT_C, "w") as f:
    f.write("\n".join(c_lines) + "\n")
print(f"Written {OUT_C} ({len(c_lines)} lines)")

with open(OUT_H, "w") as f:
    f.write("\n".join(h_lines) + "\n")
print(f"Written {OUT_H} ({len(h_lines)} lines)")

with open(OUT_WIRE, "w") as f:
    f.write("\n".join(w_lines) + "\n")
print(f"Written {OUT_WIRE} ({len(w_lines)} lines)")

print(f"\nDone: {len(none_runs)} stubs generated.")
for cls, n in stats.most_common():
    print(f"  {cls}: {n}")
