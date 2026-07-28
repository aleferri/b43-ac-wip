#!/usr/bin/env python3
"""Fit the AC-PHY RX IQ-cal coefficient formula against known ground truth.

The driver reads three 32-bit accumulators per chain (ii, qq, iq) and writes two
10-bit coefficients (a -> 0x?a0, b -> 0x?a1). The port implements the N-PHY
division form; the vendor blob reaches `wlc_phy_inv_cordic` from
`wlc_phy_calc_iq_mismatch_acphy`, so the real computation may go through an
arctangent instead. At the imbalance magnitudes these captures exercise the two
are numerically indistinguishable, so this harness enumerates candidate
formulations -- including atan2-based ones -- and scores each against every
known point.

Ground truth comes from the two d6220 captures that carry RETVALs: the
accumulator values are what the read oracle serves, the coefficients are what
the vendor writes to 0x06a0/0x06a1 and 0x08a0/0x08a1.

The harness also covers the "average over more pairs" family: instead of summing
the accumulators and dividing once, average a per-round quantity (angle or
ratio) over the last N rounds. That family is the only way to escape the
reachability bound below, since the mean of ratios is not the ratio of sums --
so it gets tested explicitly.

Usage:
    python3 fit_rxiq_solve.py [--verbose]

A candidate scores 4/4 on `b` only if it reproduces every point exactly. Note
the bound check printed at the end: it tells you whether a point is reachable
at all from its accumulators, independent of the formula.
"""
import argparse
import math

# (label, ii, qq, iq, a_expected, b_expected)
POINTS = [
    ("attach   c0", 73136278, 84597357, 1231300, -17, 0x4d),
    ("attach   c1", 56790723, 63629275, 2433520, -44, 0x3b),
    ("down->up c0", 80526475, 92834643, 1279425, -16, 0x4c),
    ("down->up c1", 62320086, 69593985, 2648374, -44, 0x39),
]

Q = 1 << 10          # coefficients are Q10
UNITY = 1 << 10      # b is stored as a delta from unity gain


# ---------------------------------------------------------------- rounding ---
def r_near(x):
    """round half away from zero"""
    return math.floor(x + 0.5) if x >= 0 else -math.floor(-x + 0.5)


def isqrt_floor(v):
    return math.isqrt(v) if v > 0 else 0


def isqrt_near(v):
    """isqrt then +1 if the remainder exceeds the root -- round to nearest"""
    if v <= 0:
        return 0
    b = math.isqrt(v)
    return b + 1 if v - b * b > b else b


def isqrt_near_ge(v):
    if v <= 0:
        return 0
    b = math.isqrt(v)
    return b + 1 if v - b * b >= b else b


def isqrt_ceil(v):
    if v <= 0:
        return 0
    b = math.isqrt(v)
    return b if b * b == v else b + 1


SQRTS = {
    "floor": isqrt_floor,
    "near": isqrt_near,
    "near>=": isqrt_near_ge,
    "ceil": isqrt_ceil,
}


def div_sym(num, den):
    """integer divide with rounding away from zero, as the port does"""
    if num >= 0:
        return (num + den // 2) // den
    return -((-num + den // 2) // den)


# --------------------------------------------------------------- a variants ---
def a_div(ii, qq, iq):
    """port: -(iq << 10) / ii, symmetric rounding"""
    return div_sym(-(iq << 10), ii)


def a_atan2_ii(ii, qq, iq):
    return r_near(-Q * math.atan2(iq, ii))


def a_atan2_geo(ii, qq, iq):
    return r_near(-Q * math.atan2(iq, math.sqrt(ii * qq)))


def a_asin_geo(ii, qq, iq):
    s = iq / math.sqrt(ii * qq)
    s = max(-1.0, min(1.0, s))
    return r_near(-Q * math.asin(s))


def a_div_geo(ii, qq, iq):
    return r_near(-Q * iq / math.sqrt(ii * qq))


def a_div_qq(ii, qq, iq):
    return div_sym(-(iq << 10), qq)


A_VARIANTS = {
    "-(iq<<10)/ii  [port]": a_div,
    "-Q*atan2(iq, ii)": a_atan2_ii,
    "-Q*atan2(iq, sqrt(ii*qq))": a_atan2_geo,
    "-Q*asin(iq/sqrt(ii*qq))": a_asin_geo,
    "-Q*iq/sqrt(ii*qq)": a_div_geo,
    "-(iq<<10)/qq": a_div_qq,
}


# --------------------------------------------------------------- b variants ---
def b_port(ii, qq, iq, a, sq):
    v = ((qq << 20) + (ii >> 1)) // ii - a * a
    return sq(v) - UNITY


def b_no_a2(ii, qq, iq, a, sq):
    v = ((qq << 20) + (ii >> 1)) // ii
    return sq(v) - UNITY


def b_a2_before_div(ii, qq, iq, a, sq):
    v = ((qq << 20) - a * a * ii + (ii >> 1)) // ii
    return sq(v) - UNITY


def b_trunc_div(ii, qq, iq, a, sq):
    v = (qq << 20) // ii - a * a
    return sq(v) - UNITY


def b_cos(ii, qq, iq, a, sq):
    """amplitude ratio scaled by cos(phase): sqrt(qq/ii)*cos(phi)"""
    phi = a / Q
    return r_near(Q * math.sqrt(qq / ii) * math.cos(phi)) - UNITY


def b_sin2(ii, qq, iq, a, sq):
    phi = a / Q
    x = qq / ii - math.sin(phi) ** 2
    return r_near(Q * math.sqrt(max(x, 0.0))) - UNITY


def b_tan2(ii, qq, iq, a, sq):
    phi = a / Q
    x = qq / ii - math.tan(phi) ** 2
    return r_near(Q * math.sqrt(max(x, 0.0))) - UNITY


def b_project(ii, qq, iq, a, sq):
    """sqrt((qq - iq^2/ii)/ii): amplitude with the correlated part projected out"""
    x = (qq - iq * iq / ii) / ii
    return r_near(Q * math.sqrt(max(x, 0.0))) - UNITY


def b_ratio_only(ii, qq, iq, a, sq):
    return r_near(Q * math.sqrt(qq / ii)) - UNITY


B_VARIANTS = {
    "isqrt(rnd(qq<<20/ii) - a^2)  [port]": b_port,
    "isqrt(rnd(qq<<20/ii))  (no a^2)": b_no_a2,
    "isqrt((qq<<20 - a^2*ii)/ii)": b_a2_before_div,
    "isqrt(trunc(qq<<20/ii) - a^2)": b_trunc_div,
    "Q*sqrt(qq/ii)*cos(phi)": b_cos,
    "Q*sqrt(qq/ii - sin^2 phi)": b_sin2,
    "Q*sqrt(qq/ii - tan^2 phi)": b_tan2,
    "Q*sqrt((qq - iq^2/ii)/ii)": b_project,
    "Q*sqrt(qq/ii)  (no phase term)": b_ratio_only,
}

# variants that do their own rounding: the sqrt rule does not apply to them
FLOAT_B = {"Q*sqrt(qq/ii)*cos(phi)", "Q*sqrt(qq/ii - sin^2 phi)",
           "Q*sqrt(qq/ii - tan^2 phi)", "Q*sqrt((qq - iq^2/ii)/ii)",
           "Q*sqrt(qq/ii)  (no phase term)"}


# ------------------------------------------------ per-round accumulators ---
# Tutti i 6 round per punto, dallo stesso strumento che ha prodotto POINTS.
# Servono a testare la famiglia "media su piu' coppie": il port somma le ultime
# due e divide una volta, ma il vendor potrebbe mediare una grandezza per-round.
ROUNDS = {
    "attach   c0": [(9969944, 9439578, -102280), (8057549, 8641806, 99946),
                    (4502743, 5180388, 87934), (2222454, 2546265, 29254),
                    (36744394, 42572638, 560867), (36391884, 42024719, 670433)],
    "attach   c1": [(9893769, 9454746, 78909), (6833137, 7551520, 260913),
                    (3611662, 4035173, 149003), (1774984, 1984061, 72000),
                    (28548808, 32110404, 1148181), (28241915, 31518871, 1285339)],
    "down->up c0": [(9892736, 9568361, -44200), (8479295, 8852235, 151713),
                    (4953717, 5698622, 68941), (2433522, 2797786, 42086),
                    (40401282, 46825015, 553916), (40125193, 46009628, 725509)],
    "down->up c1": [(9418150, 9631129, 351368), (7415513, 8005801, 264165),
                    (3960739, 4409751, 161322), (1939684, 2173524, 78302),
                    (31461093, 35137086, 1194269), (30858993, 34456899, 1454105)],
}


def mean_angle(rounds, n, use_atan):
    sel = rounds[-n:]
    if use_atan:
        tot = sum(math.atan2(iq, ii) for ii, qq, iq in sel)
    else:
        tot = sum(iq / ii for ii, qq, iq in sel)
    return r_near(-Q * tot / len(sel))


def angle_of_sums(rounds, n, use_atan):
    sel = rounds[-n:]
    si = sum(x[0] for x in sel)
    sq = sum(x[2] for x in sel)
    if use_atan:
        return r_near(-Q * math.atan2(sq, si))
    return div_sym(-(sq << 10), si)


def ratio_mean(rounds, n):
    sel = rounds[-n:]
    return sum(qq / ii for ii, qq, iq in sel) / len(sel)


def ratio_sqrt_mean(rounds, n):
    sel = rounds[-n:]
    return (sum(math.sqrt(qq / ii) for ii, qq, iq in sel) / len(sel)) ** 2


def ratio_of_sums(rounds, n):
    sel = rounds[-n:]
    return sum(x[1] for x in sel) / sum(x[0] for x in sel)


def averaging_tests():
    exp_a = [ae for *_, ae, _ in POINTS]
    exp_b = [be for *_, be in POINTS]
    labels = [p[0] for p in POINTS]

    print()
    print("=" * 78)
    print("famiglia 'media su piu' coppie': coefficiente a")
    print("=" * 78)
    for lbl, fn in (("media atan2 per-round", lambda r, n: mean_angle(r, n, True)),
                    ("media iq/ii per-round", lambda r, n: mean_angle(r, n, False)),
                    ("atan2 delle somme", lambda r, n: angle_of_sums(r, n, True)),
                    ("divisione delle somme", lambda r, n: angle_of_sums(r, n, False))):
        for n in (2, 3, 4, 6):
            got = [fn(ROUNDS[k], n) for k in labels]
            ok = sum(g == e for g, e in zip(got, exp_a))
            print(f"  {ok}/4  {lbl:24} ultimi {n}: {got}")
    print(f"       atteso                             {exp_a}")

    print()
    print("=" * 78)
    print("famiglia 'media su piu' coppie': coefficiente b")
    print("=" * 78)
    for lbl, fn in (("media dei rapporti", ratio_mean),
                    ("media delle sqrt", ratio_sqrt_mean),
                    ("rapporto delle somme", ratio_of_sums)):
        for n in (2, 3, 4, 6):
            got = []
            ok = 0
            for k, (_, ii, qq, iq, ae, be) in zip(labels, POINTS):
                r = fn(ROUNDS[k], n)
                b = r_near(Q * math.sqrt(max(r - (ae / Q) ** 2, 0.0))) - UNITY
                got.append(hex(b & 0x3ff))
                if (b & 0x3ff) == be:
                    ok += 1
            print(f"  {ok}/4  {lbl:22} ultimi {n}: {got}")
    print(f"       atteso                           {[hex(b) for b in exp_b]}")

    print()
    print("=" * 78)
    print("bound esteso: la media puo' superare il rapporto delle somme?")
    print("=" * 78)
    print("  La media dei rapporti non e' il rapporto delle somme, quindi il bound")
    print("  calcolato sulle somme non copre da solo questa famiglia. Verifica")
    print("  diretta sul punto critico:")
    print()
    for lbl, ii, qq, iq, ae, be in POINTS:
        need = ((be + UNITY - 1) ** 2 + (be + UNITY) + ae * ae) / float(1 << 20)
        rr = [qq_ / ii_ for ii_, qq_, iq_ in ROUNDS[lbl]]
        cand = {}
        for n in (2, 3, 4, 6):
            cand[f"somme{n}"] = ratio_of_sums(ROUNDS[lbl], n)
            cand[f"media{n}"] = ratio_mean(ROUNDS[lbl], n)
        reach = [k for k, v in cand.items() if v >= need]
        print(f"  {lbl}: serve qq/ii >= {need:.9f}")
        print(f"     rapporti per-round: {' '.join('%.6f' % x for x in rr)}")
        print(f"     max per-round = {max(rr):.9f}   "
              f"combinazioni che raggiungono: {reach or 'NESSUNA'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    print("=" * 78)
    print("coefficiente a")
    print("=" * 78)
    for name, fn in A_VARIANTS.items():
        got = [fn(ii, qq, iq) for _, ii, qq, iq, _, _ in POINTS]
        exp = [ae for *_, ae, _ in POINTS]
        ok = sum(g == e for g, e in zip(got, exp))
        print(f"  {ok}/4  {name:32} {got}   atteso {exp}")

    print()
    print("=" * 78)
    print("coefficiente b (usando l'a corretto per ogni punto)")
    print("=" * 78)
    best = []
    for name, fn in B_VARIANTS.items():
        rules = [("--", isqrt_near)] if name in FLOAT_B else SQRTS.items()
        for rname, sq in rules:
            got = []
            for _, ii, qq, iq, ae, _ in POINTS:
                got.append(fn(ii, qq, iq, ae, sq) & 0x3ff)
            exp = [be for *_, be in POINTS]
            ok = sum(g == e for g, e in zip(got, exp))
            best.append((ok, name, rname, got))
    for ok, name, rname, got in sorted(best, reverse=True):
        exp = [be for *_, be in POINTS]
        mark = "  <== 4/4" if ok == 4 else ""
        print(f"  {ok}/4  sqrt={rname:6} {name:36} "
              f"{[hex(g) for g in got]}{mark}")
    print(f"       atteso                                             "
          f"{[hex(be) for *_, be in POINTS]}")

    print()
    print("=" * 78)
    print("bound: il punto e' raggiungibile da questi accumulatori?")
    print("=" * 78)
    print("  Il termine di ampiezza e' sqrt(qq/ii * 2^20 - correzione), con la")
    print("  correzione >= 0 in ogni variante. Quindi (qq<<20)/ii e' un limite")
    print("  SUPERIORE per v, e se b_atteso non e' raggiungibile con v massimo")
    print("  nessuna formula di questa famiglia puo' produrlo.")
    print()
    for lbl, ii, qq, iq, ae, be in POINTS:
        v_max = ((qq << 20) + (ii >> 1)) // ii
        root_needed = be + UNITY
        # v minimo perche' isqrt_near dia root_needed
        v_min_near = (root_needed - 1) ** 2 + root_needed
        v_min_floor = root_needed ** 2
        reach_near = v_max >= v_min_near
        reach_floor = v_max >= v_min_floor
        print(f"  {lbl}: v_max={v_max}  b_atteso=0x{be:x} (root {root_needed})")
        print(f"     serve v>={v_min_near} con sqrt 'near'  -> "
              f"{'OK' if reach_near else 'IRRAGGIUNGIBILE, manca %d' % (v_min_near - v_max)}")
        print(f"     serve v>={v_min_floor} con sqrt 'floor' -> "
              f"{'OK' if reach_floor else 'IRRAGGIUNGIBILE, manca %d' % (v_min_floor - v_max)}")

    averaging_tests()


if __name__ == '__main__':
    main()
