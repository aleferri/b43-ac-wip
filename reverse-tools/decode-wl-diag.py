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
# CS.SHM (40): il chanspec scritto in shared memory, addr = chanspec. E' il
# confine di ciclo affidabile: CHANSPEC (26) viene dalla generica
# wlc_phy_chanspec_set, che sull'AC-PHY non e' sul percorso e quando scatta porta
# il chanspec CORRENTE, cioe' in ritardo di un ciclo.
# CHANSPEC (26): cambio canale, addr = chanspec. Il decoder lo espande in
# canale/banda/larghezza col formato 802.11ac standard (chan=bit 0-7,
# bw=0x3800, band=0xc000) -- assunzione, non verificata su cattura. Serve a
# tagliare a posteriori una run che copre piu' canali.
# MARK (39): etichetta iniettata dallo spazio utente con
#     echo "ch36 bw20" > /proc/wl_diag
# 12 caratteri impacchettati big-endian nei tre campi u32 (addr, val, aux). Non
# viene dal driver: e' un confine messo nella traccia da chi cattura, e sostuisce
# il taglio a posteriori sui salti temporali. wl_diag ne emette due da se',
# "mod COMING" e "mod GOING", ai bordi di ogni caricamento del bersaglio.
# OBJ.RD (24) / OBJ.WR (25): object memory del MAC. addr=offset in byte,
# sel=selettore dello spazio (SHM, SCR, IHR...) e va STAMPATO: senza di lui la
# traccia non distingue i tre spazi, e il record binario lo porta comunque. La RD ha il valore nel RETVAL
# come PHY.RD. Serve per il campione di potenza di rumore che la crs_min_pwr cal
# legge: non passa da un registro PHY, quindi senza questo hook non compare in
# nessuna cattura.
# ATTENZIONE all'origine, che cambia con la build: su 7.14.89 sono
# read/write_objmem16 e aux porta il selettore vero; su 7.14.43 quegli accessor
# non esistono e i record vengono da wlc_bmac_read/write_shm, che coprono il
# SOLO spazio SHM -- la' aux e' sempre 0 e gli accessi a SCR e IHR non compaiono.
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
    41: "OBJ.BULKR", 42: "OBJ.BULKW",
    43: "AMT.WR",    44: "RCMTA.WR",  45: "ADDRM.SET",
    46: "PHY.WARR",  47: "PHY.RDW",   48: "PHY.WRW",
    49: "IHR.WR",    50: "OBJ.SET",
    26: "CHANSPEC",
    27: "TPL.PTRW",  28: "TPL.DATW",
    29: "TPL.PTRR",  30: "TPL.DATR",  31: "TPL.RAMW",
    32: "OTP.INIT",  33: "OTP.RDW",   34: "OTP.RDR",
    35: "MAC.BW",    36: "SROMCTL.RD", 37: "SROMCTL.WR",
    38: "CAL.INIT",
    39: "MARK",
    40: "CS.SHM",
    255: "DROP",
}

# Le read loggano solo occorrenza+indirizzo: il valore NON e' catturato (hook
# all'ingresso, o foglia con return non agganciabile). Va emesso UNDEFINED, MAI
# 0x0000 inventato -- altrimenti si riparte col problema di distinguere zeri
# veri da zeri finti.
CHANSPEC = 26
CS_SHM   = 40
OBJ      = {24, 25}
# Object memory in blocco: il record porta offset (a1) e lunghezza (a3). Il
# valore NON c'e' -- sta in un buffer del chiamante -- e non e' una read con
# RETVAL: non va in READS, o compare.py lo vedrebbe come val=UNDEFINED, cioe'
# un wildcard che combacia con tutto. Il SELETTORE e' il 5o argomento e arriva
# in un record ARGX come a5: dopo `trace_filter.py --retvals` la riga lo
# porta in coda.
OBJ_BULK = {41, 42}
# Address match: il record porta il solo indice (a1). AMT.WR porta anche a3.
ADDRMATCH = {43, 44, 45}
# PHY.WARR: scrittura PHY in blocco. NON porta un indirizzo -- l'argomento e'
# un puntatore all'array -- ma il conteggio delle voci. E' un marcatore: le
# singole scritture arrivano dagli hook a 16 bit che la funzione chiama.
PHY_WARR = 46
# Registro fisso, nessun argomento indirizzo: stamparne uno sarebbe uno zero
# inventato. PHY.RDW e' anche una read, quindi il valore arriva dal RETVAL.
NO_ADDR  = {47, 48}
# OBJ.SET: memset su shared memory, offset + valore + lunghezza.
OBJ_SET  = 50
MARK     = 39
READS    = {1, 4, 18, 24, 29, 30, 32, 33, 34, 36, 47}                 # PHY.RD, RAD.RD, MAC.MHF.RD, OBJ.RD
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


def unmark(addr, val, aux):
    """i 12 byte impacchettati in tre u32, fino al primo NUL"""
    b = b"".join(w.to_bytes(4, "big") for w in (addr, val, aux))
    return b.split(b"\x00")[0].decode("ascii", "replace")


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
            if op == MARK:
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"{unmark(addr, val, aux)!r}")
            elif op in (CHANSPEC, CS_SHM):
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
            elif op == PHY_WARR:                   # conteggio voci, non un addr
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} n={val}")
            elif op in NO_ADDR:                    # registro fisso, niente addr
                valstr = "UNDEFINED" if op in READS else h(val, False)
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} val={valstr}")
            elif op == OBJ_SET:                    # off + val + len
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} "
                      f"addr={h(addr, False)} val={h(val, False)} len={aux}")
            elif op in OBJ_BULK:                   # offset + lunghezza; sel via ARGX
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} "
                      f"addr={h(addr, False)} len={aux}")
            elif op in ADDRMATCH:                  # indice; nessun valore nel record
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} "
                      f"idx={h(addr, False)}"
                      + (f" a3={h(aux, True)}" if aux else ""))
            elif op in OBJ:                        # object memory: serve il selettore
                valstr = "UNDEFINED" if op in READS else h(val, wide)
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={valstr} sel={h(aux, False)}")
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
