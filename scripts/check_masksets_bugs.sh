#!/usr/bin/env bash
# 1) mostra le 3 chiamate con mask=~0x0000 (bug attivi confermati)
# 2) elenca le 37 non-valutabili con il source della chiamata + contesto,
#    così si vede se il "set" runtime è filtrato dal caller o no.

SRC=$(CDPATH= cd -- "$(dirname -- "$0")/../src" && pwd)

python3 - "$SRC" << 'PYEOF'
import re, glob, os, sys

SRC = sys.argv[1]

CAST = re.compile(r'\(\s*u(?:8|16|32)\s*\)')
def sanitize(e): return CAST.sub('', e.strip())

def try_eval_u16(expr):
    s = sanitize(expr)
    if not re.fullmatch(r'[0-9a-fA-FxX\s\(\)\~\|\&\^\<\>\-\+]*', s):
        return None
    try:
        v = eval(s, {"__builtins__": {}}, {})
        if isinstance(v, int): return v & 0xffff
    except Exception:
        return None
    return None

def split_args(argstr):
    args, depth, cur = [], 0, []
    for ch in argstr:
        if ch == '(': depth += 1; cur.append(ch)
        elif ch == ')': depth -= 1; cur.append(ch)
        elif ch == ',' and depth == 0: args.append(''.join(cur).strip()); cur=[]
        else: cur.append(ch)
    if cur: args.append(''.join(cur).strip())
    return args

def find_calls(path):
    src_text = open(path).read()
    src_lines = src_text.splitlines(keepends=False)
    out = []
    for m in re.finditer(r'b43_phy_maskset\s*\(', src_text):
        i = m.end(); depth = 1; start = i
        while i < len(src_text) and depth > 0:
            if src_text[i] == '(': depth += 1
            elif src_text[i] == ')': depth -= 1
            i += 1
        line_no = src_text.count('\n', 0, m.start()) + 1
        end_line_no = src_text.count('\n', 0, i) + 1
        out.append((line_no, end_line_no, src_text[start:i-1], src_lines))
    return out

def show_ctx(lines, ln, before=2, after=1):
    lo = max(0, ln - 1 - before); hi = min(len(lines), ln + after)
    for j in range(lo, hi):
        mark = ' >>> ' if j == ln - 1 else '     '
        print(f'{mark}{j+1:5d}: {lines[j]}')

print("=" * 70)
print("GRUPPO A - Bug ATTIVI (set con bit nella zona 'preservata' da mask):")
print("=" * 70)
for path in sorted(glob.glob(os.path.join(SRC, '*.c'))):
    for line, endline, argstr, src_lines in find_calls(path):
        args = split_args(argstr)
        if len(args) != 4: continue
        _, _, mask_e, set_e = args
        m = try_eval_u16(mask_e); s = try_eval_u16(set_e)
        if m is None or s is None: continue
        if (s & m) != 0:
            print(f"\n{path}:{line} — mask=0x{m:04x} set=0x{s:04x} spurious=0x{s&m:04x}")
            show_ctx(src_lines, line, before=3, after=max(0, endline-line))

print()
print("=" * 70)
print("GRUPPO B - NON valutabili (set/mask runtime): il caller filtra?")
print("=" * 70)
for path in sorted(glob.glob(os.path.join(SRC, '*.c'))):
    for line, endline, argstr, src_lines in find_calls(path):
        args = split_args(argstr)
        if len(args) != 4: continue
        _, _, mask_e, set_e = args
        m = try_eval_u16(mask_e); s = try_eval_u16(set_e)
        if m is not None and s is not None: continue
        print(f"\n{path}:{line} — mask={mask_e!r} set={set_e!r}")
        show_ctx(src_lines, line, before=3, after=max(0, endline-line))
PYEOF
