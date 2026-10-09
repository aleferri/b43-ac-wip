#!/usr/bin/env python3
# Decoder of the wl_mmio_trap records (MIPS big-endian, 28 bytes per record).
#
# Typical use, in a pipe like the wl-diag decoder (the device does:
# cat /proc/wl_mmio_trap | nc -u host 5555):
#     nc -u -l -p 5555 | ./decode-wl-mmio.py
# or from a file:
#     ./decode-wl-mmio.py < capture.bin
#     ./decode-wl-mmio.py capture.bin
#
# Framing: it is a byte stream, read in blocks and consumed 28 bytes at a
# time. The same record layout as wl_diag's -- ts_ns, seq, addr, val, aux,
# op, cpu, pad -- and op codes taken from the free part of its enum, so the
# two streams can be merged on the timestamp: both stamp with sched_clock().
#
# MMIO.RD (60) / MMIO.WR (61): raw access to the register window, which the
# hooks on wl_diag's functions do not see because there is no call to hook.
# addr is the OFFSET into the window, not a VA: the virtual address changes
# from one boot to the next and tells a decoder nothing. aux carries the width
# of the access in the low byte and bit 8 when the instruction sat in a delay
# slot.
# MARK (62): a label injected from userspace with
#     echo "mark ch36 bw20" > /proc/wl_mmio_trap
# 12 characters packed big-endian into the three u32 fields, like wl_diag's
# MARK. It is queued like every other record, so it is ordered against the
# accesses around it and not against the clock of whoever writes it.
# DROP (255): the queue overflowed; aux carries how many records were lost.
# The number matches the gap in the sequence numbers, so the two counts check
# each other.
# DMA.DD (63) + DMA.DATA (64): a TX descriptor read from memory at the write
# of its channel's index (dma_dd.h). DD carries the descriptor's bus address
# in addr, the buffer's in val, and in aux the channel (31:24), the DD_F_*
# flags (23:16) and the buffer bytes (15:0); the DATA records that follow,
# 12 bytes each, carry the 16 bytes of the descriptor as they are in memory
# and then those of the buffer. The d11 AC TX header is read by
# ../reverse-tools/d11ac_txh.py, the same one decode-wl-diag.py uses.
import os, sys, struct, argparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "reverse-tools"))
import d11ac_txh

REC = struct.Struct(">QIIIIBBH")   # ts_ns, seq, addr, val, aux, op, cpu, _pad
SZ = REC.size                       # 28

MMIO_RD, MMIO_WR, MARK, DMA_DD, DMA_DATA, DROP = 60, 61, 62, 63, 64, 255
OPS = {MMIO_RD: "MMIO.RD", MMIO_WR: "MMIO.WR", MARK: "MARK",
       DMA_DD: "DMA.DD", DMA_DATA: "DMA.DATA"}

DD_SIZE = 16
DD_F_BE, DD_F_NOBUF, DD_F_GUESS = 0x01, 0x02, 0x04

AUX_WIDTH = 0xff
AUX_DELAY_SLOT = 0x100


def h(v, wide):
    return f"0x{v:08x}" if wide else f"0x{v:04x}"


def dd_lines(chan, slot, bufaddr, flags, raw, buf, hexdump):
    be = flags & DD_F_BE
    w = struct.unpack(">4I" if be else "<4I", raw)
    bc = w[1] & 0x7fff
    fl = [n for b, n in ((DD_F_BE, "be"), (DD_F_NOBUF, "nobuf"),
                         (DD_F_GUESS, "guess")) if flags & b]
    out = [f"ch={chan} dd={slot:#010x} buf={bufaddr:#010x} ctl0={w[0]:#010x} "
           f"ctl1={w[1]:#010x} bytes={bc} addrhi={w[3]:#x}"
           + (f" [{','.join(fl)}]" if fl else "")]
    if not buf:
        return out
    txh = d11ac_txh.describe(buf, bc)
    if hexdump or txh is None:
        if txh is None:
            out.append("  layout non riconosciuto:")
        out += ["  " + line for line in d11ac_txh.hexdump(buf)]
    if txh:
        out += ["  " + line for line in txh]
    return out


def unmark(addr, val, aux):
    """the 12 bytes packed into three u32, up to the first NUL"""
    b = b"".join(w.to_bytes(4, "big") for w in (addr, val, aux))
    return b.split(b"\x00")[0].decode("ascii", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", nargs="?",
                    help="file da leggere; assente = stdin, per stare in pipe")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0,
                    help="sommato a ogni offset, per avere indirizzi assoluti")
    ap.add_argument("--since", metavar="LABEL",
                    help="scarta tutto prima del MARK con questa etichetta")
    ap.add_argument("--until", metavar="LABEL",
                    help="fermati al MARK con questa etichetta")
    ap.add_argument("--hex", action="store_true",
                    help="stampa anche i byte grezzi dei buffer TX")
    args = ap.parse_args()

    f = open(args.capture, "rb") if args.capture else sys.stdin.buffer
    buf = b""
    emit = args.since is None
    dd = None       # a descriptor waiting for its DMA.DATA

    while True:
        chunk = f.read(4096)
        if not chunk:
            break
        buf += chunk
        while len(buf) >= SZ:
            ts, seq, addr, val, aux, op, cpu, _ = REC.unpack(buf[:SZ])
            buf = buf[SZ:]
            name = OPS.get(op, f"op{op}")
            t = ts / 1e9

            if op == DMA_DATA:
                if dd is None:
                    if emit:
                        print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                              "orfano (DMA.DD perso)")
                    continue
                dd["data"] += b"".join(x.to_bytes(4, "big")
                                       for x in (addr, val, aux))
                if len(dd["data"]) < dd["need"]:
                    continue
                data = dd["data"]
                if emit:
                    lines = dd_lines(dd["chan"], dd["slot"], dd["buf"],
                                     dd["flags"], data[:DD_SIZE],
                                     data[DD_SIZE:dd["need"]], args.hex)
                    print(f"{dd['t']:14.6f} #{dd['seq']:<8} cpu{dd['cpu']} "
                          f"{'DMA.DD':<8} {lines[0]}")
                    for line in lines[1:]:
                        print(" " * 15 + line)
                    sys.stdout.flush()
                dd = None
                continue
            if dd is not None:
                if emit:
                    print(f"{dd['t']:14.6f} #{dd['seq']:<8} cpu{dd['cpu']} "
                          f"{'DMA.DD':<8} troncato: "
                          f"{len(dd['data'])}/{dd['need']} byte")
                dd = None
            if op == DMA_DD:
                dd = {"t": t, "seq": seq, "cpu": cpu, "slot": addr,
                      "buf": val, "chan": aux >> 24,
                      "flags": (aux >> 16) & 0xff,
                      "need": DD_SIZE + (aux & 0xffff), "data": b""}
                continue

            if op == MARK:
                label = unmark(addr, val, aux)
                if args.since and label == args.since:
                    emit = True
                if emit:
                    print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} {label!r}")
                    sys.stdout.flush()
                if args.until and label == args.until:
                    return
                continue

            if not emit:
                continue

            if op == DROP:
                print(f"{t:14.6f}  cpu{cpu}  ** DROP **  persi={aux}")
            elif op in (MMIO_RD, MMIO_WR):
                width = aux & AUX_WIDTH
                ds = " ds" if aux & AUX_DELAY_SLOT else ""
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(args.base + addr, False)} "
                      f"val={h(val, width == 4)} w={width}{ds}")
            else:
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={h(val, True)} aux={h(aux, True)}")
            sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except (BrokenPipeError, KeyboardInterrupt):
        pass
