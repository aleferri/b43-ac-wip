#!/usr/bin/env python3
"""
Normalize a wl-diag capture and compare it against the trace emitted by
ac_trace. Only op lines are compared (address / value / mask); the
episode number, timestamp, and cpuN prefix are stripped from the vendor
trace before diffing since the test does not simulate scheduling.

Usage:
    compare.py <vendor.txt> <test.out> [OPTIONS]

--range LO:HI     keep only vendor lines whose episode # is within [LO,HI]
--squash-poll     collapse runs of identical PHY.RD 0x0270 into one line
--auto-align     find the offset in <test> that best matches <vendor>[0]
                  by scanning for the first common op; useful when the
                  test flow does a prologue (save-gain, save-tone, ...)
                  that the vendor does not emit. Reports the offset and
                  compares from there.
--align-on OP    like --auto-align but pin to a specific op string
                  (e.g. 'PHY.WR   addr=0x0463 val=0x0027').

Vendor uses PHY.OR/PHY.AND for single-bit sets/clears; they get folded
to PHY.MOD in the format `val=<kernel_mask> mask=<kernel_set>` so the
test's PHY.MOD lines diff cleanly.
"""
import re
import sys
import argparse

VENDOR_LINE = re.compile(
    r'^\s*[0-9.]+\s+#\d+\s+cpu\d+\s+(.+?)\s*(?:;.*)?$'
)
TEST_LINE = re.compile(r'^cpu\d+\s+(.+?)\s*$')

# Vendor uses PHY.OR (set-in) and PHY.AND (clear-in) alongside PHY.MOD.
# Both are folded to the PHY.MOD single-op form that the wrapper emits:
#   phy_set(X) → PHY.MOD val=X mask=0
#   phy_mask(K) → PHY.MOD val=K mask=0    (K = ~clr in kernel terms)
# PHY.OR line looks like "PHY.OR ... val=<current_or_or_in> (set X)":
#   the (set X) group gives the OR-in bits directly → val=X mask=0.
# PHY.AND line "PHY.AND ... val=<masked> (clr X)": clr X gives the
# bits to clear → val=~X (the kmask) mask=0.
PHY_OR  = re.compile(r'^PHY\.OR\s+addr=(0x[0-9a-f]+)\s+val=0x[0-9a-f]+\s*\(set\s+(0x[0-9a-f]+)\)')
PHY_AND = re.compile(r'^PHY\.AND\s+addr=(0x[0-9a-f]+)\s+val=0x[0-9a-f]+\s*\(clr\s+(0x[0-9a-f]+)\)')

# Op di alto livello e loro "ombra" a livello di core register: il tracer
# vendor logga entrambe, l'harness (come il driver) solo la prima. Le ombre sono
# la coppia addr/data del regcontrol del chipcommon e i registri GPIO; le altre
# SI.COREREG (0x0600, 0x0080, 0x0088, e tutto cio' che non e' core 0) sono op a
# se' stanti e non vanno scartate.
SHADOW_OFFSETS = {
    0x658, 0x65c,          # regcontrol addr/data   <- PMU.RC
    0x660, 0x664,          # pllcontrol addr/data   <- PMU.PLL
    0x064,                 # gpioout               <- GPIO.OUT
    0x068,                 # gpioouten             <- GPIO.OE / GPIO.OUTEN
    0x06c,                 # gpiocontrol           <- GPIO.CTL
    0x08c,                 # gpiopull              <- GPIO.CTL
}
# Op che il driver esegue davvero, ma da codice fuori dall'unita' sotto test:
# l'harness compila solo src/, non main.c di b43 ne' bcma. Vanno saltate, non
# riprodotte, e solo dopo aver verificato che chi le esegue le emetta *nel punto
# giusto* della sequenza -- altrimenti lo skip nasconde un errore d'ordine.
#
#   (core 3, 0x01e0) BCMA_CLKCTLST: b43_bcma_wireless_core_reset chiama
#   bcma_core_set_clockmode(BCMA_CLKMODE_FAST) prima di b43_phy_init, e la
#   cattura mette la richiesta a #37/#40, cioe' prima della prima op PHY (#60).
#   Ordine verificato. La seconda coppia (#115/#118) legge lo stesso registro
#   con HAVEHT ormai alto: assunta ripetizione della stessa richiesta, non
#   verificata separatamente.
#
#   (core 0, 0x600) BCMA_CC_PMU_CTL: il flush PLL_UPD che latcha i valori
#   PLLCTL. La patch 0007 lo emette con bcma_pmu_set32 subito dopo le due
#   bcma_chipco_pll_write, ed e' esattamente dove lo mette la cattura (#31/#34,
#   read e write del read-modify-write, immediatamente dopo le PLL a #15/#23).
#   Ordine verificato.
# Letture pure che il driver non fa: il tracer vendor le registra, ma sono
# read-back senza effetto e riprodurle vorrebbe dire aggiungere una lettura il
# cui risultato viene scartato. Si saltano, con la condizione che siano
# davvero prive di side effect (mask nulla).
FOREIGN_READBACK = re.compile(
    r'^PMU\.PLL addr=0x[23] val=0x0 mask=0x0\b')

FOREIGN_COREREG = {
    (0x3, 0x1e0),
    (0x0, 0x600),
}

SHADOW_PARENT = re.compile(r'^(?:PMU\.(?:RC|PLL)|GPIO\.(?:OUT|OE|OUTEN|CTL))\b')
SI_COREREG = re.compile(r'^SI\.COREREG\s+core=(0x[0-9a-fA-F]+)\s+off=(0x[0-9a-fA-F]+)')

def drop_shadow_ops(ops):
    """Scarta le SI.COREREG che implementano l'op di alto livello precedente."""
    out = []
    parent = False
    for op in ops:
        if FOREIGN_READBACK.match(op):
            parent = True          # le sue ombre restano ombre
            continue
        m = SI_COREREG.match(op)
        if m and (int(m.group(1), 16), int(m.group(2), 16)) in FOREIGN_COREREG:
            continue
        if m and parent and int(m.group(1), 16) == 0 \
                and int(m.group(2), 16) in SHADOW_OFFSETS:
            continue
        parent = bool(SHADOW_PARENT.match(op))
        out.append(op)
    return out

HEXNUM = re.compile(r'\b(0x[0-9a-fA-F]+)\b')

WS = re.compile(r'\s+')

def canon_ws(op: str) -> str:
    """Collassa lo spazio bianco: la spaziatura fra mnemonico e operandi non e'
    informazione, e i due lati la formattano diversamente."""
    return WS.sub(' ', op).strip()

def canon_values(op: str) -> str:
    """Porta ogni letterale esadecimale a una forma canonica senza zeri di
    riempimento.

    Il tracer vendor stampa i valori di ritorno a 32 bit (val=0x00000000) mentre
    l'harness li stampa a 16 (val=0x0000): sono lo stesso numero e il confronto
    non deve dipendere dalla larghezza del campo. Gli indirizzi passano per la
    stessa normalizzazione, che e' innocua perche' entrambi i lati la ricevono."""
    return HEXNUM.sub(lambda m: '0x%x' % int(m.group(1), 16), op)

def normalize_op(op: str) -> str:
    m = PHY_OR.match(op)
    if m:
        addr, setbits = m.groups()
        return canon_ws(canon_values(
            f"PHY.MOD  addr={addr} val={setbits} mask=0x0000"))
    m = PHY_AND.match(op)
    if m:
        addr, clrbits = m.groups()
        return canon_ws(canon_values(
            f"PHY.MOD  addr={addr} val=0x{(~int(clrbits, 16)) & 0xffff:04x} mask=0x0000"))
    # L'harness nomina l'abilitazione GPIO come il simbolo bcma
    # (bcma_chipco_gpio_outen), il tracer vendor come il registro (OE).
    op = re.sub(r'^GPIO\.OUTEN\b', 'GPIO.OE', op)
    return canon_ws(canon_values(op))

VAL_TOK = re.compile(r'val=(?:0x[0-9a-fA-F]+|UNDEFINED)')

RET_SUFFIX = re.compile(r'\s+ret=0x[0-9a-fA-F]+')

def ops_equal(v: str, t: str) -> bool:
    """Confronto op-per-op con il valore letto trattato come wildcard quando il
    vendor non lo ha registrato.

    Le catture prodotte senza "capture ret val" loggano ogni read come
    val=UNDEFINED: la' il valore non e' confrontabile e va ignorato, mentre
    indirizzo e classe di op restano vincolanti. Sulle catture con i RETVAL
    ripiegati (merge_retvals.py) il valore c'e' e viene confrontato."""
    # Le op read-modify-write di bcma (PMU.RC, GPIO.*) portano nel trace vendor
    # il valore riletto dopo la modifica; gli stub bcma dell'harness non
    # modellano quel readback, quindi il suffisso non e' confrontabile.
    v = RET_SUFFIX.sub('', v)
    if v == t:
        return True
    if 'val=UNDEFINED' in v:
        return VAL_TOK.sub('val=*', v, count=1) == VAL_TOK.sub('val=*', t, count=1)
    return False

def extract_episode(raw: str) -> int:
    m = re.search(r'#(\d+)', raw)
    return int(m.group(1)) if m else -1

def load_vendor(path, ep_range):
    lo, hi = ep_range or (0, 10**9)
    out = []
    for line in open(path):
        m = VENDOR_LINE.match(line)
        if not m:
            continue
        ep = extract_episode(line)
        if not (lo <= ep <= hi):
            continue
        out.append(normalize_op(m.group(1)))
    return drop_shadow_ops(out)

def load_test(path):
    out = []
    for line in open(path):
        m = TEST_LINE.match(line)
        if m:
            out.append(normalize_op(m.group(1)))
    return out

def squash_poll(ops, poll_re=re.compile(r'^PHY\.RD\s+addr=0x0270\s')):
    out = []
    prev = None
    for op in ops:
        if poll_re.match(op) and prev == 'poll':
            continue
        if poll_re.match(op):
            out.append(canon_values('PHY.RD  addr=0x0270 val=UNDEFINED  [poll]'))
            prev = 'poll'
        else:
            out.append(op)
            prev = op
    return out

def find_offset(test, target_op):
    """Return the index of `target_op` in test, or -1.

    Usa ops_equal, non l'uguaglianza esatta: se il vendor non ha registrato il
    valore letto l'ancora non deve dipendere da quello."""
    for i, op in enumerate(test):
        if ops_equal(target_op, op):
            return i
    return -1

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('vendor')
    ap.add_argument('test')
    ap.add_argument('--range', help='LO:HI vendor episode range')
    ap.add_argument('--squash-poll', action='store_true')
    ap.add_argument('--auto-align', action='store_true',
                    help='skip test prologue by aligning on vendor[0]')
    ap.add_argument('--align-on', help='align test on this exact op string')
    args = ap.parse_args()

    rng = None
    if args.range:
        lo, hi = args.range.split(':')
        rng = (int(lo), int(hi))

    vendor = load_vendor(args.vendor, rng)
    test = load_test(args.test)

    if args.align_on:
        off = find_offset(test, args.align_on)
        if off < 0:
            print(f"align-on: op not found in test: {args.align_on}")
            return 2
        print(f"aligning test at offset {off} (--align-on)")
        test = test[off:]
    elif args.auto_align and vendor:
        off = find_offset(test, vendor[0])
        if off < 0:
            print(f"auto-align: vendor[0] not found in test: {vendor[0]}")
        else:
            print(f"aligning test at offset {off} (auto: '{vendor[0]}')")
            test = test[off:]

    if args.squash_poll:
        vendor = squash_poll(vendor)
        test = squash_poll(test)

    print(f"vendor: {len(vendor)} ops")
    print(f"test:   {len(test)} ops")

    n = min(len(vendor), len(test))
    mismatches = 0
    for i in range(n):
        if not ops_equal(vendor[i], test[i]):
            mismatches += 1
            if mismatches <= 20:
                print(f"  @{i}:")
                print(f"    vendor: {vendor[i]}")
                print(f"    test:   {test[i]}")
    if len(vendor) != len(test):
        print(f"length differs: vendor={len(vendor)} test={len(test)}")

    if mismatches == 0 and len(vendor) == len(test):
        print("MATCH")
        return 0
    print(f"total mismatches (compared prefix): {mismatches}")
    return 1

if __name__ == '__main__':
    sys.exit(main())
