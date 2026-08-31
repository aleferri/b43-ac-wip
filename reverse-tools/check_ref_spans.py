#!/usr/bin/env python3
"""Check every #NNNNN reference against the span of the function holding it.

check_trace_refs.py can only say that a bare index resolves to several
different ops in several captures. This goes further: because the port
reproduces a capture op-for-op, aligning a marked port trace against that
capture gives, for each driver function, the range of vendor episodes it
accounts for. A reference inside a function must fall inside one of that
function's ranges -- and which capture makes it fall inside is the capture
name the comment is missing.

Feed it one or more runs, each a flow plus the capture it reproduces:

  ./check_ref_spans.py \\
      full:d6220:../router-data/d6220/wl-diag-wl1-attach-to-bss-up-ch36-bw20.txt \\
      switch_channel:d6220:../router-data/d6220/wl-diag-wl1-down-to-bss-ch36-bw20.txt:AC_FIRST_INIT=0

Output per reference: the runs whose span contains it. Exactly one is the
answer. None means the reference belongs to a capture not among the runs,
or is wrong. Several means it is genuinely ambiguous and needs the capture
spelled out.
"""

import difflib
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
TEST = os.path.join(ROOT, "test")

MARKER = re.compile(r"^----(/?)FN:(.+?)----$")
PORT_OP = re.compile(r"^\s*cpu\d+\s+(.*?)\s*$")
VENDOR_OP = re.compile(r"^\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+(.*?)\s*$")
HEX = re.compile(r"0x0*([0-9a-fA-F]+)")


def norm(op):
    op = " ".join(op.split())
    op = re.sub(r"\s+(ret|a5|a6)=\S+", "", op)
    op = HEX.sub(lambda m: "0x" + m.group(1).lower(), op)
    m = re.match(r"PHY\.(AND|OR)\s+addr=(\S+)\s+val=(\S+)", op)
    if m:
        op = f"PHY.MOD addr={m.group(2)} val={m.group(3)} mask=0x0"
    return re.sub(r"\s*\((set|clr)[^)]*\)", "", op)


def run_port(flow, board, env):
    """Trace with function markers; returns (ops, events).

    events is [(op_index, depth_delta, name)] so spans can be rebuilt
    without assuming functions are non-recursive.
    """
    e = dict(os.environ, AC_FN_MARKERS="1")
    e.update(env)
    out = subprocess.run([os.path.join(TEST, "ac_trace"), flow, board],
                         cwd=TEST, env=e, capture_output=True, text=True)
    ops, events = [], []
    for line in out.stdout.splitlines():
        m = MARKER.match(line.strip())
        if m:
            events.append((len(ops), -1 if m.group(1) else +1, m.group(2)))
            continue
        m = PORT_OP.match(line)
        if m:
            ops.append(norm(m.group(1)))
    return ops, events


def load_vendor(path):
    idx, ops = [], []
    for line in open(path, errors="replace"):
        m = VENDOR_OP.match(line)
        if m:
            idx.append(int(m.group(1)))
            ops.append(norm(m.group(2)))
    return idx, ops


def merge_retvals(path):
    tmp = tempfile.NamedTemporaryFile(suffix=".merged", delete=False)
    tmp.close()
    subprocess.run([sys.executable,
                    os.path.join(ROOT, "reverse-tools", "merge_retvals.py"),
                    path, tmp.name], capture_output=True)
    return tmp.name


def spans_for_run(flow, board, capture, env):
    merged = merge_retvals(capture)
    env = dict(env)
    env.setdefault("AC_READ_ORACLE", merged)
    ops, events = run_port(flow, board, env)
    vidx, vops = load_vendor(merged)

    sm = difflib.SequenceMatcher(a=vops, b=ops, autojunk=False)
    # port op position -> vendor episode number, for the aligned ones only
    port_to_ep = {}
    for i, j, n in sm.get_matching_blocks():
        for k in range(n):
            port_to_ep[j + k] = vidx[i + k]

    matched = len(port_to_ep)
    spans = {}
    stack = []
    for pos, delta, name in events:
        if delta > 0:
            stack.append((name, pos))
        elif stack:
            oname, start = stack.pop()
            eps = [port_to_ep[p] for p in range(start, pos)
                   if p in port_to_ep]
            if eps:
                spans.setdefault(oname, []).append((min(eps), max(eps)))
    return spans, matched, len(ops)


FUNC = re.compile(r"^([a-zA-Z_][\w]*)\s*\(", re.M)
COMMENT_LINE = re.compile(r"^\s*(\*/|\*|/\*|//)")
REF = re.compile(r"#(\d{3,6})\b")


def function_refs():
    """(func_name, file, line, index) for every reference inside a function."""
    out = []
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(".c"):
            continue
        lines = open(os.path.join(SRC, name)).read().split("\n")
        cur, depth, started = None, 0, False
        for lineno, line in enumerate(lines, 1):
            if cur is None:
                m = re.match(r"^(?:static\s+)?[\w][\w \t*]*?(\w+)\("
                             r"|^(\w+)\($", line)
                if m and not line.strip().startswith(("if", "for", "while",
                                                      "switch", "return")):
                    cand = m.group(1) or m.group(2)
                    if cand:
                        cur, depth, started = cand, 0, False
            if cur:
                depth += line.count("{") - line.count("}")
                if "{" in line:
                    started = True
                for r in REF.finditer(line):
                    body = line
                    if not COMMENT_LINE.match(line):
                        cut = min((line.find(t) for t in ("/*", "//")
                                   if t in line), default=-1)
                        if cut < 0 or r.start() < cut:
                            continue
                    out.append((cur, name, lineno, int(r.group(1))))
                if started and depth <= 0:
                    cur = None
    return out


def main():
    runs = []
    for spec in sys.argv[1:]:
        parts = spec.split(":")
        flow, board, capture = parts[0], parts[1], parts[2]
        env = dict(p.split("=", 1) for p in parts[3:] if "=" in p)
        runs.append((flow, board, capture, env))

    if not runs:
        print(__doc__)
        return 2

    all_spans = {}
    cache = os.environ.get("SPAN_CACHE")
    if cache and os.path.exists(cache):
        import json
        all_spans = {k: {f: [tuple(s) for s in v] for f, v in d.items()}
                     for k, d in json.load(open(cache)).items()}
        print(f"spans read from {cache}", file=sys.stderr)
    else:
        for flow, board, capture, env in runs:
            label = f"{flow}/{os.path.basename(capture)}"
            spans, matched, total = spans_for_run(flow, board, capture, env)
            print(f"{label}: {matched}/{total} port ops aligned, "
                  f"{len(spans)} functions with a span", file=sys.stderr)
            all_spans[label] = spans
        if cache:
            import json
            json.dump(all_spans, open(cache, "w"))

    refs = function_refs()
    counts = {"one": 0, "none": 0, "many": 0, "nofunc": 0}
    report = []
    for func, fname, lineno, idx in refs:
        hits = []
        seen_func = False
        for label, spans in all_spans.items():
            if func not in spans:
                continue
            seen_func = True
            if any(lo <= idx <= hi for lo, hi in spans[func]):
                hits.append(label)
        if not seen_func:
            counts["nofunc"] += 1
            continue
        if len(hits) == 1:
            counts["one"] += 1
            report.append(("one", fname, lineno, idx, func, hits))
        elif not hits:
            counts["none"] += 1
            report.append(("none", fname, lineno, idx, func, hits))
        else:
            counts["many"] += 1
            report.append(("many", fname, lineno, idx, func, hits))

    print(f"\nreferences inside a function with a known span: "
          f"{counts['one'] + counts['none'] + counts['many']}")
    print(f"  one run's span contains it (capture identified): {counts['one']}")
    print(f"  no run's span contains it (wrong or other capture): "
          f"{counts['none']}")
    print(f"  several runs contain it (needs the capture spelled out): "
          f"{counts['many']}")
    print(f"references in functions no run reached: {counts['nofunc']}")

    print("\nidentified:")
    for kind, fname, lineno, idx, func, hits in report:
        if kind == "one":
            print(f"  {fname}:{lineno} #{idx} in {func} -> {hits[0]}")

    print("\noutside every span:")
    for kind, fname, lineno, idx, func, hits in report:
        if kind == "none":
            print(f"  {fname}:{lineno} #{idx} in {func}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
