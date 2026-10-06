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
import bisect
import collections
import difflib
import os
import re
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "b43")
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
    and on the offset-in-bytes form of the address it adds nothing. And so is
    `reg=`, the register name reverse-tools/mmio2ops.py appends for the
    reader: the offset already says which register it is.
    """
    op = " ".join(op.split())
    op = re.sub(r"^GPIO\.OUTEN\b", "GPIO.OE", op)
    op = re.sub(r"\s+(ret|a5|a6|sel|reg)=\S+", "", op)
    op = _HEX.sub(lambda m: "0x" + m.group(1).lower(), op)
    m = re.match(r"PHY\.(AND|OR)\s+addr=(\S+)\s+val=(\S+)", op)
    if m:
        op = f"PHY.MOD addr={m.group(2)} val={m.group(3)} mask=0x0"
    return re.sub(r"\s*\((set|clr)[^)]*\)", "", op)


_MOD = re.compile(r"^(PHY|RAD)\.MOD\s+addr=(\S+)\s+val=(\S+)\s+mask=(\S+)$")
_ANDOR = re.compile(r"^(PHY|RAD)\.(AND|OR)\s+addr=(\S+)\s+val=(\S+)")
_ACCESSOR_ONLY = re.compile(r"^(TBL\.(WR|RD)|MAC\.MHF|AMT\.WR)\b")

# A row of the address match table (RCMTA, selector 4) as the wl-diag hooks
# see it: a bulk header of 8 bytes. On the bus it is two 32-bit words at word
# index addr/4, the read of both before the write of both.
_RCMTA_BULK = re.compile(r"^OBJ\.BULK([RW]) addr=(0x[0-9a-fA-F]+) len=(\d+)"
                         r".*\ba5=0x0*40000\b")


def unfold_bus(op):
    """The op as the bus sees it: zero, one or two ops.

    A trace taken at the MMIO bus -- test/integration -- has no accessor-level
    classes. A read-modify-write is a read and a write of the same register;
    a table access is the words on the data port, and the TBL marker that
    names the table stands for nothing on the bus; a host-flag maskset is a
    software shadow whose write-through, when it happens, is its own OBJ.WR
    on both sides; an address-match row is the logical AMT.WR record, which is
    nothing on the bus, and its 8-byte bulk, which is two 32-bit words. This
    maps an accessor-level
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
    m = _RCMTA_BULK.match(op)
    if m:
        kind, addr, n = m.group(1), int(m.group(2), 16) // 4, int(m.group(3)) // 4
        return [norm(f"OBJ.{kind}D addr=0x{addr + k:04x} val=UNDEFINED"
                     if kind == "R" else
                     f"OBJ.WR addr=0x{addr + k:04x} val=UNDEFINED")
                for k in range(n)]
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
_WIDE = re.compile(r"^PHY\.(RD|WR)W\s+(val=\S+)")
_PHY_SEL = re.compile(r"^PHY\.(?:RD|WR|MOD|AND|OR)\s+addr=(\S+)")


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

    A wide PHY access (PHY.RDW, PHY.WRW: the data port with no register
    select of its own) goes to the register the previous PHY access
    selected, and becomes a PHY.RD or PHY.WR of it, so a side that reselects
    every word and one that does not compare equal.
    """
    out = []
    sel = None
    i = 0
    n = len(raw_ops)
    while i < n:
        op = " ".join(raw_ops[i].split())
        w = _WIDE.match(op)
        if w and sel is not None:
            out.append(norm(f"PHY.{w.group(1)} addr={sel} {w.group(2)}"))
            i += 1
            continue
        a = _PHY_SEL.match(norm(op))
        if a:
            sel = a.group(1)
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
    pos_to_ep = {}
    for tag, i, i2, j, _ in align_opcodes([o.norm for o in vendor], ops):
        if tag == 'equal':
            for k in range(i2 - i):
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


# ---------------------------------------------------------------------------
# Alignment
# ---------------------------------------------------------------------------

SEED = 8
MIN_BLOCK = 2
SEED_MAX_REPEAT = 64
_GAP_FALLBACK = 4_000_000


def _seeds(a, alo, ahi, b, blo, bhi, k):
    """(i, j) pairs of runs of k items found in both ranges.

    A run found once on each side pairs with itself. A run repeated -- the
    same watchdog turn, the same statistics dump, once per second -- pairs
    its occurrences in order, the first with the first, up to
    SEED_MAX_REPEAT of them; the chain of seeds drops the pairs that do not
    fit. A run more common than that says nothing about where it is.
    """
    def grams(s, lo, hi):
        pos = collections.defaultdict(list)
        for i in range(lo, hi - k + 1):
            pos[tuple(s[i:i + k])].append(i)
        return pos
    ga = grams(a, alo, ahi)
    gb = grams(b, blo, bhi)
    out = []
    for g, pa in ga.items():
        pb = gb.get(g)
        if pb and max(len(pa), len(pb)) <= SEED_MAX_REPEAT:
            out.extend(zip(pa, pb))
    return sorted(out)


def _increasing_chain(pairs):
    """The longest subsequence of pairs, sorted by i, increasing in j."""
    tails, tails_j, prev = [], [], [None] * len(pairs)
    for n, (_, j) in enumerate(pairs):
        p = bisect.bisect_left(tails_j, j)
        if p:
            prev[n] = tails[p - 1]
        if p == len(tails):
            tails.append(n)
            tails_j.append(j)
        else:
            tails[p] = n
            tails_j[p] = j
    chain, n = [], tails[-1] if tails else None
    while n is not None:
        chain.append(pairs[n])
        n = prev[n]
    return chain[::-1]


def _blocks(a, b, k):
    """Matching blocks (i, j, n), in order, found from runs of k items.

    A run of k consecutive items found in both ranges anchors the two
    sequences to each other (see _seeds()); the longest chain of anchors in the same
    order on both sides is taken, each anchor is extended while the items
    agree, and the gaps between anchors are searched again the same way,
    where a run that was too common in the whole may not be in the gap.
    A gap with no anchor left is handed to difflib, which on a gap that
    small is cheap.
    """
    out = []
    stack = [(0, len(a), 0, len(b))]
    while stack:
        alo, ahi, blo, bhi = stack.pop()
        if alo >= ahi or blo >= bhi:
            continue
        seeds = _seeds(a, alo, ahi, b, blo, bhi, k)
        found = []
        ia, ib = alo, blo
        for i, j in _increasing_chain(seeds):
            if i < ia or j < ib:
                continue
            s, t = i, j
            while s > ia and t > ib and a[s - 1] == b[t - 1]:
                s, t = s - 1, t - 1
            e, f = i, j
            while e < ahi and f < bhi and a[e] == b[f]:
                e, f = e + 1, f + 1
            found.append((s, t, e - s))
            ia, ib = e, f
        if not found:
            if (ahi - alo) * (bhi - blo) <= _GAP_FALLBACK:
                sm = difflib.SequenceMatcher(None, a[alo:ahi], b[blo:bhi],
                                             autojunk=False)
                out.extend((alo + i, blo + j, n)
                           for i, j, n in sm.get_matching_blocks() if n)
            else:
                print(f"tracelib: {ahi - alo} x {bhi - blo} ops with no run "
                      f"of {k} in common, left unaligned", file=sys.stderr)
            continue
        out.extend(found)
        edges = [(alo, blo)] + [(s + n, t + n) for s, t, n in found]
        ends = [(s, t) for s, t, _ in found] + [(ahi, bhi)]
        for (x0, y0), (x1, y1) in zip(edges, ends):
            stack.append((x0, x1, y0, y1))
    return sorted(out)


def align_opcodes(a, b, seed=SEED, min_block=MIN_BLOCK):
    """Opcodes like difflib.SequenceMatcher.get_opcodes(), without the
    matches that are not matches.

    difflib builds its blocks from single equal items, which is quadratic in
    the repetitions of an item -- 40000 PHY address read-backs per side on a
    bus capture -- and, in the leftovers between the long blocks, accepts a
    single item as a block. One op equal to one op is not evidence that the
    two sequences are at the same point; here a block shorter than min_block
    is not a match, and the search starts from runs of seed items.
    """
    ops, i, j = [], 0, 0
    for s, t, n in _blocks(a, b, seed):
        if n < min_block:
            continue
        if i < s or j < t:
            tag = ('replace' if i < s and j < t else
                   'delete' if i < s else 'insert')
            ops.append((tag, i, s, j, t))
        ops.append(('equal', s, s + n, t, t + n))
        i, j = s + n, t + n
    if i < len(a) or j < len(b):
        tag = ('replace' if i < len(a) and j < len(b) else
               'delete' if i < len(a) else 'insert')
        ops.append((tag, i, len(a), j, len(b)))
    return ops
