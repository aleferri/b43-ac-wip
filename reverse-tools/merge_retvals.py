#!/usr/bin/env python3
"""Fold RETVAL and ARGX lines back into their antecedent op.

The wl-diag "capture ret val" format logs a read as `... val=UNDEFINED` and
then, on one or more *later* lines, the value actually returned and any extra
call arguments, tied to the source op by `for=#N`:

    #16 SI.COREREG core=0x0000 off=0x0660 val=UNDEFINED
    #17 ARGX     for=#16 a5=0x00000002 a6=0x00000000
    #18 RETVAL   for=#16 val=0x00000002

This merges each RETVAL/ARGX into op #N and drops the auxiliary lines, so every
op carries its real value inline:

  - a read (val=UNDEFINED) gets UNDEFINED replaced by the returned value;
  - an op that already has a written val= keeps it and gains ` ret=<v>`
    (write-then-readback, e.g. PMU.PLL);
  - ARGX operands are appended as ` a5=.. a6=..`.

Order is irrelevant: ops are indexed first, then attachments applied, so a
RETVAL that precedes or follows its op both work.

Usage: merge_retvals.py IN [OUT]      (OUT omitted -> stdout)
"""
import re
import sys

VAL_RE = re.compile(r'val=(0x[0-9a-fA-F]+|UNDEFINED)')
A5_RE = re.compile(r'\ba5=(0x[0-9a-fA-F]+)')
A6_RE = re.compile(r'\ba6=(0x[0-9a-fA-F]+)')


def ep_of(parts):
    """Episode number from the '#NNN' token (field 1), or None."""
    if len(parts) > 1 and parts[1].startswith('#'):
        return parts[1][1:]
    return None


def for_of(line):
    m = re.search(r'\bfor=#(\d+)\b', line)
    return m.group(1) if m else None


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: merge_retvals.py IN [OUT]")
    src, dst = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else None)

    lines = open(src, encoding='utf-8', errors='replace').read().splitlines()

    aux = [False] * len(lines)     # RETVAL / ARGX lines, dropped from output
    ret = {}                       # ep -> returned value
    args = {}                      # ep -> (a5, a6)

    for i, ln in enumerate(lines):
        parts = ln.split()
        op = parts[3] if len(parts) > 3 else ''
        if op == 'RETVAL':
            aux[i] = True
            tgt = for_of(ln)
            m = VAL_RE.search(ln)
            if tgt and m:
                ret[tgt] = m.group(1)
        elif op == 'ARGX':
            aux[i] = True
            tgt = for_of(ln)
            if tgt:
                a5 = A5_RE.search(ln)
                a6 = A6_RE.search(ln)
                args[tgt] = (a5.group(1) if a5 else None,
                             a6.group(1) if a6 else None)

    n_ret = n_arg = 0
    out = []
    for i, ln in enumerate(lines):
        if aux[i]:
            continue
        ep = ep_of(ln.split())
        if ep is not None and ep in ret:
            rv = ret[ep]
            if rv != 'UNDEFINED':
                if 'val=UNDEFINED' in ln:
                    ln = ln.replace('val=UNDEFINED', 'val=' + rv)
                else:
                    ln = ln + ' ret=' + rv
                n_ret += 1
        if ep is not None and ep in args:
            a5, a6 = args[ep]
            if a5:
                ln += ' a5=' + a5
            if a6:
                ln += ' a6=' + a6
            n_arg += 1
        out.append(ln)

    text = '\n'.join(out) + '\n'
    if dst:
        open(dst, 'w', encoding='utf-8').write(text)
        sys.stderr.write("%s: %d op, %d retval, %d argx merged\n"
                         % (dst, len(out), n_ret, n_arg))
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
