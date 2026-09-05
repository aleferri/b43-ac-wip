#!/usr/bin/env python3
"""Check the radio-2069 channel table against the vendor sweep segments.

b43_phy_ac_set_channel() only programs ch36 because most of the values it
writes were transcribed from a ch36 capture. The table itself, however,
carries a row per channel, and a sweep holds one cycle per channel -- warm
segments named *-up-chNN-bwNN or the cold ones named coldNN-chNN-bwNN,
so the rows can be checked without tuning anything.

For each channel present in the sweep this compares the table row against
what the vendor actually wrote:

  phy_bw[0..5]  -> PHY  0x0371..0x0376
  chan_raw6     -> RAD  0x011a, 0x011b, 0x0719, 0x0630, 0x065c, 0x0662
  radio_raw[]   -> the RAD registers listed in r2069_chan_writes

A row that matches means tuning that channel would emit the vendor's own
per-channel values; it says nothing about the constants that are still
transcribed from ch36 elsewhere in the flow.

  ./check_channeltab.py <segment-dir>
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

PHY_BW_REGS = [0x371, 0x372, 0x373, 0x374, 0x375, 0x376]


def parse_table():
    """channel -> {'radio_raw': [...], 'chan_raw6': [...], 'phy_bw': [...]}"""
    text = open(os.path.join(SRC, "radio_2069.c")).read()
    rows = {}
    for m in re.finditer(
            r"\.channel\s*=\s*(\d+).*?"
            r"\.radio_raw\s*=\s*\{(.*?)\}.*?"
            r"\.chan_raw6\s*=\s*\{(.*?)\}.*?"
            r"\.phy_bw\s*=\s*\{(.*?)\}", text, re.S):
        nums = lambda s: [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", s)]
        rows[int(m.group(1))] = {
            "radio_raw": nums(m.group(2)),
            "chan_raw6": nums(m.group(3)),
            "phy_bw": nums(m.group(4)),
        }
    return rows


def parse_chan_writes():
    """[(reg, raw_idx)] in the order the driver emits them."""
    text = open(os.path.join(SRC, "radio_2069.c")).read()
    m = re.search(r"r2069_chan_writes\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    out = []
    for reg, idx in re.findall(r"\{\s*(0x[0-9a-fA-F]{4}),\s*(BW\(\d+\)|-?\d+)\s*\}",
                               m.group(1)):
        if idx.startswith("BW("):
            raw = -int(idx[3:-1])
        else:
            raw = int(idx)
        out.append((int(reg, 16), raw))
    return out


def value_of(row, raw_idx):
    if raw_idx >= 0:
        return row["radio_raw"][raw_idx]
    return row["chan_raw6"][-raw_idx - 1]


LINE = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+(PHY|RAD)\.WR\s+"
                  r"addr=0x([0-9a-fA-F]+)\s+val=0x([0-9a-fA-F]+)")


def segment_writes(path, regs_phy, order):
    """Values written by the channel-setup burst.

    Several of these registers are also written by other phases -- the
    prefregs block sets radio 0x065e to 0x0ff4 long before channel setup
    sets it to 0 -- so picking the first or last write in the segment reads
    the wrong phase. The channel-setup writes come out as one contiguous
    run in the order of r2069_chan_writes, so the run is located by that
    signature instead.
    """
    phy = {}
    rad_seq = []
    with open(path, errors="replace") as fh:
        for line in fh:
            m = LINE.match(line)
            if not m:
                continue
            kind, addr, val = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
            if kind == "PHY" and addr in regs_phy:
                phy.setdefault(addr, val)
            elif kind == "RAD":
                rad_seq.append((addr, val))

    want = [reg for reg, _ in order]
    regs_only = [a for a, _ in rad_seq]
    best, best_score = None, -1
    for i in range(len(regs_only) - len(want) + 1):
        if regs_only[i] != want[0]:
            continue
        score = sum(regs_only[i + j] == want[j] for j in range(len(want)))
        if score > best_score:
            best, best_score = i, score
    rad = {}
    if best is not None:
        for j, (reg, _) in enumerate(order):
            addr, val = rad_seq[best + j]
            if addr == reg:
                rad[reg] = val
    return phy, rad, best_score, len(want)


def main():
    segdir = sys.argv[1] if len(sys.argv) > 1 else "."
    table = parse_table()
    writes = parse_chan_writes()
    regs_phy = set(PHY_BW_REGS)

    segs = {}
    for path in sorted(glob.glob(os.path.join(segdir, "*ch*-bw20.txt"))):
        ch = int(re.search(r"ch(\d+)-bw20", path).group(1))
        segs.setdefault(ch, []).append(path)

    print(f"channel table rows: {len(table)}    "
          f"BW20 channels in sweep: {len(segs)}")
    print()
    hdr = (f"{'ch':>5s} {'phy_bw':>10s} {'chan_raw6':>12s} "
           f"{'radio_raw':>12s} {'burst':>10s}")
    print(hdr)
    print("-" * len(hdr))

    totals = {"phy": [0, 0], "c6": [0, 0], "raw": [0, 0]}
    mismatches = []

    for ch in sorted(segs):
        if ch not in table:
            print(f"{ch:5d}   (no table row)")
            continue
        row = table[ch]
        phy, rad, burst_ok, burst_len = segment_writes(segs[ch][0], regs_phy, writes)

        ok = tot = 0
        for i, reg in enumerate(PHY_BW_REGS):
            if reg in phy:
                tot += 1
                if phy[reg] == row["phy_bw"][i]:
                    ok += 1
                else:
                    mismatches.append((ch, "PHY", reg, row["phy_bw"][i], phy[reg]))
        totals["phy"][0] += ok
        totals["phy"][1] += tot
        phy_s = f"{ok}/{tot}"

        c6_ok = c6_tot = raw_ok = raw_tot = 0
        for reg, raw_idx in writes:
            if reg not in rad:
                continue
            want = value_of(row, raw_idx)
            got = rad[reg]
            if raw_idx < 0:
                c6_tot += 1
                c6_ok += want == got
            else:
                raw_tot += 1
                raw_ok += want == got
            if want != got:
                mismatches.append((ch, "RAD", reg, want, got))
        totals["c6"][0] += c6_ok
        totals["c6"][1] += c6_tot
        totals["raw"][0] += raw_ok
        totals["raw"][1] += raw_tot

        flag = "" if (ok == tot and c6_ok == c6_tot and raw_ok == raw_tot) else "  <-"
        print(f"{ch:5d} {phy_s:>10s} {f'{c6_ok}/{c6_tot}':>12s} "
              f"{f'{raw_ok}/{raw_tot}':>12s} {f'{burst_ok}/{burst_len}':>10s}{flag}")

    print("-" * len(hdr))
    print(f"{'tot':>5s} {f'{totals["phy"][0]}/{totals["phy"][1]}':>10s} "
          f"{f'{totals["c6"][0]}/{totals["c6"][1]}':>12s} "
          f"{f'{totals["raw"][0]}/{totals["raw"][1]}':>12s}")

    if mismatches:
        print(f"\n{len(mismatches)} mismatches:")
        for ch, kind, reg, want, got in mismatches:
            print(f"  ch{ch:<4d} {kind} 0x{reg:04x}  table 0x{want:04x}  "
                  f"vendor 0x{got:04x}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
