#!/usr/bin/env python3
# Decoder dei record wl_diag (MIPS big-endian, 28 byte/record).
#
# Uso tipico (il device fa: cat /dev/wl_diag | nc -u host 5555):
#     nc -u -l -p 5555 | ./decode.py
# oppure da file:
#     ./decode.py < dump.bin
#
# Framing: e' uno stream di byte, si legge a blocchi di 28. Ogni record e' ben
# sotto l'MTU; con 'cat | nc' non c'e' rischio di spezzare a meta' record.
#
# DEVE restare allineato agli op-code di wl_diag.c (enum OP_*). Aggiornato per
# gli hook GPIO ChipCommon (op 10/11/12): per quelli 'addr' NON e' usato
# (addr_src=0), i campi significativi sono val=a2 e mask=aux=a1. Idem per il
# controllo verso il MAC: MAC.MCTRL (op 16) e' un RMW su reg fisso (addr
# assente, val=a2/mask=a1, 32 bit); MAC.MHF (17) porta idx=addr, val, mask;
# MAC.MHF.RD (18) e' una read (val UNDEFINED). PHY.AND (19) / PHY.OR (20):
# reg-op a un operando (addr,val); val e' la maschera-AND risp. il valore-OR,
# resi con la maschera effettiva derivata (clr ~val / set val).
import sys, struct

REC = struct.Struct(">QIIIIBBH")   # ts_ns, seq, addr, val, aux, op, cpu, _pad
SZ = REC.size                       # 28

OPS = {
    1:  "PHY.RD",   2:  "PHY.WR",   3:  "PHY.MOD",
    4:  "RAD.RD",   5:  "RAD.WR",   6:  "RAD.MOD",
    7:  "PMU.CC",   8:  "PMU.RC",   9:  "PMU.PLL",
    10: "GPIO.CTL", 11: "GPIO.OUT", 12: "GPIO.OE",
    13: "TBL.RD",   14: "TBL.WR",   15: "DELAY",
    16: "MAC.MCTRL",17: "MAC.MHF",  18: "MAC.MHF.RD",
    19: "PHY.AND",  20: "PHY.OR",
    255: "DROP",
}

# Le read loggano solo occorrenza+indirizzo: il valore NON e' catturato (hook
# all'ingresso, o foglia con return non agganciabile). Va emesso UNDEFINED, MAI
# 0x0000 inventato -- altrimenti si riparte col problema di distinguere zeri
# veri da zeri finti.
READS    = {1, 4, 18}                     # PHY.RD, RAD.RD, MAC.MHF.RD
HAS_MASK = {3, 6, 7, 8, 9, 10, 11, 12, 17} # aux e' una mask (RMW, GPIO, MHF)
GPIO     = {10, 11, 12}                   # niente addr; val=a2, mask=aux=a1
MCTRL    = {16}                            # MACCONTROL RMW: reg fisso, niente addr; val=a2, mask=aux=a1
WIDE     = {7, 8, 9, 10, 11, 12, 16}      # PMU/GPIO/MACCONTROL: val/mask a 32 bit
TABLE    = {13, 14}                       # id=addr(a1), len=val(a2), off=aux(a3)
DELAY    = 15                             # niente addr; usec=val(a1)
PHY_AND  = 19                             # addr + val=maschera-AND (bit tenuti)
PHY_OR   = 20                             # addr + val=valore-OR (bit settati)


def h(v, wide):
    return f"0x{v:08x}" if wide else f"0x{v:04x}"


def main():
    f = sys.stdin.buffer
    buf = b""
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
            wide = op in WIDE
            if op == 255:
                print(f"{t:14.6f}  cpu{cpu}  ** DROP **  persi={aux}")
            elif op == DELAY:                      # niente addr; durata in usec
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} usec={val}")
            elif op in TABLE:                      # accesso tabella: id/off/len
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"id={h(addr, False)} off={h(aux, False)} len={val}")
            elif op in GPIO or op in MCTRL:        # addr assente (reg fisso)
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"val={h(val, wide)} mask={h(aux, wide)}")
            elif op in HAS_MASK:                   # addr + val + mask
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={h(val, wide)} mask={h(aux, wide)}")
            elif op == PHY_AND:                    # read & val ; bit azzerati = ~val
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={h(val, False)} "
                      f"(clr {h((~val) & 0xffff, False)})")
            elif op == PHY_OR:                     # read | val ; bit settati = val
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={h(val, False)} "
                      f"(set {h(val, False)})")
            else:                                  # read/write semplice
                valstr = "UNDEFINED" if op in READS else h(val, wide)
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={valstr}")
            sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except (BrokenPipeError, KeyboardInterrupt):
        pass
