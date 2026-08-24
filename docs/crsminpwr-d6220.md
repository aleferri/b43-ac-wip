# crsminpwr sul D6220 — specifica derivata dal blob di riferimento

Meccanismo delle soglie CRS minimum power per l'AC-PHY, ricavato dal blob
**D6220 / 7.14.89.14** e da 52 segmenti di cattura (26 configurazioni x 2 cicli,
con BSS impostato).

Ogni affermazione porta la sua fonte. Le tre categorie non sono
interscambiabili, e mescolarle e' il modo in cui si finisce a scrivere codice
giusto per un'altra board:

- **[BLOB]** letto dal binario di riferimento;
- **[MISURA]** verificato sulle 52 corse;
- **[APERTO]** non stabilito -- da non implementare come se lo fosse.

## Le tre LUT   [BLOB]

`.rodata` del D6220, tre righe di **15 voci** u8 terminate da zero:

| offset | larghezza | voci |
|---|---|---|
| `+0x40e84` | 20 MHz | 45 48 51 53 54 57 60 63 66 68 70 72 75 78 80 |
| `+0x40e94` | 40 MHz | 44 46 48 50 52 54 56 58 60 63 66 69 71 74 76 |
| `+0x40ea4` | 80 MHz | 41 44 46 48 50 52 55 57 60 63 65 68 70 72 74 |

Sono magnitudini in dB. Il passo fra le righe e' 16 byte.

**Attenzione alla versione.** Nel blob DSL (6.30.102.7) le stesse tre righe
stanno a `0x1fa19c`, `0x1fa1a8`, `0x1fa1b4` -- passo **12**, non 16 -- e i
valori
differiscono: la riga 40 MHz e' identica, quella a 20 MHz per due voci, quella a
80 MHz per nove. Una specifica ricavata dal 6.30 va bene per la struttura e NON
per le costanti.

## Registri del core 0, per larghezza   [MISURA]

Tre set distinti, verificati su tutte le 52 corse:

| larghezza | registri scritti |
|---|---|
| 20 MHz | `0x324`, `0x330` |
| 40 MHz | `0x321`, `0x324`, `0x32d`, `0x330` |
| 80 MHz | `0x321`, `0x324`, `0x327`, `0x32a`, `0x32d`, `0x330`, `0x333`, `0x336` |

I valori scritti sono voci della LUT della larghezza corrispondente, piu' il
valore **54**, che e' il riferimento di preinizializzazione `0x36`.

A 20 MHz compaiono anche 49, 52 e 64, che **non** sono nella LUT: vengono da un
altro percorso, la configurazione di canale, che scrive gli stessi registri.
Due meccanismi sugli stessi indirizzi.

## Registri di delta dei core 1 e 2   [MISURA]

**Tutti e quattro** i registri `0x910`-`0x913`, con **entrambi** i campi byte
(`mask=0xff00` e `mask=0x00ff`), a **ogni** larghezza, con lo stesso valore.
Il blocco `0xb10`-`0xb13` e' l'equivalente per il core 2.

Questo contraddice una specifica ricavata dal 6.30, che prevede solo
`0x910`/`0x912` e solo il byte alto a 20 MHz, ed entrambi i byte a 40. Sul d6220
la distinzione fra larghezze non esiste: otto scritture uguali sempre.

I valori sono interi piccoli con segno, per larghezza:

| larghezza | valori osservati |
|---|---|
| 20 MHz | 0, +5, -5, -7 |
| 40 MHz | 0, +2, +4, +6, -4 |
| 80 MHz | 0, +2, +5 |

## La procedura: tre scritture, nessun gate   [MISURA]

Ogni ciclo, su ogni canale, scrive **tre volte** il registro del core 0:

    1.  il valore lasciato dal ciclo precedente   (stato riportato)
    2.  54 = 0x36                                 (riferimento, SEMPRE)
    3.  LUT_bw[idx]                               (risultato del calcolo)

Verificato su 52 corse: il valore in posizione 2 e' **54 in tutte**, e il primo
valore del ciclo B coincide con l'ultimo del ciclo A in **26 configurazioni su
26**.

**Non esiste un gate** che decida se aggiornare. Quello che sembrava tale --
canali dove il banco "resta a zero" -- era un artefatto di lettura: guardando la
sequenza come un INSIEME, {52, 54} sembra "nessun aggiornamento" e {52, 54, 57}
sembra "aggiornato", mentre sono la stessa procedura con risultato diverso. Lo
stesso vale per l'asimmetria fra i due cicli, che e' soltanto lo stato
riportato.

Ricerca esaustiva a conferma: nessuna delle **403** chiavi (op, indirizzo) con
valore leggibile nel segmento separa i due gruppi, a nessuna larghezza. Se ci
fosse una soglia osservabile, sarebbe emersa.

## Il risultato e' una voce della LUT, per board   [MISURA, tre board]

| board | versione | scritture a `0x324` | voce |
|---|---|---|---|
| D6220 | 7.14.89 | 52, **54**, 57 | 57 = `LUT20[5]` |
| agcombo | 7.14.43 | 58, **54**, 58 | 58 = indice 5 della sua riga |
| DSL-3580L | 6.30 | **54**, 48 / 45 | 48 = `LUT20_dsl[1]`, 45 = `[0]` |

Tre versioni con righe diverse producono tre valori diversi, ognuno nella
**propria** riga: la conferma non poggia su una sola board. E `54` compare su
tutte tre, quindi e' una costante universale e non un default del d6220.

**I valori estranei alla riga vengono da un altro percorso.** Sul d6220 a 20 MHz
compaiono 49, 52 e 64: `49` e `64` non sono in nessuna riga, e `52` e'
`LUT40[4]`
e `LUT80[5]` ma non una voce a 20 MHz. Su DSL e agcombo non compaiono affatto.
Sono la configurazione di canale, che scrive gli stessi registri.

## Gli offset per larghezza: solo le differenze   [MISURA]

All'indietro dagli indici osservati, con `idx = (v+1) + offset`:

| larghezza | risultato | indice | v implicata (offset 34/33/30) |
|---|---|---|---|
| 20 MHz | 57 | 5 | -30 |
| 40 MHz | 52 | 4 | -30 |
| 40 MHz | 58 | 7 | -27 |
| 80 MHz | 50 | 4 | -27 |
| 80 MHz | 52 | 5 | -26 |

Una singola `v` spiega due larghezze: `-30` da' 57 a 20 MHz e 52 a 40, `-27` da'
58 a 40 e 50 a 80.

**Ma il test NON individua gli offset.** Perturbandoli su tutte le 343 terne in
+-3, **27** fanno almeno altrettanto bene e nessuna fa meglio. I dati
determinano
le **differenze** -- `o20 - o40 = 1` e `o40 - o80 = 3` -- non i valori assoluti,
che spostano tutte le `v` della stessa quantita' e non sono osservabili senza
conoscere `v` per altra via.

Accettando (34, 33, 30) da fonte esterna, le `v` valgono -30/-27/-26. Senza
quell'ancoraggio si sa solo che sono tre valori a distanza 0, +3, +4.

v deve essere un parametro che è deciso dal caller e cambia quindi comportamento.

**Il valore assoluto degli offset**, per la ragione sopra: i dati fissano le
differenze, non l'ancoraggio.



## Nota su brcmsmac

`wlc_phy_adjust_crsminpwr_nphy` in `brcm80211/brcmsmac/phy/phy_n.c` scrive lo
stesso valore a tre registri (`0x27d`, `0x280`, `0x283`, byte basso) salvando i
precedenti in `nphy_crsminpwr[3]`, e il valore e' la **costante**
`NPHY_ADJUSTED_MINCRSPOWER`, non una misura: e' un aggiramento di spurie
condizionato a `gband_spurwar_en` piu' canale e larghezza.

Questo spiega perche' i tentativi di correlare il rumore misurato col valore del
banco siano falliti: in N-PHY la parte misurata (`noise_var`) e la soglia
(`crsminpwr`) sono due meccanismi separati. Il codice e' GPL-2.0 come b43,
quindi
utilizzabile e non solo consultabile.

## Persistenza del valore e stato per-slot   [MISURA]

Sui 52 segmenti dello sweep, il PRIMO write del byte basso di ogni up
ri-applica il valore FINALE del ciclo precedente in 45 giunzioni su 51; il
secondo write e' la valutazione fresca. Le 6 giunzioni che rompono la
catena cadono tutte su un cambio di freq-range o di larghezza:
ch48->ch52 (0x31), ch64->ch100 (0x31), BW20->BW40 (0x36),
ch60->ch100 BW40 (0x3a), ch36->ch52 BW80 (0x36), ch52->ch100 BW80 (0x36).

Lettura: lo stato CRS e' persistito per slot (larghezza, freq_range) — il
cambio di slot carica il valore di quello slot, non un default globale.

Conseguenza per la ricerca dell'istruzione `row[idx]`: l'indice e' quasi
certamente LETTO dall'entry (lb) e ristretto/steppato, non ricalcolato da zero: i
valori osservati cambiano raramente e per passi, con isteresi. Vincoli che
il codice trovato deve soddisfare: indici osservati {4, 5, 7} nella
propria riga per-BW; differenze di offset o20-o40=1, o40-o80=3;
l'aggiustamento +7 sulla soglia a BW80 gia' tracciato.

## Chiusura end-to-end   [TODO descrizione]
