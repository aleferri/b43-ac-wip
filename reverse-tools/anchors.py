#!/usr/bin/env python3
"""The `#NNNN` anchors in the comments: where they land, and whether still so.

An anchor names the index of an op in the capture the sequence was
transcribed from. That alone is not enough to find it again: the index spaces
of the captures overlap, so a bare `#34529` is ambiguous by construction, and
a capture that no longer exists makes it an orphan with nothing to signal it.

Three modes, in increasing order of strength. Each answers a different
question and none replaces the others:

  resolve   Which captures hold that index, and what op is there? Needs no
            harness. Answers "it exists somewhere", which is the minimum.

  class     Is the op at that index of the class the code emits there? Needs
            no harness. An anchor confirmed by its class is not necessarily
            right, but one contradicted by it is certainly wrong: if the code
            does a PHY write and the index holds a TBL, either the anchor or
            the capture is not the one.

  span      Does the index fall in the episode interval that the enclosing
            function actually covers? Needs the harness, and needs the port
            to reproduce the capture: it is the alignment that gives, per
            function, the episodes it accounts for. This is the strong mode,
            and the one that identifies *which* capture -- the information the
            anchor is missing.

And one mode that writes instead of checking:

  write     Generate the `[capture-ref: <capture>; <from>-<to>, ...]` markers
            in the comment of every instrumented function and named block,
            replacing whatever is there. This is the answer to the anchor
            going stale: a marker is not maintained by hand, it is regenerated
            from the same spans `span` checks, so a hook moving or a capture
            being replaced shows up as a diff instead of ageing silently.

            The runs come from capture-refs.conf, so a regeneration is
            reproducible and the capture written into the marker is a path
            that still resolves. `--remove` deletes every marker and writes
            none; `--dry-run` says what would change.

            The granularity is the instrumentation: one marker per
            B43_AC_FN(), plus one per B43_AC_BLOCK("name") where a section of
            a long function is worth locating on its own. Per op it would be
            unreadable; automatic it would guess.

Usage:
  anchors.py write   [--remove] [--dry-run] [--conf FILE]
  anchors.py resolve [--all] [--ref N] [--no-zips]
  anchors.py class <source.c> <capture.txt> [<capture.txt> ...]
  anchors.py span <flow>:<board>:<capture>[:VAR=val ...] [...]

`span` takes several runs because an identified anchor is one contained by
exactly ONE of them: none means it points at a capture not among these, or is
wrong; more than one means it is ambiguous and the capture must be spelled
out.

Exits 1 when at least one anchor fails to resolve, so it is usable as a gate
before a commit.
"""
import argparse
import collections
import json
import os
import re
import sys

import tracelib as T

REF = re.compile(r"#(\d{3,7})\b")
COMMENT_LINE = re.compile(r"^\s*(\*/|\*|/\*|//)")

# Which op class a line of code emits, when the name of the called function
# says so. The `class` mode needs an expectation: without one there is nothing
# to contradict.
EXPECTED_CLASS = [
    (re.compile(r"b43_maccontrol|b43_mac_enable|b43_mac_suspend"), "MAC"),
    (re.compile(r"b43_hf_write|b43_hf_read|mhf_maskset"), "MAC"),
    (re.compile(r"b43_phy_(write|read|maskset|set|mask|read_log|force_clock)\b"),
     "PHY"),
    (re.compile(r"b43_radio_(write|read|maskset|set|mask)\b"), "RAD"),
    (re.compile(r"b43_actab_(write|read|zerofill|fill)"), "TBL"),
    (re.compile(r"b43_shm_(write|read)"), "OBJ"),
]

# A table write renders as a TBL marker plus the ops on the data port, so an
# anchor on a TBL may land on a PHY and has to be accepted.
EQUIVALENT = {"TBL": {"TBL", "PHY"}}


# ---------------------------------------------------------------------------
# Collecting the anchors from the sources
# ---------------------------------------------------------------------------

def _comment_part(line):
    """The part of the line that is comment, or None if there is none."""
    if COMMENT_LINE.match(line):
        return line
    cuts = [line.find(t) for t in ("/*", "//") if t in line]
    return line[min(cuts):] if cuts else None


def anchors(sources=None):
    """(index, file, line, text) for every anchor inside a comment."""
    out = []
    names = sources or [n for n in sorted(os.listdir(T.SRC))
                        if n.endswith((".c", ".h"))]
    for name in names:
        path = name if os.path.sep in name else os.path.join(T.SRC, name)
        for n, line in enumerate(open(path, errors="replace"), 1):
            part = _comment_part(line)
            if part is None:
                continue
            for m in REF.finditer(part):
                out.append((int(m.group(1)), os.path.basename(path), n,
                            line.rstrip()))
    return out


def anchors_by_function():
    """(function, file, line, index) for every anchor inside a function.

    Recognizing the function is textual and sits at the level of approximation
    that is needed: it opens on a signature at column zero and closes when the
    braces balance again. An anchor outside any function (at the top of the
    file, or between two functions) has no function to attribute it to and
    does not enter.
    """
    out = []
    for name in sorted(os.listdir(T.SRC)):
        if not name.endswith(".c"):
            continue
        lines = open(os.path.join(T.SRC, name)).read().split("\n")
        cur, depth, opened = None, 0, False
        for n, line in enumerate(lines, 1):
            if cur is None:
                m = re.match(r"^(?:static\s+)?[\w][\w \t*]*?(\w+)\(|^(\w+)\($",
                             line)
                if m and not line.strip().startswith(
                        ("if", "for", "while", "switch", "return")):
                    cur = m.group(1) or m.group(2)
                    depth, opened = 0, False
            if cur:
                depth += line.count("{") - line.count("}")
                if "{" in line:
                    opened = True
                part = _comment_part(line)
                if part is not None:
                    for m in REF.finditer(part):
                        out.append((cur, name, n, int(m.group(1))))
                if opened and depth <= 0:
                    cur = None
    return out


# ---------------------------------------------------------------------------
# resolve -- which captures hold the index
# ---------------------------------------------------------------------------

def resolve(indices, include_zips=True):
    """{index: [(label, op), ...]}, scanning each capture once."""
    found = collections.defaultdict(list)
    for label, path in T.captures(include_zips):
        with T.open_trace(path) as f:
            for line in f:
                m = T.RE_VENDOR.match(line)
                if m and int(m.group(2)) in indices:
                    found[int(m.group(2))].append(
                        (label, " ".join(m.group(3).split())))
    return found


def mode_resolve(args):
    if args.ref is not None:
        found = resolve({args.ref}, not args.no_zips)
        for label, op in found.get(args.ref, []):
            print(f"  {label:64s} {op}")
        if not found.get(args.ref):
            print(f"  #{args.ref}: no capture under router-data/ holds it")
        return 0

    refs = anchors()
    wanted = {i for i, *_ in refs}
    print(f"{len(refs)} anchors, {len(wanted)} distinct, scanning "
          f"captures...", file=sys.stderr)
    found = resolve(wanted, not args.no_zips)

    orphan, unique, ambiguous = [], [], []
    for i, name, n, text in refs:
        cand = found.get(i, [])
        if not cand:
            orphan.append((i, name, n, text))
        elif len({c[1] for c in cand}) == 1:
            unique.append((i, name, n, cand))
        else:
            ambiguous.append((i, name, n, cand))

    print(f"resolve to one op   : {len(unique)}")
    print(f"resolve to several  : {len(ambiguous)}")
    print(f"resolve to nothing  : {len(orphan)}")

    if orphan:
        print("\nanchors no capture in this repo can resolve:")
        for i, name, n, text in orphan:
            print(f"  {name}:{n}  #{i}")
            print(f"      {text.strip()[:96]}")

    if args.all:
        print("\nambiguous anchors and their candidates:")
        for i, name, n, cand in ambiguous:
            print(f"  {name}:{n}  #{i}")
            for label, op in cand:
                print(f"      {label:60s} {op}")

    return 1 if orphan else 0


# ---------------------------------------------------------------------------
# class -- the op at that index is of the class the code emits
# ---------------------------------------------------------------------------

def class_index(path):
    """index -> space of the op.

    RETVAL records carry an index of their own and belong to the read that
    precedes them, so they resolve onto it: an anchor landing on the RETVAL of
    a read anchors that read.
    """
    d = {}
    with T.open_trace(path) as f:
        for line in f:
            m = T.RE_VENDOR.match(line)
            if not m:
                continue
            i, sp = int(m.group(2)), T.space(m.group(3)) or "?"
            if m.group(3).startswith("RETVAL"):
                fm = re.search(r"for=#(\d+)", line)
                sp = d.get(int(fm.group(1)), "RETVAL") if fm else "RETVAL"
            d[i] = sp
    return d


def expected_class(line):
    for pat, sp in EXPECTED_CLASS:
        if pat.search(line):
            return sp
    return None


def mode_class(args):
    indices = {p: class_index(p) for p in args.captures}
    status = 0
    for source in args.sources:
        lines = open(source, errors="replace").read().splitlines()
        orphan, ok = [], collections.Counter()
        for n, line in enumerate(lines, 1):
            part = _comment_part(line)
            if part is None:
                continue
            for m in REF.finditer(part):
                a = int(m.group(1))
                # The expected class comes from the line itself or, if it is a
                # pure comment, from the first line of code within five.
                exp = expected_class(line)
                if exp is None:
                    for r in lines[n:n + 5]:
                        exp = expected_class(r)
                        if exp:
                            break
                if exp is None:
                    continue
                allowed = EQUIVALENT.get(exp, {exp})
                if any(d.get(a) in allowed for d in indices.values()):
                    ok[exp] += 1
                else:
                    seen = {os.path.basename(p): d.get(a, "-")
                            for p, d in indices.items()}
                    orphan.append((n, a, exp, seen))

        print(f"{source}: {sum(ok.values())} confirmed by class, "
              f"{len(orphan)} contradicted")
        for exp, k in ok.most_common():
            print(f"  confirmed {exp:<6} {k}")
        for n, a, exp, seen in orphan:
            where = " ".join(f"{k}={v}" for k, v in seen.items())
            print(f"  {os.path.basename(source)}:{n}  #{a} expected "
                  f"{exp:<5} -> {where}")
        if orphan:
            status = 1
    return status


# ---------------------------------------------------------------------------
# span -- the index falls in the interval of the enclosing function
# ---------------------------------------------------------------------------

def collect_spans(runs, cache=None):
    """{label: {function: [(lo, hi)]}}, from the cache if there is one."""
    if cache and os.path.exists(cache):
        print(f"spans read from {cache}", file=sys.stderr)
        return {k: {f: [tuple(s) for s in v] for f, v in d.items()}
                for k, d in json.load(open(cache)).items()}
    all_spans = {}
    for flow, board, capture, env in runs:
        label = f"{flow}/{os.path.basename(capture)}"
        trace, merged = T.run_against(flow, board, capture, env)
        spans, aligned, total = T.spans_by_function(trace, merged)
        print(f"{label}: {aligned}/{total} port ops aligned, "
              f"{len(spans)} functions with a span", file=sys.stderr)
        all_spans[label] = spans
    if cache:
        json.dump(all_spans, open(cache, "w"))
    return all_spans


def mode_span(args):
    runs = [T.parse_run(s) for s in args.runs]
    all_spans = collect_spans(runs, os.environ.get("SPAN_CACHE"))

    refs = anchors_by_function()
    counts = collections.Counter()
    report = []
    for fn, name, n, i in refs:
        hits, seen = [], False
        for label, spans in all_spans.items():
            if fn not in spans:
                continue
            seen = True
            if any(lo <= i <= hi for lo, hi in spans[fn]):
                hits.append(label)
        if not seen:
            counts["nofn"] += 1
            continue
        verdict = "one" if len(hits) == 1 else ("none" if not hits else "many")
        counts[verdict] += 1
        report.append((verdict, name, n, i, fn, hits))

    inside = counts["one"] + counts["none"] + counts["many"]
    print(f"\nanchors inside a function with a known span: {inside}")
    print(f"  contained by exactly one run (capture identified): "
          f"{counts['one']}")
    print(f"  outside every span (wrong or another capture): {counts['none']}")
    print(f"  contained by several runs (capture must be spelled out): "
          f"{counts['many']}")
    print(f"anchors in functions no run reaches: {counts['nofn']}")

    print("\nidentified:")
    for verdict, name, n, i, fn, hits in report:
        if verdict == "one":
            print(f"  {name}:{n} #{i} in {fn} -> {hits[0]}")

    print("\noutside every span:")
    for verdict, name, n, i, fn, hits in report:
        if verdict == "none":
            print(f"  {name}:{n} #{i} in {fn}")

    return 1 if counts["none"] else 0



# ---------------------------------------------------------------------------
# write -- generate, update and remove the [capture-ref: ...] markers
# ---------------------------------------------------------------------------

CONF = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "capture-refs.conf")

MARKER_OPEN = re.compile(r"\[capture-ref:")
# A marker is one logical unit that may span several physical lines, so
# removal keys on the opening and runs to the closing bracket. Both shapes are
# recognized: inside a block comment (` * [capture-ref: ...]`) and standalone
# (`/* [capture-ref: ...] */`).
WIDTH = 79

# The `B43_AC_FN();` line is what makes a function appear in the trace at all,
# so it is also what says which functions may carry a marker: one on a function
# with no marker macro would be a claim nothing can check.
FN_MARK = re.compile(r"^\s*B43_AC_FN\(\);")
BLOCK_MARK = re.compile(r'^(\s*)B43_AC_BLOCK\("([^"]+)"\);')


def resolve_capture(written):
    """The capture path as the filesystem sees it.

    The conf spells paths relative to the repo root, because that is what goes
    into the marker and a marker has to mean the same thing from any working
    directory. Resolution happens here; the written form is what gets
    recorded.
    """
    if os.path.isabs(written):
        return written
    z, sep, inner = written.partition("!")
    return os.path.join(T.ROOT, z) + sep + inner


def read_conf(path=CONF):
    """The runs to generate from. [(flow, board, written, resolved, env)]."""
    runs = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        flow, board, written, env = T.parse_run(line)
        runs.append((flow, board, written, resolve_capture(written), env))
    if not runs:
        sys.exit(f"{path} lists no run")
    return runs


def render_block(entries, in_comment):
    """The markers for one site, as a single comment.

    All the captures go in one comment rather than one comment each: a
    function reached by two runs would otherwise carry six lines of marker
    where three say the same thing.

    The whole list of intervals is written, not their hull: a function called
    35 times covers 35 stretches of the capture and the min-max of those is an
    interval it does not cover. That is why a marker can run to several lines,
    and why it wraps instead of truncating -- a truncated list would read like
    a complete one.
    """
    logical = [f"[capture-ref: {cap}; "
               + ", ".join(f"{lo}-{hi}" for lo, hi in spans) + "]"
               for cap, spans in entries]

    body = []
    for text in logical:
        head, _, rest = text.partition("; ")
        line = head + ";"
        body.append(line)
        line = "  "
        for tok in rest.split(" "):
            if len(line) + len(tok) + 3 > WIDTH:
                body.append(line.rstrip())
                line = "  "
            line += tok + " "
        body.append(line.rstrip())

    if in_comment:
        return [f" * {l}".rstrip() for l in body]
    out = [f"/* {body[0]}"] + [f" * {l}".rstrip() for l in body[1:]]
    out.append(" */")
    return out


def strip_markers(lines):
    """Drop every existing marker. Returns (lines, entries removed).

    A marker takes one of two shapes and they unwind differently, which is the
    whole difficulty:

      standalone   the first line opens the comment (`/* [capture-ref: ...`),
                   so the trailing ` */` belongs to the marker and goes too;
      appended     the first line is inside a comment that was already there
                   (` * [capture-ref: ...`), so the trailing ` */` belongs to
                   the HOST comment and must stay. Eating it detaches the
                   host's prose from its closer and the next pass then removes
                   the prose as well.
    """
    out, removed, i = [], 0, 0
    while i < len(lines):
        if not (MARKER_OPEN.search(lines[i]) and _comment_part(lines[i])):
            out.append(lines[i])
            i += 1
            continue

        standalone = lines[i].lstrip().startswith("/*")
        # consume every consecutive entry: each ends on the line with its `]`
        while i < len(lines) and MARKER_OPEN.search(lines[i]):
            removed += 1
            while i < len(lines) and "]" not in lines[i]:
                i += 1
            i += 1
        if standalone and i < len(lines) and lines[i].strip() == "*/":
            i += 1
    return out, removed


def owners_in_source(lines):
    """[(insert_at, indent, name, in_comment)] for every marker site.

    A function's site is the line above its signature; a block's is the line
    above its B43_AC_BLOCK. `in_comment` says whether the marker can be
    appended inside a block comment that is already there, which is where it
    reads best, or has to bring its own.
    """
    sites = []
    for i, line in enumerate(lines):
        m = BLOCK_MARK.match(line)
        if m:
            sites.append((i, m.group(1), m.group(2), False))
            continue
        if not FN_MARK.match(line):
            continue
        # walk up to the first line of the signature
        j = i - 1
        while j >= 0 and lines[j].strip() not in ("{",) and \
                not lines[j].rstrip().endswith("{"):
            j -= 1
        start = j
        while start > 0 and lines[start - 1].strip() and \
                not lines[start - 1].rstrip().endswith(("*/", "}", ";")):
            start -= 1
        name = _signature_name(lines, start, j)
        if not name:
            continue
        prev = lines[start - 1].rstrip() if start > 0 else ""
        if prev.endswith("*/") and prev.strip() != "*/":
            # single-line block comment: not worth reopening, stand alone
            sites.append((start, "", name, False))
        elif prev.strip() == "*/":
            sites.append((start - 1, "", name, True))
        else:
            sites.append((start, "", name, False))
    return sites


def _signature_name(lines, start, end):
    text = " ".join(l.strip() for l in lines[start:end + 1])
    m = re.search(r"\b(b43_[a-z0-9_]+)\s*\(", text)
    return m.group(1) if m else None


def mode_write(args):
    runs = read_conf(args.conf)
    per_run = []
    if not args.remove:
        for flow, board, written, resolved, env in runs:
            trace, merged = T.run_against(flow, board, resolved, env)
            spans, aligned, total = T.spans_by_function(trace, merged)
            print(f"{flow}/{os.path.basename(written)}: {aligned}/{total} "
                  f"port ops aligned, {len(spans)} owners with a span",
                  file=sys.stderr)
            per_run.append((written, spans))

    touched = wrote = dropped = 0
    for name in sorted(os.listdir(T.SRC)):
        if not name.endswith(".c"):
            continue
        path = os.path.join(T.SRC, name)
        lines = open(path).read().split("\n")

        lines, removed = strip_markers(lines)
        dropped += removed

        added = 0
        if not args.remove:
            # bottom to top, so the insertion points stay valid
            for at, indent, owner, in_comment in \
                    sorted(owners_in_source(lines), reverse=True):
                entries = [(cap, spans[owner]) for cap, spans in per_run
                           if owner in spans]
                if entries:
                    block = render_block(entries, in_comment)
                    lines[at:at] = [indent + l if indent else l
                                    for l in block]
                    added += 1

        text = "\n".join(lines)
        if text != open(path).read():
            if not args.dry_run:
                open(path, "w").write(text)
            touched += 1
        wrote += added

    verb = "would " if args.dry_run else ""
    print(f"{verb}removed {dropped} marker entries, wrote markers on {wrote} "
          f"owners, {touched} files changed")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    p = sub.add_parser("resolve", help="which captures hold the index")
    p.add_argument("--all", action="store_true",
                   help="list every anchor, not just the broken ones")
    p.add_argument("--ref", type=int, help="resolve a single index and exit")
    p.add_argument("--no-zips", action="store_true",
                   help="skip the sweep archives (much faster)")
    p.set_defaults(fn=mode_resolve)

    p = sub.add_parser("class", help="the op class confirms or contradicts")
    p.add_argument("sources", nargs=1)
    p.add_argument("captures", nargs="+")
    p.set_defaults(fn=mode_class)

    p = sub.add_parser("span", help="the index falls in the function's span")
    p.add_argument("runs", nargs="+", metavar="flow:board:capture[:VAR=val]")
    p.set_defaults(fn=mode_span)

    p = sub.add_parser("write", help="generate, update or remove the markers")
    p.add_argument("--conf", default=CONF,
                   help="the run list (default reverse-tools/capture-refs.conf)")
    p.add_argument("--remove", action="store_true",
                   help="only remove the existing markers, write none")
    p.add_argument("--dry-run", action="store_true",
                   help="say what would change, touch nothing")
    p.set_defaults(fn=mode_write)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
