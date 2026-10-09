#!/usr/bin/env python3
"""A beacon or probe response template as written to template RAM.

What sits in front of the 802.11 frame (PLCP, and whatever header the
microcode expects) is not assumed: the frame is found by its frame control
and by the SSID element that opens the body of both frames, and the bytes
before it are printed raw. The frame header, the fixed fields and the
elements follow, with SSID, DS parameter, TIM, HT and VHT operation decoded
and the rest in hex.
"""

FC_NAMES = {0x0080: "beacon", 0x0050: "probe-resp"}
HDR_LEN = 24
FIXED_LEN = 12          # timestamp, beacon interval, capability
EID = {0: "SSID", 1: "rates", 3: "DS", 5: "TIM", 7: "country",
       11: "BSS load", 42: "ERP", 45: "HT cap", 48: "RSN", 50: "ext rates",
       61: "HT op", 127: "ext cap", 191: "VHT cap", 192: "VHT op",
       195: "VHT tx pwr", 221: "vendor"}


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def frame_offset(buf):
    """Offset of the frame in buf, or None."""
    for off in range(0, min(len(buf) - HDR_LEN - FIXED_LEN - 2, 64)):
        if u16(buf, off) not in FC_NAMES:
            continue
        body = off + HDR_LEN + FIXED_LEN
        if buf[body] == 0 and buf[body + 1] <= 32:
            return off
    return None


def element(eid, data):
    if eid == 0:
        return repr(data.decode("ascii", "replace"))
    if eid == 3 and len(data) >= 1:
        return f"ch{data[0]}"
    if eid == 5 and len(data) >= 3:
        return (f"dtim_count={data[0]} dtim_period={data[1]} "
                f"bitmap_ctl={data[2]:#04x} pvb={data[3:].hex(' ')}")
    if eid == 61 and len(data) >= 2:
        return (f"primary=ch{data[0]} sco={data[1] & 3} "
                f"sta_width={(data[1] >> 2) & 1} rest={data[2:].hex(' ')}")
    if eid == 192 and len(data) >= 3:
        return (f"width={data[0]} seg0=ch{data[1]} seg1=ch{data[2]} "
                f"basic_mcs={data[3:].hex(' ')}")
    return data.hex(" ")


def describe(buf):
    """Lines describing the template in buf, or None if no frame is found."""
    off = frame_offset(buf)
    if off is None:
        return None

    f = buf[off:]
    fc = u16(f, 0)
    out = [f"prefix[{off}]={buf[:off].hex(' ')}",
           f"{FC_NAMES[fc]}: fc={fc:#06x} dur={u16(f, 2):#06x} "
           f"a1={f[4:10].hex(':')} a2={f[10:16].hex(':')} "
           f"a3={f[16:22].hex(':')} seq={u16(f, 22):#06x}",
           f"tsf={f[24:32].hex(' ')} interval={u16(f, 32)} "
           f"capab={u16(f, 34):#06x}"]
    i = HDR_LEN + FIXED_LEN
    while i + 2 <= len(f):
        if not any(f[i:]):
            out.append(f"  pad={len(f) - i} zero bytes")
            return out
        eid, n = f[i], f[i + 1]
        data = f[i + 2:i + 2 + n]
        cut = " (cut)" if len(data) < n else ""
        out.append(f"  {eid:3d} {EID.get(eid, '?'):<10} len={n:<3d} "
                   f"{element(eid, data)}{cut}")
        i += 2 + n
    if i < len(f):
        out.append(f"  tail={f[i:].hex(' ')}")
    return out


def hexdump(buf):
    return [f"{i:03x}: {buf[i:i + 16].hex(' ')}" for i in range(0, len(buf), 16)]
