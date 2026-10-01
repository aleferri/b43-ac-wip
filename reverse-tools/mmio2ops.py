#!/usr/bin/env python3
"""Decode a bus-level capture of a PCIe BCMA wireless chip into op classes.

Four capture formats, the same device traffic:

    mmiotrace     the Linux kernel's mmiotrace text, taken on an x86 host
                  (router-data/archer-t5e);
    wl-mmio-trap  the binary records of wl-mmio-trap/, taken on a MIPS
                  big-endian router (router-data/agcombo/*.bin);
    bpftrace      `<ns> R32|W16|... <va> <val>` lines from kprobes on the
                  hybrid wl's osl_read*/osl_write* (router-data/macbookair6-1);
    ftrace        the kprobe events of the same accessors as the ftrace
                  buffer prints them, plus osl_pci_write_config
                  (router-data/macbookair6-1, the wl-firstload and wl-tx ones).

The trace is the raw MMIO traffic on BAR0 (16 KiB, PCIe gen2 layout):

    0x0000-0x0fff  sliding window (PCI config BAR0_WIN, not visible here)
    0x1000-0x1fff  wrapper (agent) of the core in the window
    0x2000-0x2fff  PCIe core, fixed alias
    0x3000-0x3fff  chipcommon, fixed alias (SROM at 0x3800)

The window moves through PCI config space, which mmiotrace does not record,
so the core behind the sliding window is inferred: unambiguous register
offsets (chipcommon OTP/SROM/PMU words, D11 PHY/radio/SHM ports, ...) set
the state, ambiguous ones inherit it. The ftrace captures record the config
writes: there the window is known, the cores come from the EROM the driver
reads, and an access to a core other than chipcommon and D11 is CORE.*.
The other config writes are PCICFG.WR.

Output lines follow the vendor capture format read by tracelib.py:

    <ts> #<n> cpu0 CLASS key=val ...

with the bus vocabulary of test/integration (PHY.WR/RD, RAD.WR/RD, OBJ.WR/RD
with sel=, MAC.MCTRL, MAC.MCMD, REG.WR/RD off=) plus CC.*, SROM.RD, WRAP.*,
PCIE.*, EROM.RD, CORE.* and PCICFG.WR for the parts the vendor tracer never
saw.
"""
import argparse
import collections
import re
import struct
import sys

BAR0 = 0xfe400000
BAR0_SIZE = 0x4000

# D11 registers
MACCTL = 0x120
MACCMD = 0x124
OBJADDR = 0x160
OBJDATA = 0x164
OBJDATA_HI = 0x166
RADIO_CTL = 0x3d8
RADIO_DATA = 0x3da
PHY_VER = 0x3e0
PHY_CTL = 0x3fc
PHY_DATA = 0x3fe

SHM_SEL = 1

# PCI config space of the BCMA host bridge
PCI_BAR0_WIN = 0x80
PCI_BAR0_WRAP = (0x70, 0xac)    # wrapper window: PCIe gen2, gen1

SI_ENUM_BASE = 0x18000000       # chipcommon
CORE_CC = 0x800
CORE_D11 = 0x812
CC_EROMPTR = 0xfc

# chipcommon registers whose offset no D11 core uses: one of these in the
# sliding window means the window is on the chipcommon. The OTP block
# (0x10-0x1c, 0xf4) is left out because 0x18 is also the D11 gptimer; an
# otpprog write is recognised by its start bit instead.
CC_ONLY = {
    0x2c: "chipstatus", 0x4c: "otplayoutext", 0x58: "bcastaddr",
    0x5c: "bcastdata", 0xf0: "clkdiv2", 0xf8: "fabid", 0xfc: "eromptr",
    0x190: "sromcontrol", 0x194: "sromaddress", 0x198: "sromdata",
}
CC_NAMES = dict(CC_ONLY)
CC_NAMES.update({
    0x00: "chipid", 0x04: "capabilities", 0x08: "corecontrol", 0x0c: "bist",
    0x40: "flashcontrol", 0x44: "flashaddress", 0x48: "flashdata",
    0x10: "otpstatus", 0x14: "otpcontrol", 0x18: "otpprog", 0x1c: "otplayout",
    0xf4: "otpcontrol1",
    0x20: "intstatus", 0x24: "intmask", 0x28: "chipcontrol",
    0x64: "gpioout", 0x68: "gpioouten", 0x6c: "gpiocontrol", 0x70: "gpiointpol",
    0x74: "gpiointmask", 0x88: "gpiotimerval", 0x8c: "gpiotimeroutmask",
    0x1e0: "clk_ctl_st",
    0x600: "pmucontrol", 0x604: "pmucapabilities", 0x608: "pmustatus",
    0x60c: "res_state", 0x610: "res_pending", 0x614: "pmutimer",
    0x618: "min_res_mask", 0x61c: "max_res_mask", 0x620: "res_table_sel",
    0x624: "res_dep_mask", 0x628: "res_updn_timer", 0x62c: "res_timer",
    0x630: "clkstretch", 0x634: "pmuwatchdog", 0x638: "gpiosel",
    0x63c: "gpioenable", 0x650: "chipcontrol_addr", 0x654: "chipcontrol_data",
    0x658: "regcontrol_addr", 0x65c: "regcontrol_data", 0x660: "pllcontrol_addr",
    0x664: "pllcontrol_data",
})
WRAP_NAMES = {
    0x160: "oobseloutd30", 0x164: "oobseloutd74", 0x408: "ioctrl",
    0x500: "iostatus", 0x800: "resetctrl", 0x804: "resetstatus",
}

Op = collections.namedtuple("Op", "ts kind width off val")


def parse_mmiotrace(path):
    for line in open(path):
        if line[0] not in "RW":
            continue
        f = line.split()
        yield Op(float(f[2]), f[0], int(f[1]), int(f[4], 16) - BAR0, int(f[5], 16))


# struct wl_mmio_rec of wl-mmio-trap/wl_mmio_trap_main.c, packed, big-endian.
TRAP_REC = struct.Struct(">QIIIIBBH")      # ts_ns seq addr val aux op cpu pad
TRAP_RD, TRAP_WR, TRAP_MARK, TRAP_DROP = 60, 61, 62, 255
TRAP_AUX_WIDTH = 0xff


def trap_device_offset(off, width):
    """The register a sub-word access reaches on the device.

    The trap records the offset of wl's own load or store, the CPU side. On
    the MIPS big-endian host the 16-bit register accessors address the other
    half of the 32-bit word, r ^ 2, so the device sees offset ^ 2: taken
    literally, the capture writes PHY addresses to PHY_DATA and reads the
    data back from PHY_CONTROL. 32-bit accesses are not moved."""
    return off ^ 2 if width == 2 else off


def parse_wl_mmio_trap(path):
    """Records of a wl_mmio_trap capture; marks come as kind 'M' with the
    label in `val`, drops are reported on stderr, as the queue loses them."""
    data = open(path, "rb").read()
    if len(data) % TRAP_REC.size:
        print(f"{path}: {len(data) % TRAP_REC.size} trailing bytes ignored",
              file=sys.stderr)
    for i in range(0, len(data) - TRAP_REC.size + 1, TRAP_REC.size):
        ts, seq, addr, val, aux, op, cpu, _ = TRAP_REC.unpack_from(data, i)
        t = ts / 1e9
        if op in (TRAP_RD, TRAP_WR):
            width = aux & TRAP_AUX_WIDTH
            yield Op(t, "R" if op == TRAP_RD else "W", width,
                     trap_device_offset(addr, width), val)
        elif op == TRAP_MARK:
            raw = b"".join(w.to_bytes(4, "big") for w in (addr, val, aux))
            yield Op(t, "M", 0, 0, raw.split(b"\0")[0].decode("ascii", "replace"))
        elif op == TRAP_DROP:
            print(f"{path}: {aux} records dropped before #{seq}", file=sys.stderr)


def parse_bpftrace(path):
    """Accesses of a bpftrace capture of osl_read*/osl_write*.

    The addresses are kernel virtual addresses of wl's BAR0 mapping; the base
    is the page of the lowest one, which is the sliding window at BAR0 + 0.
    The osl_delay lines are not bus traffic and are skipped. The script keys
    the open read by thread, so a read nested in an interrupt on the same
    thread loses its address and prints 0: those are dropped, and counted."""
    acc, lost = [], 0
    for line in open(path):
        f = line.split()
        if len(f) == 4 and f[1][0] in "RW" and f[1][1:] in ("8", "16", "32"):
            if int(f[2], 16) == 0:
                lost += 1
                continue
            acc.append((int(f[0]), f[1][0], int(f[1][1:]) // 8,
                        int(f[2], 16), int(f[3], 16)))
    if lost:
        print(f"{path}: {lost} reads without their address dropped", file=sys.stderr)
    base = min(a[3] for a in acc) & ~0xfff
    for ts, kind, width, addr, val in acc:
        yield Op(ts / 1e9, kind, width, addr - base, val)


FTRACE_LINE = re.compile(r"^\s*.+?-(?P<pid>\d+)\s+\[(?P<cpu>\d+)\]\s+\S+\s+(?P<ts>[\d.]+): "
                         r"(?P<ev>\w+): \([^)]*\)(?P<args>.*)$")


def parse_ftrace(path):
    """Accesses of an ftrace capture of osl_read*/osl_write* and of
    osl_pci_write_config, as the trace buffer prints the kprobe events.

    A read is two events of its task, the entry with the address (r32) and
    the return with the value (r32r), paired by task, and by CPU for the idle
    tasks, which all have pid 0. An interrupt can take the CPU between the
    two and read on its own, so each task keeps a stack of open reads; a read
    takes its place and time from its return. The base is found as in
    bpftrace. A
    config write is an Op of kind 'C' with the config offset in `off`. The
    osl_delay events are not bus traffic and are skipped."""
    acc, pending = [], collections.defaultdict(list)
    for line in open(path):
        m = FTRACE_LINE.match(line)
        if not m:
            continue
        ev = m["ev"]
        pid = m["pid"] if m["pid"] != "0" else "0/" + m["cpu"]
        args = dict(kv.split("=", 1) for kv in m["args"].split() if "=" in kv)
        ts = float(m["ts"])
        if ev == "cfgw":
            acc.append((ts, "C", int(args["size"]), int(args["off"], 16), int(args["val"], 16)))
        elif ev in ("w8", "w16", "w32"):
            acc.append((ts, "W", int(ev[1:]) // 8, int(args["addr"], 16), int(args["val"], 16)))
        elif ev in ("r8", "r16", "r32"):
            pending[pid].append((ts, int(ev[1:]) // 8, int(args["addr"], 16)))
        elif ev in ("r8r", "r16r", "r32r"):
            if not pending[pid] or pending[pid][-1][1] != int(ev[1:-1]) // 8:
                print(f"{path}: {ev} of task {pid} at {ts} without its entry", file=sys.stderr)
                continue
            _, width, addr = pending[pid].pop()
            acc.append((ts, "R", width, addr, int(args["ret"], 16)))
    base = min(a[3] for a in acc if a[1] != "C") & ~0xfff
    for ts, kind, width, addr, val in acc:
        yield Op(ts, kind, width, addr if kind == "C" else addr - base, val)


PARSERS = {"mmiotrace": parse_mmiotrace, "wl-mmio-trap": parse_wl_mmio_trap,
           "bpftrace": parse_bpftrace, "ftrace": parse_ftrace}


def guess_format(path):
    with open(path, "rb") as f:
        head = f.read(64)
    try:
        text = head.decode("ascii")
    except UnicodeDecodeError:
        return "wl-mmio-trap"
    # Both are ftrace buffers; mmiotrace names its own tracer.
    if text.startswith("# tracer:") and not text.startswith("# tracer: mmiotrace"):
        return "ftrace"
    return "mmiotrace"


class Decoder:
    """Turns bus accesses into ops; one instance per trace."""

    def __init__(self, bulk_min=2, mark_windows=False):
        self.mark_windows = mark_windows
        self.window = "CC"
        self.erom_next = None
        self.erom_words = {}
        # Set by the first config write: the window is known from then on.
        self.exact = False
        self.erom_base = None
        self.cores = {}
        self.win_core = None
        self.phy_addr = None
        self.radio_addr = None
        self.objaddr = 0
        self.bulk_min = bulk_min
        self.bulk = None

    # -- window inference ---------------------------------------------------

    def is_d11(self, op):
        o, w = op.off, op.width
        if o in CC_ONLY:
            return False
        if 0x120 <= o < 0x1e0 or 0x200 <= o < 0x300 or 0x3d8 <= o < 0x400:
            return True
        if w == 2 and 0x400 <= o < 0x1000:
            return not (self.window == "CC" and op.kind == "R" and 0x800 <= o < 0xa00)
        if op.kind == "W" and 0x100 <= o < 0x120:
            return True
        return False

    def is_cc(self, op):
        o, w = op.off, op.width
        if o in CC_ONLY or (w == 4 and 0x600 <= o < 0x680):
            return True
        return o == 0x18 and op.kind == "W" and bool(op.val & 0x80000000)

    def track_window(self, op):
        if self.exact:
            return
        if self.window == "EROM":
            if op.off == self.erom_next or op.off == self.erom_next - 4:
                self.erom_next = op.off + 4
                return
            self.window = "CC"
        if self.is_d11(op):
            self.window = "D11"
        elif self.is_cc(op):
            self.window = "CC"

    def after(self, op):
        """The eromptr read is the last chipcommon access before the window
        moves onto the EROM."""
        if self.window != "CC" or op.off != CC_EROMPTR or op.kind != "R":
            return
        if self.exact:
            self.erom_base = op.val & ~0xfff
            return
        self.window = "EROM"
        self.erom_next = 0

    def move_window(self, op):
        """A write of the BAR0 window in PCI config space. The EROM is read
        through the window before any other core, so the cores it lists name
        every later position."""
        self.exact = True
        if op.off != PCI_BAR0_WIN:
            return
        if self.window == "EROM" and not self.cores:
            self.cores = {addr: c["id"] for c in erom_cores(
                [self.erom_words[k] for k in sorted(self.erom_words)])
                for addr, kind in c["addr"] if kind == "slave"}
        base = op.val & ~0xfff
        if base == self.erom_base:
            self.window = "EROM"
            return
        self.win_core = self.cores.get(base, CORE_CC if base == SI_ENUM_BASE else None)
        self.win_base = base
        self.window = {CORE_CC: "CC", CORE_D11: "D11"}.get(self.win_core, "CORE")

    # -- emission -----------------------------------------------------------

    def decode(self, ops):
        for op in ops:
            if op.kind == "M":
                yield from ((op.ts, o) for o in self.flush_bulk())
                yield op.ts, f"MARK '{op.val}'"
                continue
            if op.kind == "C":
                yield from ((op.ts, o) for o in self.flush_bulk())
                before = self.window
                self.move_window(op)
                if op.off not in (PCI_BAR0_WIN,) + PCI_BAR0_WRAP:
                    yield op.ts, f"PCICFG.WR off=0x{op.off:02x} val=0x{op.val:08x}"
                elif self.mark_windows and op.off == PCI_BAR0_WIN:
                    yield op.ts, f"WIN core={self.window} from={before} base=0x{op.val:08x}"
                continue
            before = self.window
            self.track_window(op)
            if self.mark_windows and self.window != before:
                yield from ((op.ts, o) for o in self.flush_bulk())
                yield op.ts, f"WIN core={self.window} from={before}"
            for out in self.classify(op):
                yield op.ts, out
            self.after(op)
        yield from self.flush_bulk()

    def classify(self, op):
        o = op.off
        if o >= 0x3000:
            yield from self.flush_bulk()
            yield from self.cc(op, o - 0x3000)
        elif o >= 0x2000:
            yield from self.flush_bulk()
            yield f"PCIE.{rw(op)} off=0x{o - 0x2000:04x} val={hexw(op)}"
        elif o >= 0x1000:
            yield from self.flush_bulk()
            name = WRAP_NAMES.get(o - 0x1000, "")
            yield f"WRAP.{rw(op)} off=0x{o - 0x1000:04x} val={hexw(op)}" + (f" reg={name}" if name else "")
        elif self.window == "EROM":
            self.erom_words[o] = op.val
            yield f"EROM.RD off=0x{o:04x} val={hexw(op)}"
        elif self.window == "CORE":
            core = f"core=0x{self.win_core:03x}" if self.win_core is not None \
                else f"base=0x{self.win_base:08x}"
            yield f"CORE.{rw(op)} {core} off=0x{o:04x} val={hexw(op)}"
        elif self.window == "CC":
            yield from self.cc(op, o)
        else:
            yield from self.d11(op)

    def cc(self, op, o):
        if 0x800 <= o < 0xa00 and op.width == 2:
            yield f"SROM.RD word=0x{(o - 0x800) >> 1:03x} val={hexw(op)}"
            return
        name = CC_NAMES.get(o, "")
        yield f"CC.{rw(op)} off=0x{o:04x} val={hexw(op)}" + (f" reg={name}" if name else "")

    def d11(self, op):
        o, v, w = op.off, op.val, op.width
        if o not in (OBJDATA, OBJDATA_HI):
            yield from self.flush_bulk()
        # A read of an address register is the posted-write flush of the
        # write before it (the MIPS routers do one after every address
        # write); it is a bus operation of its own and stays in the output.
        if o in (PHY_CTL, RADIO_CTL, OBJADDR) and op.kind == "R" and w != 4 \
                or o == OBJADDR and op.kind == "R":
            yield f"REG.RD off=0x{o:04x} val={hexw(op)}"
            return
        if o == PHY_CTL:
            if w == 4:
                self.phy_addr = v & 0xffff
                yield f"PHY.WR addr=0x{self.phy_addr:04x} val=0x{v >> 16:04x}"
            else:
                self.phy_addr = v
            return
        if o == PHY_DATA:
            yield f"PHY.{rw(op)} addr=0x{self.phy_addr or 0:04x} val=0x{v & 0xffff:04x}"
            return
        if o == RADIO_CTL and w == 2:
            self.radio_addr = v
            return
        if o == RADIO_DATA:
            yield f"RAD.{rw(op)} addr=0x{self.radio_addr or 0:04x} val=0x{v:04x}"
            return
        if o == OBJADDR:
            self.objaddr = v
            return
        if o in (OBJDATA, OBJDATA_HI):
            yield from self.obj(op)
            return
        if o == MACCTL and op.kind == "W":
            yield f"MAC.MCTRL val=0x{v:08x}"
            return
        if o == MACCMD and op.kind == "W":
            yield f"MAC.MCMD val=0x{v:08x}"
            return
        yield f"REG.{rw(op)} off=0x{o:04x} val={hexw(op)}"

    # -- shared memory --------------------------------------------------------

    def obj(self, op):
        sel = (self.objaddr >> 16) & 0xff
        autoinc = (self.objaddr >> 24) & 3
        idx = self.objaddr & 0xffff
        cls = "OBJ." + rw(op)
        if sel == SHM_SEL:
            addr = (idx << 2) + (2 if op.off == OBJDATA_HI else 0)
        else:
            addr = idx
        if op.width == 4 and autoinc:
            self.objaddr += 1
            yield from self.bulk_add(cls, sel, addr, op.val)
            return
        yield from self.flush_bulk()
        yield from self.obj_ops(cls, sel, addr, op.val, op.width)

    @staticmethod
    def obj_ops(cls, sel, addr, val, width):
        """One bus access as the ops the vendor logs: SHM is 16-bit words,
        so a 32-bit access there is two ops, low half first (little-endian
        host); the index-addressed spaces take the whole word."""
        tag = f"sel=0x{sel << 16:x}"
        if width == 2:
            yield f"{cls} addr=0x{addr:04x} val=0x{val:04x} {tag}"
        elif sel != SHM_SEL:
            yield f"{cls} addr=0x{addr:04x} val=0x{val:08x} {tag}"
        else:
            yield f"{cls} addr=0x{addr:04x} val=0x{val & 0xffff:04x} {tag}"
            yield f"{cls} addr=0x{addr + 2:04x} val=0x{val >> 16:04x} {tag}"

    def bulk_add(self, cls, sel, addr, val):
        step = 4 if sel == SHM_SEL else 1
        if self.bulk and (self.bulk["cls"], self.bulk["sel"]) == (cls, sel) \
                and self.bulk["next"] == addr:
            self.bulk["vals"].append(val)
            self.bulk["next"] = addr + step
            return
        yield from self.flush_bulk()
        self.bulk = {"cls": cls, "sel": sel, "addr": addr, "vals": [val],
                     "next": addr + step}

    def flush_bulk(self):
        b, self.bulk = self.bulk, None
        if not b:
            return
        step = 4 if b["sel"] == SHM_SEL else 1
        if len(b["vals"]) >= self.bulk_min:
            yield f"{b['cls'].replace('.', '.BULK')[:-1]} addr=0x{b['addr']:04x} len={len(b['vals'])} width=32 sel=0x{b['sel'] << 16:x}"
            return
        for i, v in enumerate(b["vals"]):
            yield from self.obj_ops(b["cls"], b["sel"], b["addr"] + i * step, v, 4)


def hexw(op):
    return f"0x{op.val:0{2 * op.width}x}"


def rw(op):
    return "WR" if op.kind == "W" else "RD"


# BCMA_CORE_* of include/linux/bcma/bcma.h; the ARM ones (mfg 0x43b) by role.
CORE_NAMES = {0x800: "chipcommon", 0x812: "d11", 0x83c: "pcie2", 0x83e: "armcr4",
              0x81a: "usb20dev", 0x840: "gci", 0x820: "pcie", 0x827: "pmu",
              0x135: "default", 0x367: "oob-router", 0x366: "erom", 0x301: "axi2apb"}


def erom_cores(words):
    """Component and address descriptors of an EROM, as bcma scans them."""
    cores, i = [], 0
    while i < len(words):
        w = words[i]
        tag = w & 0xe
        if tag == 0xe:
            break
        if (w & 0x6) == 4:
            kind = {0: "slave", 0x40: "bridge", 0x80: "swrap", 0xc0: "mwrap"}[w & 0xc0]
            addr = w & ~0xfff
            i += 1
            if w & 8:
                addr |= words[i] << 32
                i += 1
            if (w & 0x30) == 0x30:
                if words[i] & 8:
                    i += 1
                i += 1
            if cores:
                cores[-1]["addr"].append((addr, kind))
            continue
        if tag == 0:
            cib = words[i + 1]
            cores.append({"id": (w >> 8) & 0xfff, "mfg": w >> 20, "rev": cib >> 24,
                          "addr": []})
            i += 2
            continue
        i += 1
    return cores


def erom_report(ops, out):
    dec = Decoder()
    for _ in dec.decode(ops):
        pass
    words = dec.erom_words
    for c in erom_cores([words[k] for k in sorted(words)]):
        name = CORE_NAMES.get(c["id"], "?")
        addr = ", ".join(f"{a:#x}/{k}" for a, k in c["addr"])
        out.write(f"core 0x{c['id']:03x} {name:<10} rev {c['rev']:<3} mfg 0x{c['mfg']:03x}  {addr}\n")


def write_srom(ops, path):
    words = {}
    for op in ops:
        if op.off >= 0x3800 and op.off < 0x3a00 and op.width == 2 and op.kind == "R":
            words[(op.off - 0x3800) >> 1] = op.val
    if not words:
        return 0
    n = max(words) + 1
    with open(path, "w") as f:
        for i in range(0, n, 8):
            row = "  ".join(f"0x{words.get(j, 0):04x}" for j in range(i, min(i + 8, n)))
            f.write(f"  srom[{i:03d}]:  {row}  \n")
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--format", choices=sorted(PARSERS),
                    help="capture format (default: binary is wl-mmio-trap, text is mmiotrace; "
                         "bpftrace must be named)")
    ap.add_argument("-o", "--out", help="decoded ops (default: stdout)")
    ap.add_argument("--srom", help="write the SROM words read through the chipcommon alias here")
    ap.add_argument("--erom", action="store_true",
                    help="print the cores enumerated from the EROM and exit")
    ap.add_argument("--no-bulk", action="store_true",
                    help="expand auto-increment runs into single OBJ ops")
    ap.add_argument("--mark-windows", action="store_true",
                    help="emit a WIN line where the sliding window is inferred to move")
    ap.add_argument("--keep-flush", action="store_true",
                    help="keep the PHY_VER read that follows PHY writes (wl's write flush)")
    args = ap.parse_args()

    ops = list(PARSERS[args.format or guess_format(args.trace)](args.trace))
    if args.erom:
        erom_report(ops, sys.stdout)
        return
    if args.srom:
        n = write_srom(ops, args.srom)
        print(f"srom: {n} words -> {args.srom}", file=sys.stderr)

    dec = Decoder(bulk_min=1 << 30 if args.no_bulk else 2,
                  mark_windows=args.mark_windows)
    out = open(args.out, "w") if args.out else sys.stdout
    n = 0
    for ts, line in dec.decode(ops):
        if not args.keep_flush and line.startswith("REG.RD off=0x03e0"):
            continue
        n += 1
        out.write(f"{ts:15.6f} #{n:<8} cpu0 {line}\n")
    if args.out:
        out.close()
        print(f"{n} ops -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
