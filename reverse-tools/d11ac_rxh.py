#!/usr/bin/env python3
"""The d11 AC RX header as the stock driver receives it, read the way b43
reads it, so a capture of the stock driver checks b43's RX path field by
field.

b43 (xmit.c, b43_rx()) takes, from the start of the DMA buffer: frame_len
(+0), PHY RX status 0-5 (+4..+15), MAC RX status (+16, 32 bits), MAC time
(+20), channel (+22), the per-core power in dBm (bytes +9 and +10); the frame
starts at +40 (B43_DMA0_RX_AC_FO), with two bytes of padding first when MAC
status has PADDING, then the 6-byte PLCP, whose frame type is PHY status 0
bits 1:0, then the 802.11 header. What the stock driver puts in +24..+39 is
printed raw.
"""

from d11ac_txh import FT, plcp_text, u16

HDR_LEN = 40
MAC_FLAGS = ((0x00000001, "fcserr"), (0x00000002, "resp"),
             (0x00000004, "pad"), (0x00000008, "dec"), (0x00000010, "decerr"),
             (0x00008000, "bcnsent"), (0x00010000, "amsdu"))


def s8(b):
    return b - 256 if b > 127 else b


def describe(buf, total):
    """Lines describing the header in buf, or None if buf is too short to
    hold one. total is the packet length the driver had."""
    if len(buf) < 24:
        return None

    flen = u16(buf, 0)
    ph = [u16(buf, 4 + 2 * i) for i in range(6)]
    mac = u16(buf, 16) | (u16(buf, 18) << 16)
    flags = [n for b, n in MAC_FLAGS if mac & b]
    agg = (mac >> 17) & 3
    check = ("len ok" if total == HDR_LEN + flen else
             f"frame_len != len-{HDR_LEN} ({total - HDR_LEN})")
    out = [f"rxh: frame_len={flen} ({check}) "
           f"phy={' '.join(f'{w:#06x}' for w in ph)} "
           f"pwr={s8(buf[9])},{s8(buf[10])}",
           f"mac={mac:#010x} [{','.join(flags)}{' agg=' + str(agg) if agg else ''}] "
           f"time={u16(buf, 20):#06x} chan={u16(buf, 22):#06x} "
           f"tail={buf[24:HDR_LEN].hex(' ')}"]

    off = HDR_LEN + (2 if mac & 0x4 else 0)
    if len(buf) >= off + 6:
        ft = ph[0] & 3
        p = buf[off:off + 6]
        out.append(f"plcp@{off}: ft={FT[ft]} {p.hex(' ')} ({plcp_text(ft, p)})")
    f = buf[off + 6:]
    if len(f) >= 16:
        fc = u16(f, 0)
        out.append(f"802.11: fc={fc:#06x} type={(fc >> 2) & 3} "
                   f"subtype={(fc >> 4) & 0xf:#x} dur={u16(f, 2):#06x} "
                   f"a1={f[4:10].hex(':')} a2={f[10:16].hex(':')}")
    return out
