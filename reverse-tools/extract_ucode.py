#!/usr/bin/env python3
"""Extract a d11 microcode blob from a Broadcom wl object by symbol.

The wl objects carry the per-corerev ucode as global arrays named d11ucodeNN
(and d11ucodeNN_variant), each paired with a d11ucodeNNsz word. This reads the
array straight out of its section at the symbol's offset and size, so the
output is the exact bytes the driver would upload, big-endian as stored.

    extract_ucode.py WL_OBJECT --rev 42 -o d11ucode42.bin
    extract_ucode.py WL_OBJECT --symbol d11ucode42 -o out.bin
    extract_ucode.py WL_OBJECT --list

The blob is derived from a proprietary binary; keep it out of the tree, as
PROVENANCE.md records for the wl objects themselves. b43 consumes ucode through
linux-firmware / b43-fwcutter, which wraps the same bytes in a b43_fw_header;
this tool emits the raw array for the simulator, not that firmware file.
"""
import argparse
import struct
import sys

from elftools.elf.elffile import ELFFile


def sym_bytes(elf, f, sym):
    sec = elf.get_section(sym["st_shndx"])
    off = sec["sh_offset"] + (sym["st_value"] - sec["sh_addr"])
    f.seek(off)
    return sec.name, off, f.read(sym["st_size"])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("obj")
    ap.add_argument("--rev", type=int, help="corerev, selects symbol d11ucode<rev>")
    ap.add_argument("--symbol", help="exact ucode symbol name")
    ap.add_argument("-o", "--out", help="output blob path")
    ap.add_argument("--list", action="store_true", help="list d11ucode* symbols")
    args = ap.parse_args()

    with open(args.obj, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")
        if symtab is None:
            sys.exit("no .symtab: object is stripped")

        ucode_syms = {s.name: s for s in symtab.iter_symbols()
                      if s.name.startswith("d11ucode") and s["st_size"] > 64}

        if args.list:
            for name in sorted(ucode_syms, key=lambda n: -ucode_syms[n]["st_size"]):
                print(f"{name:28s} {ucode_syms[name]['st_size']:8d} bytes")
            return 0

        name = args.symbol or (f"d11ucode{args.rev}" if args.rev is not None else None)
        if not name:
            sys.exit("give --rev, --symbol or --list")
        if name not in ucode_syms:
            sys.exit(f"{name} not found; try --list")

        secname, off, data = sym_bytes(elf, f, ucode_syms[name])

        szname = name + "sz"
        szsym = next((s for s in symtab.iter_symbols() if s.name == szname), None)
        decl = None
        if szsym is not None:
            _, _, szdata = sym_bytes(elf, f, szsym)
            if len(szdata) == 4:
                decl = struct.unpack(">I", szdata)[0]

        print(f"{name}: section={secname} file_off=0x{off:x} "
              f"size={len(data)}" + (f" (declared {decl})" if decl is not None else ""),
              file=sys.stderr)
        if decl is not None and decl != len(data):
            print(f"WARNING: {szname}={decl} != array size {len(data)}", file=sys.stderr)

        out = args.out or (name + ".bin")
        with open(out, "wb") as o:
            o.write(data)
        print(f"-> {out} ({len(data)} bytes, {len(data)//4} words)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
