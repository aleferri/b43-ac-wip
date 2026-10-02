#!/usr/bin/env python3
"""Rebuild the firmware b43 loads from a bus capture of wl's `up`.

A bus capture of the stock driver's bring-up holds everything b43 takes from
its firmware files, because wl writes the same data through the same ports:

    ucode       the OBJDATA words written while OBJADDR selects the ucode
                space, at the index the address auto-increments to;
    initvals    the first run of at least MIN_IV writes with no read in
                between after the ucode upload, from its last MACCMD write
                on: wl's init list is written blind, every other phase of
                the bring-up reads something back, and a command is the
                driver's, not a value of the list;
    bsinitvals  the first group of writes to the band registers after it.

The output is a directory b43 can load from (`b43_fw_header` + payload, the
names the AC port requests) and the raw big-endian ucode the d11 simulator
and the b43-tools interpreter take:

    fw_from_capture.py router-data/agcombo/ch36.bin -o /tmp/fw-agcombo

The files are derived from a proprietary driver; keep them out of the tree.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmio2ops  # noqa: E402

D11_LIMIT = 0x1000
OBJADDR, OBJDATA = 0x160, 0x164
SEL_UCODE = 0
MACCMD = 0x124
BAND_REGS = (0x686, 0x680, 0x682, 0x700, 0x684)
MIN_IV = 32


def ucode_words(ops):
    """The ucode image and the index of the op that ends its upload."""
    words, objaddr, last = {}, None, None
    for i, op in enumerate(ops):
        if op.kind != "W":
            continue
        if op.off == OBJADDR:
            objaddr = op.val
        elif op.off == OBJDATA and op.width == 4 and objaddr is not None \
                and (objaddr >> 16) & 0xff == SEL_UCODE:
            words[objaddr & 0xffff] = op.val
            if (objaddr >> 24) & 3:
                objaddr += 1
            last = i
    if not words:
        sys.exit("no ucode upload in the capture")
    image = [words.get(k, 0) for k in range(max(words) + 1)]
    return image, last


def blind_run(ops, start):
    """Bounds of the initvals run, as the module docstring defines it."""
    i = start
    while i < len(ops):
        j = i
        while j < len(ops) and ops[j].kind == "W" and ops[j].off < D11_LIMIT:
            j += 1
        if j - i >= MIN_IV:
            cmds = [k for k in range(i, j) if ops[k].off == MACCMD]
            return (cmds[-1] + 1 if cmds else i), j
        i = j + 1
    sys.exit("no initvals run after the ucode upload")


def band_group(ops, start):
    for i in range(start, len(ops) - len(BAND_REGS) + 1):
        group = ops[i:i + len(BAND_REGS)]
        if all(op.kind == "W" and op.off == reg
               for op, reg in zip(group, BAND_REGS)):
            return group
    sys.exit("no bsinitvals group after the initvals")


def fw_file(kind, payload, size_field):
    return struct.pack(">BBxxI", ord(kind), 1, size_field) + payload


def iv_file(writes):
    payload = b""
    for op in writes:
        if op.width == 4:
            payload += struct.pack(">HI", op.off | 0x8000, op.val)
        else:
            payload += struct.pack(">HH", op.off, op.val)
    return fw_file("i", payload, len(writes))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--format", choices=sorted(mmio2ops.PARSERS))
    ap.add_argument("--rev", type=int, default=42)
    ap.add_argument("-o", "--out", required=True, help="output directory")
    args = ap.parse_args()

    fmt = args.format or mmio2ops.guess_format(args.capture)
    ops = [op for op in mmio2ops.PARSERS[fmt](args.capture) if op.kind in "RW"]

    image, end = ucode_words(ops)
    lo, hi = blind_run(ops, end + 1)
    iv = ops[lo:hi]
    bs = band_group(ops, hi)

    raw = b"".join(struct.pack(">I", w) for w in image)
    os.makedirs(os.path.join(args.out, "b43"), exist_ok=True)
    files = {
        f"d11ucode{args.rev}.bin": raw,
        f"b43/ucode{args.rev}.fw": fw_file("u", raw, len(raw)),
        f"b43/ac1initvals{args.rev}.fw": iv_file(iv),
        f"b43/ac1bsinitvals{args.rev}.fw": iv_file(bs),
    }
    for name, data in files.items():
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(data)
    print(f"ucode {len(image)} words, initvals {len(iv)} (ops {lo}..{hi - 1}), "
          f"bsinitvals {len(bs)} -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
