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
the same question, so they live here and not in the callers.
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
TEST = os.path.join(ROOT, "test", "unit")
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
    them. So is `sel=`, the shared-memory routing: the d6220 captures were
    decoded without it and the DSL ones and the integration harness carry it,
    and on the offset-in-bytes form of the address it adds nothing.
    """
    op = " ".join(op.split())
    op = re.sub(r"^GPIO\.OUTEN\b", "GPIO.OE", op)
    op = re.sub(r"\s+(ret|a5|a6|sel)=\S+", "", op)
    op = _HEX.sub(lambda m: "0x" + m.group(1).lower(), op)
    m = re.match(r"PHY\.(AND|OR)\s+addr=(\S+)\s+val=(\S+)", op)
    if m:
        op = f"PHY.MOD addr={m.group(2)} val={m.group(3)} mask=0x0"
    return re.sub(r"\s*\((set|clr)[^)]*\)", "", op)


_MOD = re.compile(r"^(PHY|RAD)\.MOD\s+addr=(\S+)\s+val=(\S+)\s+mask=(\S+)$")
_ANDOR = re.compile(r"^(PHY|RAD)\.(AND|OR)\s+addr=(\S+)\s+val=(\S+)")
_ACCESSOR_ONLY = re.compile(r"^(TBL\.(WR|RD)|MAC\.MHF)\b")


def unfold_bus(op):
    """The op as the bus sees it: zero, one or two ops.

    A trace taken at the MMIO bus -- test/integration -- has no accessor-level
    classes. A read-modify-write is a read and a write of the same register;
    a table access is the words on the data port, and the TBL marker that
    names the table stands for nothing on the bus; a host-flag maskset is a
    software shadow whose write-through, when it happens, is its own OBJ.WR
    on both sides. This maps an accessor-level
    op onto that vocabulary so the two can be compared without teaching the
    bus tracer what the accessors were.

    The read carries no value: the vendor never logs what a MOD read back.
    The write is constrained on the modified bits only, `mask=` kept on the
    op so ops_equal knows which bits to compare; the rest came from the read
    and is not the driver's doing. AND/OR are handled from their raw form,
    before norm() folds them to MOD with the null-mask sentinel and the
    direction is lost.
    """
    op = " ".join(op.split())
    if _ACCESSOR_ONLY.match(op):
        return []
    m = _ANDOR.match(op)
    if m:
        space, kind, addr, val = m.groups()
        v = int(val, 16)
        mask = v if kind == "OR" else (~v & 0xffff)
        set_ = v if kind == "OR" else 0
        return [norm(f"{space}.RD addr={addr} val=UNDEFINED"),
                norm(f"{space}.WR addr={addr} val=0x{set_:04x} mask=0x{mask:04x}")]
    op = norm(op)
    m = _MOD.match(op)
    if m:
        space, addr, val, mask = m.groups()
        if int(mask, 16) == 0:
            return [op]
        return [f"{space}.RD addr={addr} val=UNDEFINED",
                f"{space}.WR addr={addr} val={val} mask={mask}"]
    return [op]


_RD_OF = re.compile(r"^(PHY|RAD)\.RD\s+addr=(\S+)\s+val=(\S+)")


def unfold_bus_seq(raw_ops):
    """unfold_bus() over a whole trace, with the one case that needs lookahead.

    The vendor tracer hooks the radio read and the radio write that
    radio_reg_mod() makes internally, so every RAD.MOD comes as a triple --
    the MOD, the RAD.RD it performed and the RAD.WR it performed, 435 of 435
    on the reference segment -- and the unit harness prints the same triple.
    Those two inner lines are the bus ops themselves, values included, so the
    MOD line stands for nothing on the bus and is dropped. PHY MODs carry no
    such shadow (52 of 3435 are followed by a read of the same register, no
    more than chance) and are left to unfold_bus().
    """
    out = []
    i = 0
    n = len(raw_ops)
    while i < n:
        op = " ".join(raw_ops[i].split())
        m = _MOD.match(norm(op))
        if m and m.group(1) == "RAD" and i + 2 < n:
            r = _RD_OF.match(norm(raw_ops[i + 1]))
            w = re.match(r"^RAD\.WR\s+addr=(\S+)", norm(raw_ops[i + 2]))
            if (r and r.group(1) == "RAD" and r.group(2) == m.group(2) and
                    w and w.group(1) == m.group(2)):
                i += 1
                continue
        out.extend(unfold_bus(op))
        i += 1
    return out


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

    BLK markers are the exception to the pairing: B43_AC_BLOCK is a point
    marker with no closing counterpart, so a block is closed here -- where the
    next sibling block opens, or where the enclosing function leaves. Blocks
    are siblings, not nested: two names for one stretch of trace is not a
    thing, and the innermost owner of an op inside a named section is that
    section.
    """
    ops, events, owner = [], [], []
    stack = []
    with open_trace(path) as f:
        for line in f:
            m = RE_MARKER.match(line.strip())
            if m:
                closing, kind, name = m.groups()
                if closing:
                    # close the blocks the leave implicitly ends, innermost
                    # first, then the owner itself
                    while stack and stack[-1][0] == "BLK" \
                            and stack[-1][1] != name:
                        events.append((len(ops), -1, stack[-1][1]))
                        stack.pop()
                    if any(n == name for _, n in stack):
                        events.append((len(ops), -1, name))
                        while stack.pop()[1] != name:
                            pass
                elif kind == "BLK":
                    # a block is a point marker: it ends where the next one
                    # begins, so an open sibling closes here
                    if stack and stack[-1][0] == "BLK":
                        events.append((len(ops), -1, stack[-1][1]))
                        stack.pop()
                    events.append((len(ops), +1, name))
                    stack.append((kind, name))
                else:
                    events.append((len(ops), +1, name))
                    stack.append((kind, name))
                continue
            m = RE_PORT.match(line)
            if m:
                ops.append(norm(m.group(1)))
                owner.append(stack[-1][1] if stack else None)
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


def materialize(path):
    """A real filesystem path for `path`, extracting from a zip if needed.

    Tools that hand the path to another process cannot pass the
    `archive.zip!inner.txt` form: only open_trace() understands it.
    """
    if "!" not in path:
        return path
    z, inner = path.split("!", 1)
    with zipfile.ZipFile(z) as zf:
        data = zf.read(inner)
    tmp = tempfile.NamedTemporaryFile(suffix=".txt", delete=False)
    tmp.write(data)
    tmp.close()
    return tmp.name


def merge_retvals(path):
    """Fold the RETVAL records in and return the path of the temporary.

    Needed before using a capture as an oracle or as a term of comparison:
    without the folding every read has `val=UNDEFINED` and the value is not
    comparable.
    """
    tmp = tempfile.NamedTemporaryFile(suffix=".merged", delete=False)
    tmp.close()
    r = subprocess.run([sys.executable,
                        os.path.join(ROOT, "reverse-tools", "trace_filter.py"),
                        "--retvals",
                        materialize(path), tmp.name], capture_output=True,
                       text=True)
    # An empty oracle is worse than a crash: every read falls through to the
    # mirror of the writes and the run diverges for a reason that looks like a
    # defect of the port. So the failure is raised, not swallowed.
    if r.returncode != 0 or os.path.getsize(tmp.name) == 0:
        raise RuntimeError(f"merge_retvals failed on {path}: "
                           f"{r.stderr.strip() or 'empty output'}")
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

    The harness must run with cwd=test/unit/ because the board profiles read the
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
    it. An owner's interval is then the first and last episode of the window
    its markers bracket.

    That window ENCLOSES what the callees emit: it is not the set of ops the
    owner emits itself. For an anchor that is the right answer -- a `#NNNN`
    inside a function is inside the stretch of capture that function is
    responsible for, whoever emitted the individual op -- and it is why a
    function's interval contains those of the blocks and functions inside it.
    For "who touched this register" the answer is different and comes from
    emitted_by_function(), which uses the innermost owner instead.

    An owner entered N times has N intervals, in call order: the min-max hull
    of all of them is not an interval it covers, and collapsing them would be
    a silent loss.
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
