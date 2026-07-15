#!/usr/bin/env python3
"""Verify NVRAM values against the trace *where the driver consumes them*.

Unlike a global value scan, this checks only the four SROM inputs the ported
b43 AC-PHY driver actually reads from bus_sprom, each inside the trace window
of the function that consumes it, applying that function's transform:

  rxchain        -> coremask                 (phy_ac.c:3783)  structural
  subband5gver   -> pa5g_group(freq)         (phy_ac.c:176)   slice selector
  rxgains_5gl    -> rxgain_init              (phy_ac.c:3234)  transformed regs
  core_pwr_info  -> txpwrctrl_setup est_pwr  (phy_ac.c:637)   transfer function

Everything else in the NVRAM dump is not read by this driver (hardcoded from
capture, consumed off the PHY path by the b43 core/MAC, or part of the
still-unported calibration blocks), so it is intentionally out of scope.
"""
import re
import sys
from pathlib import Path

NV_RE = re.compile(r'^([a-z0-9_]+)=(.+)$')


def load_nvram(path):
    nv = {}
    for line in Path(path).read_text().splitlines():
        m = NV_RE.match(line.strip())
        if m:
            nv[m.group(1)] = m.group(2)
    return nv


def hexlist(s):
    return [int(x, 16) if x.lower().startswith('0x') else int(x)
            for x in s.split(',')]


def cdiv(a, b):                      # C integer division: truncate toward zero
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def est_pwr_lut(a1, b0, b1):
    def s16(x):
        return x - 0x10000 if x >= 0x8000 else x
    a1, b0, b1 = s16(a1), s16(b0), s16(b1)
    num, den, out = b0 << 9, 0x8000, []
    for _ in range(128):
        d = den or 1
        v = max(-8, min(0x7f, cdiv(d // 2 + num, d)))
        out.append(v & 0xff)
        num += b1 * 0x20
        den += a1
    return out


def pa5g_group(freq, subband5gver):
    if subband5gver == 4:
        return 0 if freq < 5250 else 1 if freq < 5500 else 2 if freq < 5700 else 3
    return 0                          # other versions not exercised here


def read_lines(paths):
    out = []
    for p in paths:
        out += Path(p).read_text(errors='replace').splitlines()
    return out


def find_reg(lines, addr, val, mask=None):
    pat = f'addr=0x{addr:04x} val=0x{val:04x}'
    if mask is not None:
        pat += f' mask=0x{mask:04x}'
    return [re.search(r'#(\d+)', l).group(1) for l in lines if pat in l]


def table_data(lines, tid):
    """First write of PHY table `tid`: the sequence of 0x000f data bytes."""
    key = f'TBL.WR   id=0x{tid:04x}'
    for i, l in enumerate(lines):
        if key in l:
            data, started = [], False
            for l2 in lines[i + 1:]:
                m = re.search(r'addr=0x000f val=0x([0-9a-fA-F]+)', l2)
                if m:
                    data.append(int(m.group(1), 16) & 0xff)
                    started = True
                elif started and ('TBL.WR' in l2 or 'addr=0x000d' in l2):
                    break
            return data
    return None


def verify(board, nvram_path, trace_paths, freq=5180):
    nv = load_nvram(nvram_path)
    lines = read_lines(trace_paths)
    print(f"\n{'='*70}\n{board}   (channel freq {freq} MHz)\n{'='*70}")

    # 1. rxchain -> coremask -> number of active per-core est_pwr tables
    coremask = int(nv['rxchain']) & 7 or 3
    n_cores = bin(coremask).count('1')
    written = [tid for tid in (0x40, 0x60, 0x80) if table_data(lines, tid)]
    print(f"[rxchain={nv['rxchain']}] coremask=0x{coremask:x} -> {n_cores} cores; "
          f"est_pwr tables written: {[hex(t) for t in written]}  "
          f"{'OK' if len(written) == n_cores else 'MISMATCH'}")

    # 2. subband5gver -> pa5g group slice for this freq
    sv = int(nv['subband5gver'], 16)
    grp = pa5g_group(freq, sv)
    print(f"[subband5gver=0x{sv:x}] pa5g group for {freq} MHz = {grp} "
          f"(uses pa5gaX[{grp*3}:{grp*3+3}])")

    # 3. rxgains_5gl -> rxgain_init transforms (per core)
    for core in range(n_cores):
        triso = int(nv[f'rxgains5gtrisoa{core}'])
        elna = int(nv[f'rxgains5gelnagaina{core}'])
        gainctx = ((triso + 4) << 1) + 2
        hits = find_reg(lines, 0x06f9 + core * 0x200,
                        (gainctx << 8) & 0x7f00, 0x7f00)
        print(f"[rxgains_5gl core{core}] triso={triso} -> gainctx=0x{gainctx:02x} "
              f"-> reg 0x{0x6f9+core*0x200:04x} val=0x{(gainctx<<8)&0x7f00:04x}: "
              f"{'FOUND ' + hits[0] if hits else 'not found'}")

    # 4. core_pwr_info.pa5ga -> est_pwr transfer function -> tables
    tid_for = {0: 0x40, 1: 0x60, 2: 0x80}
    for core in range(n_cores):
        pa = hexlist(nv[f'pa5ga{core}'])[grp * 3: grp * 3 + 3]
        want = est_pwr_lut(*pa)
        got = table_data(lines, tid_for[core])
        n = sum(a == b for a, b in zip(got or [], want))
        print(f"[pa5ga{core}[grp{grp}]] est_pwr LUT -> tbl 0x{tid_for[core]:02x}: "
              f"{n}/128 bytes {'EXACT' if got == want else 'DIFF'}")


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
    rd = root / 'router-data'
    verify('d6220', rd / 'd6220/wl1_nvram.txt',
           [rd / 'd6220/wl-diag-wl1-down-to-bss-up.txt'])
    verify('agcombo', rd / 'agcombo/agcombo_nvram.txt',
           [rd / 'agcombo/agcombo-wl1-4360-down-to-bss-ch36.txt'])


if __name__ == '__main__':
    main()
