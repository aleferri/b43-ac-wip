#!/usr/bin/env python3
# Decoder dei record wl_mmio_trap (MIPS big-endian, 28 byte/record).
#
# Uso tipico, in pipe come il decoder di wl-diag (il device fa:
# cat /proc/wl_mmio_trap | nc -u host 5555):
#     nc -u -l -p 5555 | ./decode-wl-mmio.py
# oppure da file:
#     ./decode-wl-mmio.py < capture.bin
#     ./decode-wl-mmio.py capture.bin
#
# Framing: e' uno stream di byte, si legge a blocchi e si consuma 28 byte
# alla volta. Stesso layout di record di wl_diag -- ts_ns, seq, addr, val,
# aux, op, cpu, pad -- e op-code presi dalla parte libera del suo enum, in
# modo che i due flussi si possano fondere sul timestamp: entrambi timbrano
# con sched_clock().
#
# MMIO.RD (60) / MMIO.WR (61): accesso grezzo alla finestra dei registri,
# quello che gli hook sulle funzioni di wl_diag non vedono perche' non c'e'
# nessuna chiamata da agganciare. addr e' l'OFFSET nella finestra, non un VA:
# l'indirizzo virtuale cambia da un avvio all'altro e non dice niente a un
# decoder. aux porta la larghezza dell'accesso nel byte basso e il bit 8
# quando l'istruzione stava in un delay slot.
# MARK (62): etichetta iniettata dallo spazio utente con
#     echo "mark ch36 bw20" > /proc/wl_mmio_trap
# 12 caratteri impacchettati big-endian nei tre campi u32, come il MARK di
# wl_diag. Va in coda come ogni altro record, quindi e' ordinato rispetto
# agli accessi che lo circondano e non rispetto all'orologio di chi lo
# scrive.
# DROP (255): la coda ha tracimato; aux porta quanti record sono andati
# persi. Il numero corrisponde al buco nei numeri di sequenza, quindi le due
# contabilita' si controllano a vicenda.
# DMA.DD (63) + DMA.DATA (64): un descrittore TX letto dalla memoria alla
# scrittura dell'indice del suo canale (dma_dd.h). DD porta in addr
# l'indirizzo di bus del descrittore, in val quello del buffer, in aux il
# canale (31:24), i flag DD_F_* (23:16) e i byte di buffer (15:0); i DATA
# che seguono, 12 byte ciascuno, portano i 16 byte del descrittore come
# stanno in memoria e poi quelli del buffer. Il decoder riconosce l'header
# TX d11 AC (124 byte, con o senza i 4 del TX offload header davanti) dal
# frame_len che torna con il byte count del descrittore, e ne stampa i
# campi, i blocchi di rate con le PHY TX control word e il PLCP letto come
# L-SIG, HT-SIG o VHT-SIG-A secondo il frame type, e l'header 802.11.
import sys, struct, argparse

REC = struct.Struct(">QIIIIBBH")   # ts_ns, seq, addr, val, aux, op, cpu, _pad
SZ = REC.size                       # 28

MMIO_RD, MMIO_WR, MARK, DMA_DD, DMA_DATA, DROP = 60, 61, 62, 63, 64, 255
OPS = {MMIO_RD: "MMIO.RD", MMIO_WR: "MMIO.WR", MARK: "MARK",
       DMA_DD: "DMA.DD", DMA_DATA: "DMA.DATA"}

DD_SIZE = 16
DD_F_BE, DD_F_NOBUF, DD_F_GUESS = 0x01, 0x02, 0x04
D11AC_TXH_LEN = 124
FT = {0: "CCK", 1: "OFDM", 2: "HT", 3: "VHT"}
OFDM_RATE = {0xb: 6, 0xf: 9, 0xa: 12, 0xe: 18, 0x9: 24, 0xd: 36, 0x8: 48,
             0xc: 54}
CHSPEC_BW = {0x0800: "10", 0x1000: "20", 0x1800: "40", 0x2000: "80",
             0x2800: "160", 0x3000: "80+80"}

AUX_WIDTH = 0xff
AUX_DELAY_SLOT = 0x100


def h(v, wide):
    return f"0x{v:08x}" if wide else f"0x{v:04x}"


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def chanspec(v):
    bw = CHSPEC_BW.get(v & 0x3800, f"?{v & 0x3800:#x}")
    band = "5g" if v & 0xc000 == 0xc000 else "2g" if v & 0xc000 == 0 else "?"
    return f"ch{v & 0xff} bw{bw} sb{(v >> 8) & 7} {band}"


def plcp_text(ft, p):
    """Il PLCP del blocco di rate letto secondo il frame type."""
    if ft == 0:
        return f"cck signal={p[0]:#04x}"
    if ft == 1:
        sig = p[0] | (p[1] << 8) | (p[2] << 16)
        rate = OFDM_RATE.get(sig & 0xf, f"?{sig & 0xf:#x}")
        return f"l-sig {rate}M len={(sig >> 5) & 0xfff}"
    if ft == 2:
        return (f"ht-sig mcs{p[0] & 0x7f} {'40' if p[0] & 0x80 else '20'}MHz "
                f"len={p[1] | (p[2] << 8)} b3={p[3]:#04x} b4={p[4]:#04x} "
                f"b5={p[5]:#04x}")
    a1 = p[0] | (p[1] << 8) | (p[2] << 16)
    a2 = p[3] | (p[4] << 8) | (p[5] << 16)
    bw = {0: "20", 1: "40", 2: "80", 3: "160"}[a1 & 3]
    stbc = (a1 >> 3) & 1
    nsts = ((a1 >> 10) & 7) + 1
    return (f"vht-sig-a {bw}MHz mcs{(a2 >> 4) & 0xf} nsts={nsts} stbc={stbc} "
            f"gid={(a1 >> 4) & 0x3f} sgi={a2 & 1} ldpc={(a2 >> 2) & 1} "
            f"a1={a1:#08x} a2={a2:#08x}")


def txh_offset(buf, bytecount):
    """Dove comincia l'header TX d11 AC nel buffer, o None: frame_len
    (+10 nell'header) deve tornare con il byte count del descrittore, sia
    con il frame nello stesso buffer sia in un descrittore a parte."""
    for off in (4, 0):
        if len(buf) < off + 12:
            continue
        fl = u16(buf, off + 10)
        if bytecount in (off + D11AC_TXH_LEN + fl - 4, off + D11AC_TXH_LEN):
            return off
    return None


def dd_lines(chan, slot, bufaddr, flags, raw, buf, hexdump):
    be = flags & DD_F_BE
    w = struct.unpack(">4I" if be else "<4I", raw)
    bc = w[1] & 0x7fff
    fl = [n for b, n in ((DD_F_BE, "be"), (DD_F_NOBUF, "nobuf"),
                         (DD_F_GUESS, "guess")) if flags & b]
    out = [f"ch={chan} dd={slot:#010x} buf={bufaddr:#010x} ctl0={w[0]:#010x} "
           f"ctl1={w[1]:#010x} bytes={bc} addrhi={w[3]:#x}"
           + (f" [{','.join(fl)}]" if fl else "")]
    if hexdump or not buf:
        for i in range(0, len(buf), 16):
            out.append(f"  {i:03x}: {buf[i:i + 16].hex(' ')}")
    off = txh_offset(buf, bc) if buf else None
    if off is None:
        if buf and not hexdump:
            out.append("  layout non riconosciuto:")
            for i in range(0, len(buf), 16):
                out.append(f"  {i:03x}: {buf[i:i + 16].hex(' ')}")
        return out

    t = buf[off:]
    out.append(f"  txh@{off}{' toe=' + buf[:4].hex(' ') if off else ''}: "
               f"tso={u16(t, 0):#06x} macctl={u16(t, 2):#06x}/{u16(t, 4):#06x} "
               f"chanspec={u16(t, 6):#06x} ({chanspec(u16(t, 6))}) "
               f"iv_off={t[8]} pktcache={t[9]} frame_len={u16(t, 10)} "
               f"frameid={u16(t, 12):#06x} seq={u16(t, 14):#06x} "
               f"tstamp={u16(t, 16):#06x} txstatus={u16(t, 18):#06x}")
    for i in range(4):
        r = 20 + 20 * i
        if len(t) < r + 20:
            break
        phy = [u16(t, r + 2 * k) for k in range(3)]
        plcp = t[r + 6:r + 12]
        rts = u16(t, r + 16)
        if not any(t[r:r + 20]):
            continue
        ft = phy[0] & 3
        out.append(f"  rate{i}: phy={phy[0]:#06x} {phy[1]:#06x} {phy[2]:#06x} "
                   f"ft={FT[ft]} cores={(phy[0] >> 6) & 0xf:#x} "
                   f"plcp={plcp.hex(' ')} ({plcp_text(ft, plcp)}) "
                   f"fbw={u16(t, r + 12):#06x} rate={u16(t, r + 14):#06x} "
                   f"rts={rts:#06x} bfm={u16(t, r + 18):#06x}")
        if rts & 0x0020:
            break
    f = buf[off + D11AC_TXH_LEN:]
    if len(f) >= 10:
        fc = u16(f, 0)
        out.append(f"  802.11: fc={fc:#06x} type={(fc >> 2) & 3} "
                   f"subtype={(fc >> 4) & 0xf:#x} dur={u16(f, 2):#06x} "
                   f"a1={f[4:10].hex(':')}")
    return out


def unmark(addr, val, aux):
    """i 12 byte impacchettati in tre u32, fino al primo NUL"""
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
    dd = None       # descrittore in attesa dei suoi DMA.DATA

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
