#!/usr/bin/env python3
"""Score one port trace against one vendor capture, twice.

The full score counts every vendor op. The restricted score drops the op
classes the port does not implement or the harness does not model, so that
a per-channel regression is not buried under a constant offset that is the
same on every channel:

  OBJ.*        shared memory traffic, not implemented
  TPL.*        template RAM, not implemented
  SI.COREREG   backplane core registers, not modelled by the harness
  PMU.*        PMU, not modelled
  OTP.*        OTP reads, attach path only
  SROMCTL.*    SROM control, attach path only
  CAL.INIT     cal-forcing switch, carries no address or value in this build
  MAC.BW       MAC-level bandwidth, not emitted by the port

Usage: sweep_score.py <vendor> <port> [label]
"""

import difflib
import re
import sys

SKIP = ("OBJ.", "TPL.", "SI.COREREG", "PMU.", "OTP.", "SROMCTL.",
        "CAL.INIT", "MAC.BW")

VENDOR = re.compile(r"^\s*[\d.]+\s+#\d+\s+cpu\d+\s+(.*?)\s*$")
PORT = re.compile(r"^\s*cpu\d+\s+(.*?)\s*$")
HEX = re.compile(r"0x0*([0-9a-fA-F]+)")


def norm(op):
    op = " ".join(op.split())
    op = re.sub(r"\s+(ret|a5|a6)=\S+", "", op)
    op = HEX.sub(lambda m: "0x" + m.group(1).lower(), op)
    # the vendor renders single-operand reg ops as AND/OR with a rendered
    # mask; the harness renders them as MOD with mask 0
    m = re.match(r"PHY\.(AND|OR)\s+addr=(\S+)\s+val=(\S+)", op)
    if m:
        op = f"PHY.MOD addr={m.group(2)} val={m.group(3)} mask=0x0"
    op = re.sub(r"\s*\((set|clr)[^)]*\)", "", op)
    return op


def load(path, rx):
    out = []
    for line in open(path, errors="replace"):
        m = rx.match(line)
        if m:
            out.append(norm(m.group(1)))
    return out


def lcs_len(a, b):
    """Matched-in-order op count, la stessa misura di test/cmp_skip.py.

    NB: qui il denominatore sono le sole op del vendor, mentre cmp_skip.py usa
    l'unione dei due flussi e penalizza le op in piu'. I numeri non sono
    confrontabili; per un punteggio citabile si usa cmp_skip.py.
    """
    sm = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
    return sum(n for _, _, n in sm.get_matching_blocks())


def score(vendor, port):
    if not vendor:
        return 0, 0, 0.0
    n = lcs_len(vendor, port)
    return n, len(vendor), 100.0 * n / len(vendor)


def main():
    vendor = load(sys.argv[1], VENDOR)
    port = load(sys.argv[2], PORT)
    label = sys.argv[3] if len(sys.argv) > 3 else "-"

    keep = lambda seq: [o for o in seq
                        if not any(o.startswith(p) for p in SKIP)]

    _, _, full = score(vendor, port)
    n_r, d_r, restricted = score(keep(vendor), keep(port))

    status = "MATCH" if restricted >= 99.995 else f"{d_r - n_r} op"
    print(f"{label:>5s} {len(vendor):8d} {len(port):8d} "
          f"{full:9.2f}% {restricted:9.2f}% {status}")


if __name__ == "__main__":
    main()
