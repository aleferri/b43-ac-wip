#!/usr/bin/env python3
"""Cut a decoded trace into one segment per configuration.

The boundary and the label are two distinct things and come from different
signals. The boundary criterion is chosen with --on, and which one is right
depends on how the capture was taken, not on preference:

  mark       The MARK records. They are placed by whoever captures (`echo
             "ch36 bw20" > /proc/wl_diag`) and by wl_diag at the edges of each
             load of the target, so the boundary is written in the trace and
             does not have to be guessed. This is the criterion of the COLD
             sweep of cold_capture.sh, where every cycle is an rmmod+insmod.

  chanspec   The chanspec write into shared memory (`OBJ.WR 0xa0`, or the
             CS.SHM argument that performs it). Exact boundary and name from
             the same signal. This is the criterion the CALD sweep segments in
             router-data/d6220/hot-sweep.zip were produced with. Mind what it
             implies: inside a cycle the chanspec lands after the head of the
             cycle -- a CAL.INIT sits further on -- so the head of each cycle
             ends up at the tail of the previous segment.

  gaps       The time gaps. Between a `wl down` and the next `chanspec`
             capture_plan.sh sleeps 1 s, and no bring-up sequence has pauses
             like that. Needed when the trace carries neither MARK nor
             chanspec, and it cuts later than `chanspec`: it keeps the head of
             the cycle with the cycle, which is the right way round, but it
             does not reproduce the split already published in the archives.

The gap alone is not enough, and that is why `gaps` is not just a threshold:
while the interface is up, a periodic sampling leaves ~1.00 s holes
indistinguishable by duration from the sleep between cycles. On d6220/DSL that
periodic is OBJ.RD, on agcombo it is MAC.MCTRL, and in both cases it has the
SAME op on either side of the hole; a real boundary has a change of context.

The names of the files produced are the ones already published inside the
archives -- `00-init-parziale.txt`, `00-scartati.txt` -- and are data, not
code: renaming them would break the reproduction of a published split and the
default segment names in test/gates.sh.

Usage:
  split_trace.py --on mark     trace.txt dir/ [--prefix cold] [--bringup-only]
  split_trace.py --on chanspec trace.txt dir/
  split_trace.py --on gaps     trace.txt dir/ [--gap 1.03] [--channels 36,40]
"""
import argparse
import bisect
import os
import re
import sys

# MARK from the decoder:  <ts> #<seq> cpu0 MARK     'ch36 bw20'
RE_MARK = re.compile(r"\bMARK\s+'([^']*)'")
# The module's automatic markers are not cycle boundaries: they are load edges
# INSIDE a cycle, and cutting on them would split the attach from the channel
# that asked for it.
RE_MOD = re.compile(r"^mod (COMING|GOING)$")
RE_RAD = re.compile(r"\bRAD\.(RD|WR|MOD|AND|OR)\b")
RE_T = re.compile(r"\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+(\S+)")
RE_CS = re.compile(r"\bCHANSPEC\b.*?\bch=(\d+)\s+bw=(\S+)\s+band=(\S+)")

# The chanspec of the cycle, from either of the two hooks that carry it: the
# shared-memory write at 0xa0, or the argument of wlc_phy_chanspec_shm_set
# that performs it. They are adjacent and redundant; one of the two being
# armed is enough.
RE_SHM = re.compile(r"OBJ\.WR\s+addr=0x0*a0\s+val=0x0*([0-9a-f]+)")
RE_CSSHM = re.compile(r"CS\.SHM\b.*\braw=0x0*([0-9a-f]+)")
BW_CS = {0x1000: 20, 0x1800: 40, 0x2000: 80, 0x2800: 160}
CENTER = {20: 0, 40: 2, 80: 6, 160: 14}


def chanspec_of(line):
    """(low channel, bandwidth) from a line, or None.

    At 40 and 80 MHz the value carries the CENTER channel, and it is brought
    back to the low channel of the block: 5g36/40 gives ch=38 and the name
    wants ch36.
    """
    m = RE_SHM.search(line) or RE_CSSHM.search(line)
    if not m:
        return None
    cs = int(m.group(1), 16)
    bw = BW_CS.get(cs & 0x3800)
    if bw is None:
        return None
    return (cs & 0xff) - CENTER[bw], bw


def chanspec_label(lines):
    for l in lines:
        e = chanspec_of(l)
        if e:
            return e
    return None


def name_from_mark(n, label):
    """label -> file name, keeping the repo's -chN-bwN convention."""
    m = re.match(r"^ch(\d+)\s+bw(\d+)$", label.strip())
    if m:
        base = f"ch{m.group(1)}-bw{m.group(2)}"
    else:
        base = re.sub(r"[^A-Za-z0-9._-]+", "-", label.strip()) or "senza-nome"
    return f"{n:02d}-{base}.txt"


# ---------------------------------------------------------------------------
# mark
# ---------------------------------------------------------------------------

def cut_on_mark(lines, skip_mod):
    """[(name, lines)]. A segment starts AT its marker, so that 'mod COMING'
    and the attach that follows sit in the same file as the channel label that
    precedes them. Whatever comes before the first MARK goes to
    00-preambolo.txt instead of being thrown away."""
    segments, current = [], []
    name, n, found = "00-preambolo.txt", 0, 0
    for line in lines:
        m = RE_MARK.search(line)
        if m:
            found += 1
            label = m.group(1)
            if label == "fine corsa":
                # closes a run, not a cycle: not a boundary
                current.append(line)
                continue
            automatic = bool(RE_MOD.match(label))
            if not automatic or skip_mod:
                if current:
                    segments.append((name, current))
                n += 1
                name = name_from_mark(n, label)
                current = []
        current.append(line)
    if current:
        segments.append((name, current))
    return segments, found


def filter_bringup(segments):
    """Keep among the numbered ones only the segments whose bring-up reaches
    the radio, and renumber, so the numbers follow the channels.

    A label does not guarantee a cycle: in the d6220 cold sweep the first
    `ch36 bw20` appears three times before an attach gets all the way through.

    The criterion is the presence of RAD ops, not of PHY ops, and the
    difference matters: the d6220's interrupted insmod emits ten PHY writes
    (0xa5-0xa7, 0x8f, 0x78) but zero RAD, while every real cycle has about
    1710 of them. Those PHY writes belong to the core, not to the PHY driver.
    A size threshold would not do either: it would be chosen by eye.
    """
    good, dropped = [], []
    for name, lines in segments:
        if name.startswith("00-") or any(RE_RAD.search(r) for r in lines):
            good.append((name, lines))
        else:
            dropped.append((name, lines))
    renum, k = [], 0
    for name, lines in good:
        if name.startswith("00-"):
            renum.append((name, lines))
            continue
        k += 1
        renum.append((re.sub(r"^\d+-", f"{k:02d}-", name), lines))
    return renum, dropped


# ---------------------------------------------------------------------------
# chanspec and gaps
# ---------------------------------------------------------------------------

def bounds_from_chanspec(lines):
    """Positions of the chanspec writes, one per event.

    When both hooks are armed, CS.SHM and the OBJ.WR at 0xa0 that follows it
    carry the same value two records apart and are the same event: the first
    of the two is kept.
    """
    out, last = [], None
    for i, l in enumerate(lines):
        if chanspec_of(l) is None:
            continue
        if last is None or i - last > 2:
            out.append(i)
        last = i
    return out


def bounds_from_gaps(lines, gap):
    out, prev_t, prev_op = [], None, None
    for i, l in enumerate(lines):
        m = RE_T.match(l)
        if not m:
            continue
        t, op = float(m.group(1)), m.group(3)
        if prev_t is not None and t - prev_t > gap and prev_op != op:
            out.append(i)
        prev_t, prev_op = t, op
    return out


def validate_chanspec(lines, bounds):
    """Each segment must contain exactly ONE distinct chanspec.

    Distinct VALUES are counted, not records. If the count does not add up, the
    set of boundaries is wrong and that is said, instead of writing a plausible
    and false split.
    """
    pos = [(i, chanspec_of(l)) for i, l in enumerate(lines) if chanspec_of(l)]
    if not pos:
        return None
    per_seg = {}
    for p, e in pos:
        per_seg.setdefault(bisect.bisect_right(bounds, p), set()).add(e)
    empty = [k for k in range(len(bounds) + 1) if not per_seg.get(k)]
    double = [k for k in range(len(bounds) + 1)
              if len(per_seg.get(k, ())) > 1]
    return empty, double


def cut_at(lines, bounds, channels):
    """[(name, lines)] from the boundaries, labelled by the inner chanspec."""
    edges = [0] + bounds + [len(lines)]
    used, segments = {}, []
    for k in range(len(edges) - 1):
        body = lines[edges[k]:edges[k + 1]]
        if not any(x.strip() for x in body):
            continue
        e = chanspec_label(body)
        # The first segment is a fragment only if it carries no chanspec of its
        # own: the init tail before the first cycle (bus, OTP, PLL). CAL.INIT
        # is NOT looked at -- captures taken before that hook existed have none
        # anyway, and it would make a complete cycle look like a fragment.
        if k == 0 and not e:
            base = "00-init-parziale"
        elif e:
            base = f"{k:02d}-up-ch{e[0]}-bw{e[1]}"
        else:
            base = f"{k:02d}" + (f"-up-ch{channels[k]}"
                                 if k < len(channels) else "")
        n = used.get(base, 0) + 1
        used[base] = n
        segments.append((f"{base}.txt" if n == 1 else f"{base}-{n}.txt", body))
    return segments


# ---------------------------------------------------------------------------

def write_segments(segments, outdir, prefix):
    os.makedirs(outdir, exist_ok=True)
    # EVERYTHING is written before anything is printed: with SIGPIPE at
    # SIG_DFL a `| head` kills the process at the first print, and printing
    # inside the loop would end with the first files written and the others
    # not -- silently. It has happened.
    written = []
    for name, lines in segments:
        dest = os.path.join(outdir, prefix + name if prefix else name)
        with open(dest, "w", encoding="utf-8") as g:
            g.writelines(lines)
        written.append((dest, len(lines)))
    for dest, n in written:
        print(f"{dest}: {n} lines")
    return written


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--on", required=True,
                    choices=("mark", "chanspec", "gaps"),
                    help="the boundary criterion")
    ap.add_argument("trace")
    ap.add_argument("outdir", nargs="?", default="split")
    ap.add_argument("--prefix", default="", help="prefix of the file names")
    ap.add_argument("--skip-mod", action="store_true",
                    help="on mark: also cut on the 'mod COMING'/'GOING' MARKs")
    ap.add_argument("--bringup-only", action="store_true",
                    help="on mark: number only the segments that reach the "
                         "radio; the others go to 00-scartati.txt")
    ap.add_argument("--gap", type=float, default=1.03,
                    help="on gaps: threshold in seconds (default 1.03)")
    ap.add_argument("--channels", default="",
                    help="on gaps: positional names when the chanspec is "
                         "missing")
    a = ap.parse_args()

    if os.path.exists(a.outdir) and not os.path.isdir(a.outdir):
        sys.exit(f"'{a.outdir}' exists and is not a directory")

    with open(a.trace, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    channels = a.channels.split(",") if a.channels else []

    if a.on == "mark":
        segments, found = cut_on_mark(lines, a.skip_mod)
        if found == 0:
            print("no MARK record in the trace. If it comes from "
                  "capture_plan.sh (hot sweep), the right criterion is "
                  "--on chanspec.", file=sys.stderr)
            return 1
        dropped = []
        if a.bringup_only:
            segments, dropped = filter_bringup(segments)
        write_segments(segments, a.outdir, a.prefix)
        if dropped:
            dest = os.path.join(a.outdir, a.prefix + "00-scartati.txt")
            with open(dest, "w", encoding="utf-8") as g:
                for name, rr in dropped:
                    g.write(f"### {name}: nessuna op RAD, non e' un bring-up\n")
                    g.writelines(rr)
            print(f"{dest}: {len(dropped)} segments with no RAD op "
                  f"({', '.join(n for n, _ in dropped)})")
        print(f"\n{found} MARKs found, {len(segments)} segments written.")
        print("The names come from the LABELS, not from an assumed order: if "
              "a channel is missing, it is missing because the cycle did not "
              "happen.")
        return 0

    if a.on == "chanspec":
        bounds = bounds_from_chanspec(lines)
        criterion = f"chanspec: {len(bounds)}"
    else:
        bounds = bounds_from_gaps(lines, a.gap)
        criterion = f"gaps > {a.gap}s: {len(bounds)}"
        outcome = validate_chanspec(lines, bounds)
        if outcome:
            empty, double = outcome
            if empty or double:
                print(f"WARNING: {len(bounds) + 1} segments, {len(empty)} "
                      f"with no chanspec and {len(double)} with more than one "
                      f"distinct chanspec. The gap boundaries do not add up "
                      f"on this trace.")
            else:
                criterion += ", validated by the chanspecs (one per segment)"

    segments = cut_at(lines, bounds, channels)
    print(f"{len(segments)} segments (boundaries from {criterion})")
    if channels and len(segments) != len(channels):
        print(f"WARNING: {len(segments)} segments but {len(channels)} channels "
              f"in the list. The positional names are unreliable.")
    write_segments(segments, a.outdir, a.prefix)

    for i, (name, body) in enumerate(segments):
        seen = [f"ch{m.group(1)}/{m.group(2)}" for x in body
                if (m := RE_CS.search(x))]
        expected = f"ch{channels[i]}" if i < len(channels) else None
        if seen and expected and not any(c.startswith(expected + "/")
                                         for c in seen):
            print(f"  {name}: CHANSPEC {','.join(seen)} <-- does not agree "
                  f"with --channels")
    print("\nThe -up-chN-bwN names come from the CHANSPEC found in the "
          "segment.")
    if channels:
        print("Those without come from the ORDER of --channels, and must be "
              "checked: it is positional.")
    print("At 40 and 80 MHz the chanspec carries the CENTER channel (5g36/40 "
          "-> ch=38); the name reports the LOW channel of the block.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
