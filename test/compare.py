#!/usr/bin/env python3
"""
Normalize a wl-diag capture and compare it against the trace emitted by
rxiq_trace. Only op lines are compared (address / value / mask); the
episode number, timestamp, and cpuN prefix are stripped from the vendor
trace before diffing since the test does not simulate scheduling.

Usage:
    compare.py <vendor.txt> <test.out> [OPTIONS]

--range LO:HI     keep only vendor lines whose episode # is within [LO,HI]
--squash-poll     collapse runs of identical PHY.RD 0x0270 into one line
--auto-align     find the offset in <test> that best matches <vendor>[0]
                  by scanning for the first common op; useful when the
                  test flow does a prologue (save-gain, save-tone, ...)
                  that the vendor does not emit. Reports the offset and
                  compares from there.
--align-on OP    like --auto-align but pin to a specific op string
                  (e.g. 'PHY.WR   addr=0x0463 val=0x0027').

Vendor uses PHY.OR/PHY.AND for single-bit sets/clears; they get folded
to PHY.MOD in the format `val=<kernel_mask> mask=<kernel_set>` so the
test's PHY.MOD lines diff cleanly.
"""
import re
import sys
import argparse

VENDOR_LINE = re.compile(
    r'^\s*[0-9.]+\s+#\d+\s+cpu\d+\s+(.+?)\s*(?:;.*)?$'
)
TEST_LINE = re.compile(r'^cpu\d+\s+(.+?)\s*$')

# Vendor uses PHY.OR (set-in) and PHY.AND (clear-in) alongside PHY.MOD.
# Both are folded to the PHY.MOD single-op form that the wrapper emits:
#   phy_set(X) → PHY.MOD val=X mask=0
#   phy_mask(K) → PHY.MOD val=K mask=0    (K = ~clr in kernel terms)
# PHY.OR line looks like "PHY.OR ... val=<current_or_or_in> (set X)":
#   the (set X) group gives the OR-in bits directly → val=X mask=0.
# PHY.AND line "PHY.AND ... val=<masked> (clr X)": clr X gives the
# bits to clear → val=~X (the kmask) mask=0.
PHY_OR  = re.compile(r'^PHY\.OR\s+addr=(0x[0-9a-f]+)\s+val=0x[0-9a-f]+\s*\(set\s+(0x[0-9a-f]+)\)')
PHY_AND = re.compile(r'^PHY\.AND\s+addr=(0x[0-9a-f]+)\s+val=0x[0-9a-f]+\s*\(clr\s+(0x[0-9a-f]+)\)')

def normalize_op(op: str) -> str:
    m = PHY_OR.match(op)
    if m:
        addr, setbits = m.groups()
        return f"PHY.MOD  addr={addr} val={setbits} mask=0x0000"
    m = PHY_AND.match(op)
    if m:
        addr, clrbits = m.groups()
        return f"PHY.MOD  addr={addr} val=0x{(~int(clrbits, 16)) & 0xffff:04x} mask=0x0000"
    return op

def extract_episode(raw: str) -> int:
    m = re.search(r'#(\d+)', raw)
    return int(m.group(1)) if m else -1

def load_vendor(path, ep_range):
    lo, hi = ep_range or (0, 10**9)
    out = []
    for line in open(path):
        m = VENDOR_LINE.match(line)
        if not m:
            continue
        ep = extract_episode(line)
        if not (lo <= ep <= hi):
            continue
        out.append(normalize_op(m.group(1)))
    return out

def load_test(path):
    out = []
    for line in open(path):
        m = TEST_LINE.match(line)
        if m:
            out.append(m.group(1))
    return out

def squash_poll(ops, poll_re=re.compile(r'^PHY\.RD\s+addr=0x0270\s')):
    out = []
    prev = None
    for op in ops:
        if poll_re.match(op) and prev == 'poll':
            continue
        if poll_re.match(op):
            out.append('PHY.RD  addr=0x0270 val=UNDEFINED  [poll]')
            prev = 'poll'
        else:
            out.append(op)
            prev = op
    return out

def find_offset(test, target_op):
    """Return the index of `target_op` in test, or -1."""
    for i, op in enumerate(test):
        if op == target_op:
            return i
    return -1

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('vendor')
    ap.add_argument('test')
    ap.add_argument('--range', help='LO:HI vendor episode range')
    ap.add_argument('--squash-poll', action='store_true')
    ap.add_argument('--auto-align', action='store_true',
                    help='skip test prologue by aligning on vendor[0]')
    ap.add_argument('--align-on', help='align test on this exact op string')
    args = ap.parse_args()

    rng = None
    if args.range:
        lo, hi = args.range.split(':')
        rng = (int(lo), int(hi))

    vendor = load_vendor(args.vendor, rng)
    test = load_test(args.test)

    if args.align_on:
        off = find_offset(test, args.align_on)
        if off < 0:
            print(f"align-on: op not found in test: {args.align_on}")
            return 2
        print(f"aligning test at offset {off} (--align-on)")
        test = test[off:]
    elif args.auto_align and vendor:
        off = find_offset(test, vendor[0])
        if off < 0:
            print(f"auto-align: vendor[0] not found in test: {vendor[0]}")
        else:
            print(f"aligning test at offset {off} (auto: '{vendor[0]}')")
            test = test[off:]

    if args.squash_poll:
        vendor = squash_poll(vendor)
        test = squash_poll(test)

    print(f"vendor: {len(vendor)} ops")
    print(f"test:   {len(test)} ops")

    n = min(len(vendor), len(test))
    mismatches = 0
    for i in range(n):
        if vendor[i] != test[i]:
            mismatches += 1
            if mismatches <= 20:
                print(f"  @{i}:")
                print(f"    vendor: {vendor[i]}")
                print(f"    test:   {test[i]}")
    if len(vendor) != len(test):
        print(f"length differs: vendor={len(vendor)} test={len(test)}")

    if mismatches == 0 and len(vendor) == len(test):
        print("MATCH")
        return 0
    print(f"total mismatches (compared prefix): {mismatches}")
    return 1

if __name__ == '__main__':
    sys.exit(main())
