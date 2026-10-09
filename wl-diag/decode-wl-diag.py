#!/usr/bin/env python3
# Decoder of the wl_diag records (MIPS big-endian, 28 bytes per record).
#
# Typical use (the device does: cat /proc/wl_diag | nc <HOST> 5555, over TCP):
#     ncat -l 5555 | python3 decode-wl-diag.py
# or from a file:
#     python3 decode-wl-diag.py < dump.bin
#
# Framing: it is a byte stream, read in blocks of 28; a record split between
# two reads stays in the buffer until the next one.
#
# It MUST stay aligned with the op codes of 3-4-11/wl_diag.c and
# 2-6-30/wl_diag.c (enum OP_*). For the ChipCommon GPIO hooks (ops 10/11/12)
# 'addr' is NOT used (addr_src=0): the meaningful fields are val=a2 and
# mask=aux=a1. Likewise for the control towards the MAC: MAC.MCTRL (op 16) is
# an RMW on a fixed register (no addr, val=a2/mask=a1, 32 bits); MAC.MHF (17)
# carries idx=addr, val, mask; MAC.MHF.RD (18) is a read (val UNDEFINED).
# PHY.AND (19) / PHY.OR (20): single-operand register ops (addr,val); val is
# the AND mask or the OR value, printed with the effective mask derived from
# it (clr ~val / set val).
# MAC.BW (35): bandwidth at the MAC level, val = the argument.
# SROMCTL.RD/WR (36,37): the SROM control register, from the attach path.
# OTP.* (32-34): OTP reads from the generic layer. addr = word number or
# region; the value goes into a pointer, not the return, so what matters here
# is WHEN and WHICH, not the content (which is in the SROM dumps).
# TPL.* (27-31): template RAM, where the PHY loads the tone waveforms. PTRR and
# DATR have the value in the RETVAL. On 6.30 only TPL.RAMW exists.
# CS.SHM (40): the chanspec written to shared memory, addr = chanspec. It is
# the reliable cycle boundary: CHANSPEC (26) comes from the generic
# wlc_phy_chanspec_set, which on the AC-PHY is off the path and, when it
# fires, carries the CURRENT chanspec, that is one cycle late.
# CHANSPEC (26): channel change, addr = chanspec. The decoder expands it, like
# CS.SHM, into channel/band/width with the 802.11ac layout (chan=bits 0-7,
# bw=0x3800, band=0xc000); the channel is the CENTRE one, not the primary (see
# B43_PHY_AC_CHANSPEC_* in b43/phy_ac.h). It is for cutting a run that covers
# several channels afterwards.
# MARK (39): a label injected from userspace with
#     echo "ch36 bw20" > /proc/wl_diag
# 12 characters packed big-endian into the three u32 fields (addr, val, aux).
# It does not come from the driver: it is a boundary put into the trace by
# whoever captures, and it replaces cutting on time gaps afterwards. wl_diag
# emits two by itself, "mod COMING" and "mod GOING", at the edges of every
# load of the target.
# OBJ.RD (24) / OBJ.WR (25): the MAC's object memory. addr=offset in bytes,
# sel=the space selector (SHM, SCR, IHR...), which has to be PRINTED: without
# it the trace does not tell the three spaces apart, and the binary record
# carries it anyway. The RD has the value in the RETVAL like PHY.RD. It is
# for the noise power sample the crs_min_pwr cal reads: it does not come from
# a PHY register, so without this hook it is in no capture.
# MIND the origin, which changes with the build: on 7.14.89 they are
# read/write_objmem16 and aux carries the real selector; on 7.14.43 those
# accessors do not exist and the records come from wlc_bmac_read/write_shm,
# which cover the SHM space ONLY -- there aux is always 0 and the accesses to
# SCR and IHR do not appear.
# TX.PKT (55) + TX.DATA (56): a frame posted to a TX ring, at the entry of
# the TX post. TX.PKT carries the length posted (addr), the bytes that
# follow (val) and in aux 1 for a raw buffer, 2 for a tagged nbuff pointer
# that was not read; the TX.DATA records, right
# after it, carry the bytes, twelve per record packed like MARK. The d11 AC TX
# header is read by ../reverse-tools/d11ac_txh.py, the same one
# decode-wl-mmio.py uses; a layout that does not add up is printed in hex.
# TPL.RAMW (31) + TPL.DATA (57): with tpldump set, aux of a TPL.RAMW is the
# number of bytes of the write that follow in TPL.DATA records, packed the
# same way. The TPL.RAMW line stays as it is; the content follows it,
# indented, read by ../reverse-tools/d11_template.py when it holds a beacon
# or a probe response, in hex otherwise.
# RX.PKT (58) + RX.DATA (59): a received frame at the entry of wlc_recv, the
# same shape as TX.PKT. The RX header is read by
# ../reverse-tools/d11ac_rxh.py as b43_rx() reads it.
import os, sys, struct

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "reverse-tools"))
import d11ac_txh
import d11_template
import d11ac_rxh

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
    51: "PHY.FGC",
    52: "IOCTL",      53: "IOVAR.NAME", 54: "IOVAR.SET",
    55: "TX.PKT",     56: "TX.DATA",    57: "TPL.DATA",
    58: "RX.PKT",     59: "RX.DATA",
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

# The record of a read carries only the occurrence and the address: the hook
# is at the entry, and the value, when the hook has retcap, arrives in the
# RETVAL that follows. The read's line says UNDEFINED, NEVER an invented
# 0x0000 -- otherwise real zeros and fake ones cannot be told apart again.
CHANSPEC = 26
CS_SHM   = 40
OBJ      = {24, 25}
# Bulk object memory: the record carries offset (a1) and length (a3). The
# value is NOT there -- it is in a buffer of the caller -- and it is not a
# read with a RETVAL: it does not go in READS, or compare.py would see it as
# val=UNDEFINED, a wildcard that matches anything. The SELECTOR is the 5th
# argument and arrives in an ARGX record as a5: after
# `trace_filter.py --retvals` the line carries it at the end.
OBJ_BULK = {41, 42}
# Address match: the record carries the index (a1) only. AMT.WR also a3.
ADDRMATCH = {43, 44, 45}
# PHY.WARR: bulk PHY write. It carries NO address -- the argument is a
# pointer to the array -- but the count of entries. It is a marker: the
# individual writes come from the 16-bit hooks the function calls.
PHY_WARR = 46
# Fixed register, no address argument: printing one would be an invented
# zero. PHY.RDW is also a read, so the value comes from the RETVAL.
NO_ADDR  = {47, 48, 51}		# fixed register or no address: PHY.RDW/WRW, PHY.FGC
# OBJ.SET: memset over shared memory, offset + value + length.
OBJ_SET  = 50
MARK     = 39
READS    = {1, 4, 18, 24, 29, 30, 32, 33, 34, 36, 47}                 # read: the line carries no value
HAS_MASK = {3, 6, 7, 8, 9, 10, 11, 12, 17} # aux is a mask (RMW, GPIO, MHF)
GPIO     = {10, 11, 12}                   # no addr; val=a2, mask=aux=a1
MCTRL    = {16}                            # MACCONTROL RMW: fixed reg, no addr; val=a2, mask=aux=a1
WIDE     = {7, 8, 9, 10, 11, 12, 16}      # PMU/GPIO/MACCONTROL: 32-bit val/mask
TABLE    = {13, 14}                       # id=addr(a1), len=val(a2), off=aux(a3)
DELAY    = 15                             # no addr; usec=val(a1)
PHY_AND  = 19                             # addr + val=AND mask (bits kept)
PHY_OR   = 20                             # addr + val=OR value (bits set)
COREREG  = 21                             # core reg: off=addr(a2), core=aux(a1); val in the ARGX
# Follow-on records (tied to the main one through 'for=#<parent_seq>'):
#   ARGX  (22): extra stack arguments -> a5=addr, a6=val, parent=aux
#   RETVAL(23): value returned by a read/rmw -> parent=addr, val=val
ARGX     = 22
RETVAL   = 23


def h(v, wide):
    return f"0x{v:08x}" if wide else f"0x{v:04x}"


_BW = {0x1000: "20", 0x1800: "40", 0x2000: "80", 0x2800: "160", 0x3000: "80+80"}


def chanspec(cs):
    """802.11ac layout: chan bits 0-7, bw 0x3800, band 0xc000"""
    return (f"ch={cs & 0xff} bw={_BW.get(cs & 0x3800, '?')} "
            f"band={'5g' if (cs & 0xc000) == 0xc000 else '2g'} raw=0x{cs:04x}")


def unmark(addr, val, aux):
    """the 12 bytes packed into three u32, up to the first NUL"""
    b = b"".join(w.to_bytes(4, "big") for w in (addr, val, aux))
    return b.split(b"\x00")[0].decode("ascii", "replace")


# The userspace command, from the hook on wlc_ioctl. IOVAR.NAME carries the
# name of a WLC_SET_VAR in twelve-byte pieces, packed like MARK, and comes
# before its IOVAR.SET: the pieces are gathered per CPU, because a record of
# another CPU can fall between two records of the same call, and the line goes
# out with the SET. The value is the first u32 after the NUL, in the driver's
# byte order.
IOCTL, IOVAR_NAME, IOVAR_SET = 52, 53, 54
IOCTL_NAMES = {2: "UP", 3: "DOWN", 26: "SET_SSID"}
TX_PKT, TX_DATA = 55, 56
TPL_RAMW, TPL_DATA = 31, 57
RX_PKT, RX_DATA = 58, 59
DATA_OF = {TX_DATA: TX_PKT, TPL_DATA: TPL_RAMW, RX_DATA: RX_PKT}


def print_group(g):
    """A TX.PKT, RX.PKT or TPL.RAMW with its bytes; cut short if another
    record arrived before the end of the group, that is if some were lost."""
    data = g["data"][:g["need"]]
    if g["op"] in (TX_PKT, RX_PKT):
        src = ("buf" if g["aux"] & 1 else
               "nbuff-tagged, not read" if g["aux"] & 2 else "pkt")
        name = "TX.PKT" if g["op"] == TX_PKT else "RX.PKT"
        first = f"{name:<8} {src} len={g['addr']} bytes={g['need']}"
        describe = (d11ac_txh.describe if g["op"] == TX_PKT
                    else d11ac_rxh.describe)
        dump = d11ac_txh.hexdump
        args = (data, g["addr"])
    else:
        first = (f"{'TPL.RAMW':<8} addr={h(g['addr'], False)} "
                 f"val={h(g['val'], False)}")
        describe, dump = d11_template.describe, d11_template.hexdump
        args = (data,)
    lines = [first]
    if len(data) < g["need"]:
        lines[0] += f" troncato a {len(data)}"
    elif data:
        txt = describe(*args)
        if txt is None:
            lines.append("layout non riconosciuto:")
            lines += dump(data)
        else:
            lines += txt
    print(f"{g['t']:14.6f} #{g['seq']:<8} cpu{g['cpu']} {lines[0]}")
    for line in lines[1:]:
        print(" " * 17 + line)


def main():
    f = sys.stdin.buffer
    buf = b""
    iovar_name = {}
    group = None        # a TX.PKT or TPL.RAMW waiting for its data records
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
            if op in DATA_OF:
                if group is None or group["op"] != DATA_OF[op]:
                    print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                          "orfano (record di testa perso)")
                    continue
                group["data"] += b"".join(x.to_bytes(4, "big")
                                          for x in (addr, val, aux))
                if len(group["data"]) < group["need"]:
                    continue
                print_group(group)
                group = None
                sys.stdout.flush()
                continue
            if group is not None:
                print_group(group)
                group = None
            if op in (TX_PKT, RX_PKT) or (op == TPL_RAMW and aux):
                group = {"op": op, "t": t, "seq": seq, "cpu": cpu,
                         "addr": addr, "val": val, "aux": aux,
                         "need": aux if op == TPL_RAMW else val, "data": b""}
                if not group["need"]:
                    print_group(group)
                    group = None
                continue
            if op == IOVAR_NAME:
                iovar_name[cpu] = iovar_name.get(cpu, "") + unmark(addr, val, aux)
                continue
            if op == IOVAR_SET:
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} "
                      f"name={iovar_name.pop(cpu, '')} val={h(addr, True)} len={val}")
            elif op == IOCTL:
                cmd = IOCTL_NAMES.get(addr, str(addr))
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} "
                      f"cmd={cmd} arg={h(val, True)} len={aux}")
            elif op == MARK:
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"{unmark(addr, val, aux)!r}")
            elif op in (CHANSPEC, CS_SHM):
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} {chanspec(addr)}")
            elif op == 255:
                print(f"{t:14.6f}  cpu{cpu}  ** DROP **  persi={aux}")
            elif op == DELAY:                      # no addr; duration in usec
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} usec={val}")
            elif op in TABLE:                      # table access: id/off/len
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"id={h(addr, False)} off={h(aux, False)} len={val}")
            elif op in GPIO or op in MCTRL:        # no addr (fixed reg)
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"val={h(val, wide)} mask={h(aux, wide)}")
            elif op in HAS_MASK:                   # addr + val + mask
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={h(val, wide)} mask={h(aux, wide)}")
            elif op == ARGX:                       # stack argument, follow-on
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"for=#{aux} a5={h(addr, True)} a6={h(val, True)}")
            elif op == RETVAL:                     # return value, follow-on
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"for=#{addr} val={h(val, True)}")
            elif op == COREREG:                    # core reg: core+off; val in the ARGX, return in the RETVAL
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"core={h(aux, False)} off={h(addr, False)} val=UNDEFINED")
            elif op == PHY_WARR:                   # entry count, not an addr
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} n={val}")
            elif op in NO_ADDR:                    # fixed register, no addr
                valstr = "UNDEFINED" if op in READS else h(val, False)
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} val={valstr}")
            elif op == OBJ_SET:                    # off + val + len
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} "
                      f"addr={h(addr, False)} val={h(val, False)} len={aux}")
            elif op in OBJ_BULK:                   # offset + length; sel through ARGX
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} "
                      f"addr={h(addr, False)} len={aux}")
            elif op in ADDRMATCH:                  # index; no value in the record
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<9} "
                      f"idx={h(addr, False)}"
                      + (f" a3={h(aux, True)}" if aux else ""))
            elif op in OBJ:                        # object memory: the selector is needed
                valstr = "UNDEFINED" if op in READS else h(val, wide)
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={valstr} sel={h(aux, False)}")
            elif op == PHY_AND:                    # read & val ; bits cleared = ~val
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={h(val, False)} "
                      f"(clr {h((~val) & 0xffff, False)})")
            elif op == PHY_OR:                     # read | val ; bits set = val
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={h(val, False)} "
                      f"(set {h(val, False)})")
            else:                                  # plain read/write
                valstr = "UNDEFINED" if op in READS else h(val, wide)
                print(f"{t:14.6f} #{seq:<8} cpu{cpu} {name:<8} "
                      f"addr={h(addr, False)} val={valstr}")
            sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except (BrokenPipeError, KeyboardInterrupt):
        pass
