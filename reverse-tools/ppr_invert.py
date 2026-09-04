#!/usr/bin/env python3
"""Inverte la catena PPR su un segmento, enumerando i tetti compatibili.

    campo[i] = (max(cp) - cp[i]) / 2      cp[i] = min(ppr[i], tetto)
    ppr[i]   = maxp5ga[sb] - 2 * nib[i]

Il massimo non e' un'incognita libera: dato il tetto e' max(cp). Quindi si
enumera il solo tetto e si tiene ogni valore che riproduce gli otto campi. Se
ne resta uno il punto e' determinato; se ne restano molti il punto e'
degenere e vincola solo una relazione -- succede quando il tetto appiattisce
tutti i rate, e allora i dati fissano soltanto max - tetto.
"""

MAP_MCS = [0, 0, 0, 0, 1, 2, 3, 4]   # 6,9,12,18 -> mcs0; 24,36,48,54 -> mcs1..4


def ppr_rates(maxp, po):
    """I ppr degli otto rate OFDM legacy. Nibble senza segno."""
    return [maxp - 2 * ((po >> (4 * MAP_MCS[i])) & 0xf) for i in range(8)]


def tetti_compatibili(campi, maxp, po, lo=32, hi=132):
    """Ogni tetto che riproduce i campi osservati. None = nessun tetto."""
    ppr = ppr_rates(maxp, po)
    out = []
    for tetto in [None] + list(range(lo, hi + 1, 2)):
        cp = ppr if tetto is None else [min(p, tetto) for p in ppr]
        m = max(cp)
        if all((m - cp[i]) % 2 == 0 and (m - cp[i]) // 2 == campi[i]
               for i in range(8)):
            out.append(tetto)
    return ppr, out


def descrivi(campi, maxp, po):
    ppr, ts = tetti_compatibili(campi, maxp, po)
    if not ts:
        return ppr, 'incoerente', None, None
    # "nessun tetto" e ogni tetto >= max(ppr) sono lo stesso caso.
    effettivi = sorted({t for t in ts if t is not None and t < max(ppr)})
    senza = None in ts or any(t is None or t >= max(ppr) for t in ts)
    if effettivi and not senza:
        stato = 'determinato' if len(effettivi) == 1 else 'degenere'
        return ppr, stato, effettivi, max(cp for cp in
                                          [min(p, effettivi[0]) for p in ppr])
    if senza and not effettivi:
        return ppr, 'determinato', [None], max(ppr)
    return ppr, 'degenere', ([None] + effettivi), None
