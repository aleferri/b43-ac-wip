#!/usr/bin/env python3
"""Trace reading, op normalization, and attribution of ops to functions.

Three formats pass through here:

  vendor capture   `<ts> #<ep> cpuN <op>`, produced by decode-wl-diag.py
  port trace       `cpuN <op>`, produced by the harness in test/
  markers          `----FN:name----` / `----/FN:name----`, which the harness
                   interleaves into its own trace when AC_FN_MARKERS is set

Op normalization and attribution to functions are not details of one tool:
they are the definition of what "the same op" and "this op belongs to this
function" mean. Two tools implementing them differently give two answers to
the same question, which has happened, so they live here and not in the
callers.
"""
import collections
import difflib
import os
import re
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
TEST = os.path.join(ROOT, "test")
DATA = os.path.join(ROOT, "router-data")

RE_VENDOR = re.compile(r"^\s*([\d.]+)\s+#(\d+)\s+cpu\d+\s+(.*?)\s*$")
RE_PORT = re.compile(r"^\s*cpu\d+\s+(.*?)\s*$")
# Markers come in pairs, enter and leave, and they nest. The kind is FN for a
# function; the anchor generator uses others.
RE_MARKER = re.compile(r"^-+(/?)(FN|BLK):(\S+?)-+$")

_HEX = re.compile(r"0x0*([0-9a-fA-F]+)")
_ADDR = re.compile(r"\baddr=(0x[0-9a-fA-F]+)")
_CLASS = re.compile(r"^([A-Z]{2,7})\.([A-Z]+)")


def norm(op):
    """Canonical form of an op, for comparing the two sides.

    Three normalizations, each because the two sides render the same thing
    differently:

      spacing     the gap between mnemonic and operands is not information;
      width       the vendor tracer prints returns as 32 bit
                  (val=0x00000000), the harness as 16 (val=0x0000): same
                  number;
      AND/OR      the vendor renders a single-bit set or clear as PHY.OR /
                  PHY.AND with the mask already applied, the harness as
                  PHY.MOD with a null mask. The decoder already prints `val=`
                  in the kernel form -- `PHY.AND val=0xfffb (clr 0x0004)`,
                  where 0xfffb is ~0x0004 -- so the logged value is the one to
                  keep, and the annotation is redundant.
      naming      the harness names the GPIO enable after the bcma symbol
                  (bcma_chipco_gpio_outen), the vendor tracer after the
                  register (OE).

    The `ret=`/`a5=`/`a6=` suffix of the read hooks is also dropped: those are
    arguments of the tracer, not of the op, and the harness stubs do not model
    them.
    """
    op = " ".join(op.split())
    op = re.sub(r"^GPIO\.OUTEN\b", "GPIO.OE", op)
    op = re.sub(r"\s+(ret|a5|a6)=\S+", "", op)
    op = _HEX.sub(lambda m: "0x" + m.group(1).lower(), op)
    m = re.match(r"PHY\.(AND|OR)\s+addr=(\S+)\s+val=(\S+)", op)
    if m:
        op = f"PHY.MOD addr={m.group(2)} val={m.group(3)} mask=0x0"
    return re.sub(r"\s*\((set|clr)[^)]*\)", "", op)


def op_class(op):
    """`PHY.WR addr=...` -> `PHY.WR`, or None."""
    m = _CLASS.match(op.strip())
    return f"{m.group(1)}.{m.group(2)}" if m else None


def space(op):
    """`PHY.WR addr=...` -> `PHY`, or None."""
    m = _CLASS.match(op.strip())
    return m.group(1) if m else None


def key(op):
    """(space, address) of an op, or None if it carries no address.

    This is the identity of the register touched, which is the granularity to
    reason at when asking "who touches this register" -- not the access class,
    because a read and a write of the same register speak about the same cell.
    """
    a, s = _ADDR.search(op), space(op)
    return (s, a.group(1)) if a and s else None


Op = collections.namedtuple("Op", "ep ts text norm")


def read_vendor(path):
    """The ops of a vendor capture, in order, with episode and timestamp."""
    out = []
    with open_trace(path) as f:
        for line in f:
            m = RE_VENDOR.match(line)
            if m:
                out.append(Op(int(m.group(2)), float(m.group(1)),
                              m.group(3), norm(m.group(3))))
    return out


def read_port(path):
    """The ops of a harness trace, normalized, without the markers."""
    out = []
    with open_trace(path) as f:
        for line in f:
            if RE_MARKER.match(line.strip()):
                continue
            m = RE_PORT.match(line)
            if m:
                out.append(norm(m.group(1)))
    return out


def open_trace(path):
    """A trace file, including inside a zip with the `zip!inner` syntax."""
    if "!" in path:
        z, inner = path.split("!", 1)
        return _ZipLines(z, inner)
    return open(path, errors="replace")


class _ZipLines:
    def __init__(self, z, inner):
        self.zf = zipfile.ZipFile(z)
        self.fh = self.zf.open(inner)

    def __iter__(self):
        for line in self.fh:
            yield line.decode("utf-8", "replace")

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.fh.close()
        self.zf.close()


def segment(path):
    """Attribute every op of the trace to its innermost owner.

    Returns (ops, events, owner):

      ops      the normalized ops, in order;
      events   [(position, +1|-1, name)], enough to rebuild the intervals
               without assuming a function is not recursive;
      owner    [name | None] parallel to ops.

    Markers come in pairs and nest, so every op goes to the innermost active
    function: reading only the enter marker attributes to the callee
    everything the caller emits after the return, and a function that touches a
    single register would come out owning a thousand ops.

    On a leave that is not on top, the stack is unwound down to it rather than
    trusted: the pairing is guaranteed by the cleanup attribute of B43_AC_FN,
    but a trace truncated halfway must not corrupt the attribution of
    everything after it.
    """
    ops, events, owner = [], [], []
    stack = []
    with open_trace(path) as f:
        for line in f:
            m = RE_MARKER.match(line.strip())
            if m:
                closing, _kind, name = m.groups()
                if closing:
                    if name in stack:
                        events.append((len(ops), -1, name))
                        while stack.pop() != name:
                            pass
                else:
                    events.append((len(ops), +1, name))
                    stack.append(name)
                continue
            m = RE_PORT.match(line)
            if m:
                ops.append(norm(m.group(1)))
                owner.append(stack[-1] if stack else None)
    return ops, events, owner


def intervals(events, end):
    """From events to {name: [(start, end_exclusive), ...]} in op positions."""
    out = collections.defaultdict(list)
    stack = []
    for pos, delta, name in events:
        if delta > 0:
            stack.append((name, pos))
        elif stack:
            n, start = stack.pop()
            out[n].append((start, pos))
    while stack:
        n, start = stack.pop()
        out[n].append((start, end))
    return dict(out)


def emitted_by_function(path, keyfn=key):
    """{function: Counter(key)} of the ops each function emits itself."""
    ops, _, owner = segment(path)
    out = collections.defaultdict(collections.Counter)
    for op, fn in zip(ops, owner):
        if fn is None:
            continue
        k = keyfn(op)
        if k:
            out[fn][k] += 1
    return out


def count_keys(path, keyfn=key):
    """Counter(key) over a vendor capture or a port trace.

    The ops go through norm() like those of emitted_by_function, otherwise the
    addresses of the two sides do not match: the vendor tracer prints them
    zero-padded and the harness does not.
    """
    c = collections.Counter()
    with open_trace(path) as f:
        for line in f:
            m = RE_VENDOR.match(line)
            text = m.group(3) if m else None
            if text is None:
                m = RE_PORT.match(line)
                text = m.group(1) if m else None
            if text is None:
                continue
            k = keyfn(norm(text))
            if k:
                c[k] += 1
    return c


def merge_retvals(path):
    """Fold the RETVAL records in and return the path of the temporary.

    Needed before using a capture as an oracle or as a term of comparison:
    without the folding every read has `val=UNDEFINED` and the value is not
    comparable.
    """
    tmp = tempfile.NamedTemporaryFile(suffix=".merged", delete=False)
    tmp.close()
    subprocess.run([sys.executable,
                    os.path.join(ROOT, "reverse-tools", "trace_filter.py"),
                    "--retvals",
                    path, tmp.name], capture_output=True)
    return tmp.name


def captures(include_zips=True):
    """Every wl-diag capture under router-data/, zips included.

    Returns [(label, path)], where the path of a capture inside an archive
    uses the `archive.zip!inner.txt` syntax that open_trace() accepts.
    """
    out = []
    for board in sorted(os.listdir(DATA)):
        bdir = os.path.join(DATA, board)
        if not os.path.isdir(bdir):
            continue
        for name in sorted(os.listdir(bdir)):
            path = os.path.join(bdir, name)
            if name.endswith(".txt") and ("wl-diag" in name or "wl1-" in name):
                out.append((f"{board}/{name}", path))
            elif name.endswith(".zip") and include_zips:
                with zipfile.ZipFile(path) as zf:
                    inner = sorted(i.filename for i in zf.infolist())
                for one in inner:
                    if not one.endswith(".txt"):
                        continue
                    if re.search(r"LEGGIMI|README", one):
                        continue
                    out.append((f"{board}/{name}!{one}", f"{path}!{one}"))
    return out


def run_port(flow, board, env=None, markers=True):
    """Run the harness and return its stdout.

    The harness must run with cwd=test/ because the board profiles read the
    dumps from router-data/ by relative path.
    """
    e = dict(os.environ)
    if markers:
        e["AC_FN_MARKERS"] = "1"
    e.update(env or {})
    r = subprocess.run([os.path.join(TEST, "ac_trace"), flow, board],
                       cwd=TEST, env=e, capture_output=True, text=True)
    return r.stdout


def parse_run(spec):
    """`flow:board:capture[:VAR=val ...]` -> (flow, board, capture, env)."""
    parts = spec.split(":")
    if len(parts) < 3:
        raise ValueError(f"malformed run: {spec!r}, need flow:board:capture")
    env = dict(p.split("=", 1) for p in parts[3:] if "=" in p)
    return parts[0], parts[1], parts[2], env


def run_against(flow, board, capture, env=None):
    """Run the port against the capture it is supposed to reproduce.

    Returns (port_trace, folded_capture). The capture goes through
    merge_retvals and becomes the read oracle: without it the port reads
    values that are not the capture's, and from there on it would diverge for
    a reason that is not a defect of the port.
    """
    merged = merge_retvals(capture)
    env = dict(env or {})
    env.setdefault("AC_READ_ORACLE", merged)
    trace = write_temp(run_port(flow, board, env))
    return trace, merged


def spans_by_function(trace, capture):
    """{function: [(ep_min, ep_max), ...]}, plus (aligned, total).

    The port reproduces the capture op by op, so aligning the two sequences
    gives, for every aligned port op, the vendor episode that corresponds to
    it. From there a function's interval is the minimum and maximum of the
    episodes of its own ops -- its own, not those of its callees, which the
    nested markers keep separate.

    A function called N times has N intervals, in call order: the min-max hull
    of all the calls is not an interval the function covers, and collapsing
    them would be a silent loss.
    """
    ops, events, _ = segment(trace)
    vendor = read_vendor(capture)
    sm = difflib.SequenceMatcher(a=[o.norm for o in vendor], b=ops,
                                 autojunk=False)
    pos_to_ep = {}
    for i, j, n in sm.get_matching_blocks():
        for k in range(n):
            pos_to_ep[j + k] = vendor[i + k].ep

    spans = {}
    for name, ivs in intervals(events, len(ops)).items():
        for start, end in ivs:
            eps = [pos_to_ep[p] for p in range(start, end) if p in pos_to_ep]
            if eps:
                spans.setdefault(name, []).append((min(eps), max(eps)))
    return spans, len(pos_to_ep), len(ops)


def write_temp(text, suffix=".txt"):
    tmp = tempfile.NamedTemporaryFile(mode="w", suffix=suffix, delete=False)
    tmp.write(text)
    tmp.close()
    return tmp.name
