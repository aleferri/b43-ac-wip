#!/usr/bin/env python3
"""Which port phases the vendor does not execute above a threshold.

This is what decides what to gate: above 5250 MHz the stock driver skips some
phases, the port still runs them, and the extra ops weigh on the score. The
question is which phases, and the answer is read by comparing how many times
the vendor touches each phase's addresses below and above the threshold.

Two methods, and the difference between them is the most important result this
tool has produced:

  every address (default)
      For each phase, ALL the addresses it emits are examined. The phase is a
      gate candidate if every one of its own addresses sits at zero above the
      threshold. If only some of them do, what was found is a branch inside
      the phase, not the phase.

  --witness-only
      A single address per phase is examined, the one that only that phase
      touches in the port. It reads faster but answers a weaker question, and
      the counterexample is idle_tssi_meas: its three exclusive registers sit
      at zero above 5250 MHz while the phase runs, with 192 reads of PHY
      0x0013 against 198. **An exclusive witness at zero proves the register
      absent, not the phase.** It stays useful because it also says which
      phases have no register of their own, which is the limit of the method
      and not a result: those cannot be told apart by address.

Ubiquitous addresses -- the table ports 0x000d-0x0010 and the gate 0x019e --
discriminate nothing and are excluded from the report: for a phase working
through a data port the identity is the table, which is examined with
--tables.

The port trace must be produced with AC_FN_MARKERS=1; passing `flow:board`
instead runs it.

Usage:
  phase_absent.py <port|flow:board> <vendor-below> <vendor-above>
                  [--witness-only] [--min N] [--fn NAME] [--tables]
"""
import argparse
import collections
import os
import re
import sys

import tracelib as T

# Ports and gates every phase goes through: always present, so they say
# nothing about a phase being absent. The addresses are in tracelib's
# normalized form, without the zero padding.
UBIQUITOUS = {('PHY', '0xd'), ('PHY', '0xe'), ('PHY', '0xf'),
              ('PHY', '0x10'), ('PHY', '0x19e')}

TBL = re.compile(r"(TBL\.(?:WR|RD)) +(id=0x[0-9a-f]+) +(off=0x[0-9a-f]+)")


def table_key(op):
    m = TBL.search(op)
    return (m.group(2), m.group(3)) if m else None


def shown(k):
    """The key as the tracer writes it: four-digit addresses.

    Internally the addresses are normalized without zero padding, because the
    two sides print them differently and the keys have to match; in the report
    they are put back, so they are searchable in the captures.
    """
    space, a = k
    if a.startswith('0x'):
        return space, '0x%04x' % int(a, 16)
    return k


def port_trace(arg):
    if ":" in arg and not os.path.exists(arg):
        flow, board = arg.split(":", 1)
        return T.write_temp(T.run_port(flow, board))
    return arg


def print_detail(emitted, below, above, fn):
    e = emitted.get(fn)
    if not e:
        print(f"{fn}: no op attributed; does the name exist in the markers?")
        return 1
    print(f"{fn}: {len(e)} keys, {sum(e.values())} ops in the port")
    print(f"  {'key':<22} {'port':>5} {'below':>7} {'above':>7}")
    for k, n in e.most_common():
        u = ' (ubiquitous)' if k in UBIQUITOUS else ''
        sp, a = shown(k)
        print(f"  {sp:<6}{a:<16} {n:>5} {below.get(k, 0):>7} "
              f"{above.get(k, 0):>7}{u}")
    return 0


def mode_witness(emitted, below, above):
    """One address per phase: the one only that phase touches in the port."""
    owners = collections.defaultdict(set)
    for fn, keys in emitted.items():
        for k in keys:
            owners[k].add(fn)

    rows = []
    for fn, keys in emitted.items():
        exclusive = [k for k in keys if owners[k] == {fn}]
        if not exclusive:
            rows.append((fn, None, 0, 0, 0))
            continue
        k = max(exclusive, key=lambda x: keys[x])
        rows.append((fn, k, keys[k], below.get(k, 0), above.get(k, 0)))

    groups = (
        ("the vendor does NOT run it above the threshold: gate candidate",
         [r for r in rows if r[1] and r[3] > 0 and r[4] == 0]),
        ("it runs on both sides: do NOT gate",
         [r for r in rows if r[1] and r[3] > 0 and r[4] > 0]),
        ("witness absent below too: port ops with no oracle",
         [r for r in rows if r[1] and r[3] == 0]),
    )
    for title, group in groups:
        print(f"\n### {title}")
        if not group:
            print("  (none)")
            continue
        print(f"  {'function':<44} {'witness':<14} {'port':>5} "
              f"{'below':>6} {'above':>6}")
        for fn, k, n, b, a in sorted(group, key=lambda r: -r[2]):
            sp, addr = shown(k)
            print(f"  {fn:<44} {sp:<4}{addr:<10} {n:>5} {b:>6} {a:>6}")

    mute = [r for r in rows if not r[1]]
    print(f"\n### no exclusive witness, the method says nothing: {len(mute)}")
    for fn, *_ in sorted(mute):
        print(f"  {fn}")
    return 0


def mode_all(emitted, below, above, minimum):
    """Every address that belongs to each phase."""
    rows = []
    for fn, e in emitted.items():
        nops = sum(e.values())
        if nops < minimum:
            continue
        own = [k for k in e if k not in UBIQUITOUS]
        if not own:
            rows.append((fn, nops, 0, 0, 0))
            continue
        zero = sum(1 for k in own if above.get(k, 0) == 0)
        # below the threshold the phase must be there, or the comparison says
        # nothing
        alive = sum(1 for k in own if below.get(k, 0) > 0)
        rows.append((fn, nops, len(own), zero, alive))

    print(f"{'function':<46} {'ops':>6} {'own':>7} {'at zero':>7} "
          f"{'alive below':>11}")
    print('-' * 82)
    full = [r for r in rows if r[2] and r[3] == r[2] and r[4] == r[2]]
    most = [r for r in rows if r[2] and r[2] > r[3] >= r[2] * 0.6]
    rest = [r for r in rows if r not in full and r not in most]

    for title, group in (("absent entirely: gate candidate", full),
                         ("absent in the majority: look at the detail", most),
                         ("runs above the threshold, or has no own address",
                          rest)):
        print(f"\n### {title}")
        for fn, nops, o, z, a in sorted(group, key=lambda r: -r[1]):
            print(f"{fn:<46} {nops:>6} {o:>7} {z:>7} {a:>11}")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port', metavar='port|flow:board')
    ap.add_argument('below')
    ap.add_argument('above')
    ap.add_argument('--witness-only', action='store_true',
                    help="a single exclusive address per phase; proves the "
                         "register absent, not the phase (see the docstring)")
    ap.add_argument('--min', type=int, default=1,
                    help="ignore phases with fewer than N ops in the port")
    ap.add_argument('--fn', help="address-by-address detail of one phase")
    ap.add_argument('--tables', action='store_true',
                    help="use the table (id, off) instead of the address")
    args = ap.parse_args()

    keyfn = table_key if args.tables else T.key
    port = port_trace(args.port)
    emitted = T.emitted_by_function(port, keyfn)
    below = T.count_keys(args.below, keyfn)
    above = T.count_keys(args.above, keyfn)

    if args.fn:
        return print_detail(emitted, below, above, args.fn)
    if args.witness_only:
        return mode_witness(emitted, below, above)
    return mode_all(emitted, below, above, args.min)


if __name__ == '__main__':
    sys.exit(main())
