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
# MAC.BW (35): larghezza al livello MAC, val = il parametro. Il port non la fa.
# SROMCTL.RD/WR (36,37): registro di controllo SROM, dal percorso di attach.
# OTP.* (32-34): letture OTP dal livello generico. addr = numero di word o
# regione; il valore va in un puntatore, non nel ritorno, quindi qui interessa
# QUANDO e QUALE, non il contenuto (che sta nei dump SROM).
# TPL.* (27-31): template RAM, dove il PHY carica le forme d'onda dei toni.
# Le PTRR/DATR hanno il valore nel RETVAL. Su 6.30 esiste solo TPL.RAMW.
# CHANSPEC (26): cambio canale, addr = chanspec. Il decoder lo espande in
# canale/banda/larghezza col formato 802.11ac standard (chan=bit 0-7,
# bw=0x3800, band=0xc000) -- assunzione, non verificata su cattura. Serve a
# tagliare a posteriori una run che copre piu' canali.
# OBJ.RD (24) / OBJ.WR (25): object memory del MAC via read/write_objmem16.
# addr=offset in byte, aux=selettore dello spazio (SHM, SCR, IHR...). La RD ha
# il valore nel RETVAL come PHY.RD. Serve per il campione di potenza di rumore
# che la crs_min_pwr cal legge: non passa da un registro PHY, quindi senza
# questo hook non compare in nessuna cattura.
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
    21: "SI.COREREG",
    22: "ARGX",     23: "RETVAL",
    24: "OBJ.RD",   25: "OBJ.WR",
    26: "CHANSPEC",
    27: "TPL.PTRW",  28: "TPL.DATW",
    29: "TPL.PTRR",  30: "TPL.DATR",  31: "TPL.RAMW",
    32: "OTP.INIT",  33: "OTP.RDW",   34: "OTP.RDR",
    35: "MAC.BW",    36: "SROMCTL.RD", 37: "SROMCTL.WR",
    38: "CAL.INIT",
    255: "DROP",
}

# Le read loggano solo occorrenza+indirizzo: il valore NON e' catturato (hook
# all'ingresso, o foglia con return non agganciabile). Va emesso UNDEFINED, MAI
# 0x0000 inventato -- altrimenti si riparte col problema di distinguere zeri
# veri da zeri finti.
CHANSPEC = 26
READS    = {1, 4, 18, 24, 29, 30, 32, 33, 34, 36}                 # PHY.RD, RAD.RD, MAC.MHF.RD, OBJ.RD
HAS_MASK = {3, 6, 7, 8, 9, 10, 11, 12, 17} # aux e' una mask (RMW, GPIO, MHF)
GPIO     = {10, 11, 12}                   # niente addr; val=a2, mask=aux=a1
MCTRL    = {16}                            # MACCONTROL RMW: reg fisso, niente addr; val=a2, mask=aux=a1
WIDE     = {7, 8, 9, 10, 11, 12, 16}      # PMU/GPIO/MACCONTROL: val/mask a 32 bit
TABLE    = {13, 14}                       # id=addr(a1), len=val(a2), off=aux(a3)
DELAY    = 15                             # niente addr; usec=val(a1)
PHY_AND  = 19                             # addr + val=maschera-AND (bit tenuti)
PHY_OR   = 20                             # addr + val=valore-OR (bit settati)
COREREG  = 21                             # core reg: off=addr(a2), core=aux(a1); val non catturato
# Record di continuazione (correlati al principale via 'for=#<parent_seq>'):
#   ARGX  (22): arg su stack extra -> a5=addr, a6=val, parent=aux
#   RETVAL(23): valore restituito da una read/rmw -> parent=addr, val=val
ARGX     = 22
RETVAL   = 23


def h(v, wide):
    return f"0x{v:08x}" if wide else f"0x{v:04x}"


_BW = {0x1000: "20", 0x1800: "40", 0x2000: "80", 0x2800: "160", 0x3000: "80+80"}


def chanspec(cs):
    """formato 802.11ac: chan bit 0-7, bw 0x3800, band 0xc000"""
    return (f"ch={cs & 0xff} bw={_BW.get(cs & 0x3800, '?')} "
            f"band={'5g' if (cs & 0xc000) == 0xc000 else '2g'} raw=0x{cs:04x}")


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
            if op == CHANSPEC:
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} {chanspec(addr)}")
            elif op == 255:
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
            elif op == ARGX:                       # arg su stack, continuazione
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"for=#{aux} a5={h(addr, True)} a6={h(val, True)}")
            elif op == RETVAL:                     # valore restituito, continuazione
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"for=#{addr} val={h(val, True)}")
            elif op == COREREG:                    # core reg: core+off, valore non catturato all'ingresso
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"core={h(aux, False)} off={h(addr, False)} val=UNDEFINED")
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
