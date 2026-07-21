#!/usr/bin/env python3
"""Find readback-dependent values the port hardcodes.

Flags registers the vendor *reads* (and consumes, i.e. not a plain
read-modify-write of the same address) but the port does *not* read in its
own output, while it *does* write them -- the signature of a literal
hardcode where a derived value belongs (the TX/RX-LPF and DACBUF cases were
exactly this before they were solved).

Both inputs are wl-diag-style traces in the same format
("... FAM.OP addr=0x.... val=0x...."):
  vendor  : a real capture (collapsed/merged is fine)
  harness : the port's output for the matching flow, e.g.
            `./rxiq_trace set_channel d6220`

Usage:
  find_readback_hardcodes.py <vendor_trace> <harness_output> [rmw_window]

rmw_window (default 4): how many ops after a read to look for a write-back
to the same address before calling the read "standalone" (value consumed
elsewhere, not a plain RMW).
"""
import re
import sys

OP = re.compile(r'\b(PHY|RAD)\.(RD|WR|MOD|AND|OR)\s+addr=(0x[0-9a-f]+)')


def parse(path):
    ops = []
    for line in open(path, errors="ignore"):
        m = OP.search(line)
        if m:
            fam, kind, addr = m.group(1), m.group(2), int(m.group(3), 16)
            ops.append((fam, kind, addr))
    return ops


def standalone_reads(ops, window):
    """Reads not followed within `window` ops by a write to the same addr."""
    out = set()
    for i, (fam, kind, addr) in enumerate(ops):
        if kind != "RD":
            continue
        rmw = False
        for j in range(i + 1, min(i + 1 + window, len(ops))):
            f2, k2, a2 = ops[j]
            if f2 == fam and a2 == addr and k2 in ("WR", "MOD", "AND", "OR"):
                rmw = True
                break
        if not rmw:
            out.add((fam, addr))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    window = int(sys.argv[3]) if len(sys.argv) > 3 else 4

    vendor = parse(sys.argv[1])
    port = parse(sys.argv[2])

    vendor_reads = standalone_reads(vendor, window)
    port_reads = {(f, a) for f, k, a in port if k == "RD"}
    port_writes = {(f, a) for f, k, a in port if k in ("WR", "MOD", "AND", "OR")}

    risk = sorted(
        (fam, addr)
        for (fam, addr) in vendor_reads
        if (fam, addr) not in port_reads and (fam, addr) in port_writes
    )

    print(f"vendor standalone reads : {len(vendor_reads)}")
    print(f"port reads              : {len(port_reads)}")
    print(f"port writes             : {len(port_writes)}")
    if risk:
        print(f"\nHARDCODE RISK -- vendor reads, port writes a literal instead:")
        for fam, addr in risk:
            print(f"  {fam} 0x{addr:04x}")
    else:
        print("\nno hardcode-risk registers "
              "(every register the vendor reads, the port reads too)")


if __name__ == "__main__":
    main()
