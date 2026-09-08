# I test

Due suite, che misurano cose diverse e non si sostituiscono.

## `unit/` — le parti, una per una

Quella che c'e' da sempre. L'harness impersona b43: `unit/main.c` chiama i
punti d'ingresso del PHY nell'ordine che decide lui, con `unit/stubs/b43.h`
che finge le strutture, e confronta l'output con una cattura del vendor.

E' la suite che produce il numero citabile, e misura **il driver PHY**: quanto
di `src/phy_ac.c`, `radio_2069.c`, `tables_phy_ac.c` e `rxiqcal_phy_ac.c`
riproduce il vendor. Vedi `unit/README.md`.

Il suo limite e' nella sua forma: le op che b43 emette da altrove -- `MAC.*`,
gli `OBJ.*` del core, `TPL.RAMW`, la address match table -- l'harness non le
puo' emettere, quindi stanno nel perimetro di `unit/compare.py` e non nel
denominatore. Il `100.00%` e' su cio' che il PHY deve fare, non su cio' che il
driver deve fare.

## `integration/` — b43 intero su AC

Lo step successivo, e la ragione per cui esiste: in b43 l'ordine degli eventi
non si legge. `main.c` ha 42 gate su `core_rev`, `fw.rev`, `phy->type` e
`phy->rev`, con rami `else` liberi, e i quattro punti d'ingresso del PHY sono
chiamati da `phy_common.c:83` dentro `b43_phy_init()`, che a sua volta e'
chiamata da `b43_chip_init()`, che e' chiamata da `b43_wireless_core_init()`.
Ricostruire quella sequenza leggendo produce errori; eseguirla no.

Quindi qui non si impersona b43: si compilano `b43/main.c` e
`b43/phy_common.c` **veri** e si stubba il livello sotto, cosi' il driver
guida se stesso, gate ed `else` compresi, e la traccia e' quella di b43
intero.

Il guadagno atteso non e' un punteggio piu' alto, e' un denominatore piu'
onesto: le op che oggi sono fuori perimetro perche' l'harness non le puo'
emettere diventano emettibili, e il perimetro si accorcia a cio' che b43
davvero non fa.

Vedi `integration/README.md` per lo stato e i traguardi.
