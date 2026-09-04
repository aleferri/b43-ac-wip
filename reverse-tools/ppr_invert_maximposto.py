"""Inversione col massimo imposto: resta solo il tetto di gruppo.

    campo[i] = (M - cp[i]) / 2     cp[i] = min(ppr[i], tetto)
    M        = maxp5ga[sb] - 2 * min(nib)      (SROM, non tagliato)
    ppr[i]   = maxp5ga[sb] - 2 * nib[i]

cp e' determinato dall'osservazione: cp[i] = M - 2*campo[i]. Il tetto segue --
dove cp coincide con ppr non morde, dove e' minore vale cp, e i rate tagliati
devono condividerlo. Nessuna enumerazione, nessun parametro libero.
"""
MAP_MCS = [0, 0, 0, 0, 1, 2, 3, 4]


def nib(po, i):
    return (po >> (4 * i)) & 0xf


def risolvi(campi, maxp, po):
    ppr = [maxp - 2 * nib(po, MAP_MCS[i]) for i in range(8)]
    m = maxp - 2 * min(nib(po, i) for i in range(8))
    cp = [m - 2 * c for c in campi]
    tagliati = [i for i in range(8) if cp[i] != ppr[i]]
    if not tagliati:
        return m, None, True                       # nessun taglio
    if any(cp[i] > ppr[i] for i in tagliati):
        return m, None, False                      # taglio verso l'alto: assurdo
    valori = {cp[i] for i in tagliati}
    if len(valori) != 1:
        return m, sorted(valori), False            # tagliati discordi
    t = valori.pop()
    ok = all(cp[i] == min(ppr[i], t) for i in range(8))
    return m, t, ok
