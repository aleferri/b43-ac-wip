#!/usr/bin/env python3
"""The d11 AC TX header as the stock driver hands it to DMA.

Both tracers record it: wl-mmio-trap from memory at a TX index write,
wl-diag at the entry of hnddma's TX post. One reading of the header, so the
two captures print the same fields the same way.

Layout, after the optional 4-byte TX offload header: 20 bytes of packet info
(TSO, MAC control low/high, chanspec, IV offset, packet cache length,
frame_len, frame ID, sequence, timestamp, TX status), four 20-byte rate
blocks (three PHY TX control words, 6 bytes of PLCP, fallback bandwidth,
rate, RTS/CTS control, beamforming), 24 bytes of cache info: 124 bytes, the
same as `struct b43_txhdr_ac` without its `toe`. The 802.11 frame follows.
"""

TXH_LEN = 124
FT = {0: "CCK", 1: "OFDM", 2: "HT", 3: "VHT"}
OFDM_RATE = {0xb: 6, 0xf: 9, 0xa: 12, 0xe: 18, 0x9: 24, 0xd: 36, 0x8: 48,
             0xc: 54}
CHSPEC_BW = {0x0800: "10", 0x1000: "20", 0x1800: "40", 0x2000: "80",
             0x2800: "160", 0x3000: "80+80"}
LAST_RATE = 0x0020          # rts_cts_ctl of the last rate block


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def chanspec(v):
    bw = CHSPEC_BW.get(v & 0x3800, f"?{v & 0x3800:#x}")
    band = "5g" if v & 0xc000 == 0xc000 else "2g" if v & 0xc000 == 0 else "?"
    return f"ch{v & 0xff} bw{bw} sb{(v >> 8) & 7} {band}"


def plcp_text(ft, p):
    """The rate block's PLCP, read by its frame type, the fields as
    b43_rx_rate_ac() reads them on receive."""
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
    return (f"vht-sig-a {bw}MHz mcs{(a2 >> 4) & 0xf} "
            f"nsts={((a1 >> 10) & 7) + 1} stbc={(a1 >> 3) & 1} "
            f"gid={(a1 >> 4) & 0x3f} sgi={a2 & 1} ldpc={(a2 >> 2) & 1} "
            f"a1={a1:#08x} a2={a2:#08x}")


def offset(buf, total):
    """Where the header starts in buf, or None. total is the byte count of
    the buffer as the driver posted it. frame_len (+10 in the header) counts
    what goes on air, FCS and whatever the hardware adds for encryption
    included, and the frame may go on in further buffers: so the buffer
    holds the header and at most frame_len - 4 bytes of frame, and the
    frame control right after the header has to be one."""
    for off in (4, 0):
        if len(buf) < off + 12:
            continue
        fl = u16(buf, off + 10)
        if not off + TXH_LEN <= total <= off + TXH_LEN + fl - 4:
            continue
        if len(buf) >= off + TXH_LEN + 2:
            fc = u16(buf, off + TXH_LEN)
            if fc & 3 or (fc >> 2) & 3 == 3:
                continue
        return off
    return None


def describe(buf, total):
    """Lines describing the header in buf, or None if it is not one."""
    off = offset(buf, total)
    if off is None:
        return None

    t = buf[off:]
    out = [f"txh@{off}{' toe=' + buf[:4].hex(' ') if off else ''}: "
           f"tso={u16(t, 0):#06x} macctl={u16(t, 2):#06x}/{u16(t, 4):#06x} "
           f"chanspec={u16(t, 6):#06x} ({chanspec(u16(t, 6))}) "
           f"iv_off={t[8]} pktcache={t[9]} frame_len={u16(t, 10)} "
           f"frameid={u16(t, 12):#06x} seq={u16(t, 14):#06x} "
           f"tstamp={u16(t, 16):#06x} txstatus={u16(t, 18):#06x}"]
    for i in range(4):
        r = 20 + 20 * i
        if len(t) < r + 20:
            break
        if not any(t[r:r + 20]):
            continue
        phy = [u16(t, r + 2 * k) for k in range(3)]
        plcp = t[r + 6:r + 12]
        rts = u16(t, r + 16)
        ft = phy[0] & 3
        out.append(f"rate{i}: phy={phy[0]:#06x} {phy[1]:#06x} {phy[2]:#06x} "
                   f"ft={FT[ft]} cores={(phy[0] >> 6) & 0xf:#x} "
                   f"plcp={plcp.hex(' ')} ({plcp_text(ft, plcp)}) "
                   f"fbw={u16(t, r + 12):#06x} rate={u16(t, r + 14):#06x} "
                   f"rts={rts:#06x} bfm={u16(t, r + 18):#06x}")
        if rts & LAST_RATE:
            break
    f = buf[off + TXH_LEN:]
    if len(f) >= 10:
        fc = u16(f, 0)
        out.append(f"802.11: fc={fc:#06x} type={(fc >> 2) & 3} "
                   f"subtype={(fc >> 4) & 0xf:#x} dur={u16(f, 2):#06x} "
                   f"a1={f[4:10].hex(':')}")
    return out


def hexdump(buf):
    return [f"{i:03x}: {buf[i:i + 16].hex(' ')}" for i in range(0, len(buf), 16)]
