#!/usr/bin/env python3
"""Toglie da una cattura l'attach dell'altro core wireless.

`wl` al caricamento fa l'attach di *tutti* i core, non solo di quello sotto
esame, e ogni segmento degli sweep contiene un modulo ricaricato. Quindi la
testa di ogni segmento ha due attach di seguito: prima quello di wl0 --
l'N-PHY 2.4 GHz -- e poi quello di wl1, che e' l'AC.

Il danno non e' nel confronto, che parte dalla prima op PHY dell'attach AC e
quel prefisso lo salta gia'. E' nell'**oracolo**, che parte dall'insmod per
coprire l'OTP e il probe dei core, e cosi' si mangia anche le letture di wl0:
le code per (classe, indirizzo) risultano sfasate e il driver sotto esame
legge i valori della shared memory dell'*altro* core. Sulle celle 0x0000 e
0x0002 -- UCODEREV e UCODEPATCH -- si vede: entrambi i core eseguono il
self-test di lettura/scrittura, e senza questo filtro le prime tre letture
servite sono quelle di wl0.

Il confine non e' `cpu`. La prima op dell'attach di wl1 e' su cpu0 come tutto
il prefisso, ed e' del lavoro di wl1 che compare su cpu0 piu' avanti nei
segmenti (il blocco GPIO frontend, su cold05). Il confine e' la coppia
OTP.RDR/OTP.INIT: ogni attach rilegge la SPROM, quindi ce ne sono due, e
l'attach da tenere comincia dalla seconda.

La verifica che rende questo un taglio misurato e non una congettura sono i
cinque registri che `b43/phy_n.h` nomina RFCTL_CMD (0x078), AFECTL_OVER1
(0x08f), AFECTL_OVER (0x0a5), AFECTL_C1 (0x0a6) e AFECTL_C2 (0x0a7): sono
scritti solo dall'N-PHY, compaiono esattamente due volte ciascuno in un
segmento, e devono cadere tutti dentro la regione tagliata. Se non ci cadono
il filtro esce con errore, perche' vuol dire che wl0 fa qualcosa fuori dal
prefisso e un taglio di prefisso non basta.

Le catture in router-data/ non si toccano: questo produce un file derivato,
come fa trace_filter.py.

Uso: strip_other_core.py <cattura> [out]     (senza out, su stdout)
"""
import re
import sys

OPNUM = re.compile(r'#(\d+)\s')
FOR = re.compile(r'\bfor=#(\d+)\b')
NPHY_ONLY = (0x078, 0x08f, 0x0a5, 0x0a6, 0x0a7)
NPHY_RE = re.compile(r'\bPHY\.\S+\s+addr=0x0*(%s)\b' %
                     '|'.join('%03x' % a for a in NPHY_ONLY))


def opnum(line):
    m = OPNUM.search(line)
    return int(m.group(1)) if m else None


def main(argv):
    if not 2 <= len(argv) <= 3:
        sys.exit(__doc__)
    lines = open(argv[1]).read().splitlines(True)

    otp = [opnum(l) for l in lines if 'OTP.RDR' in l]
    otp = [n for n in otp if n is not None]
    if len(otp) < 2:
        sys.exit('%s: %d OTP.RDR, ne servono almeno 2: non c\'e\' un secondo '
                 'attach da cui partire' % (argv[1], len(otp)))
    first, second = otp[0], otp[1]

    nphy = [opnum(l) for l in lines if NPHY_RE.search(l)]
    outside = [n for n in nphy if n is None or not first <= n < second]
    if outside:
        sys.exit('%s: firma N-PHY fuori dalla regione [%d,%d): op %s. '
                 'wl0 tocca il PHY anche fuori dal prefisso, un taglio di '
                 'prefisso non basta.' % (argv[1], first, second,
                                          ', '.join(map(str, outside))))
    if not nphy:
        sys.exit('%s: nessuna firma N-PHY. Il prefisso non e\' identificabile '
                 'come attach dell\'altro core.' % argv[1])

    kept, dropped = [], 0
    for line in lines:
        n = opnum(line)
        ref = FOR.search(line)
        owner = int(ref.group(1)) if ref else n
        if owner is not None and first <= owner < second:
            dropped += 1
            continue
        kept.append(line)

    out = open(argv[2], 'w') if len(argv) == 3 else sys.stdout
    out.writelines(kept)
    if len(argv) == 3:
        out.close()
    print('%s: attach altro core [#%d,#%d), %d righe tolte, %d firme N-PHY'
          % (argv[1].split('/')[-1], first, second, dropped, len(nphy)),
          file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv)
