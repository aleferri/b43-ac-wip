# crsminpwr sul D6220 — specifica derivata dal blob di riferimento

Meccanismo delle soglie CRS minimum power per l'AC-PHY, ricavato dal blob
**D6220 / 7.14.89.14** e dalle catture del d6220: 43 segmenti a freddo e 44
`up`, con BSS impostato. Le misure marcate [MISURA] sono state rifatte su questo
set; dove il set precedente -- 26 configurazioni per due cicli -- diceva altro,
la voce lo dice. Quella struttura a coppie non c'e' piu': lo sweep a caldo di
oggi fa un ciclo per configurazione, quindi gli argomenti che poggiavano sul
confronto fra le due copie della stessa configurazione vanno riletti come
confronto fra il freddo e l'`up` dello stesso canale.

Ogni affermazione porta la sua fonte. Le tre categorie non sono
interscambiabili, e mescolarle e' il modo in cui si finisce a scrivere codice
giusto per un'altra board:

- **[BLOB]** letto dal binario di riferimento;
- **[MISURA]** verificato sulle 87 corse del set corrente;
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

**Non sono tre set: sono sempre tutti e otto.** Sul set corrente
`0x321`, `0x324`, `0x327`, `0x32a`, `0x32d`, `0x330`, `0x333` e `0x336`
ricevono lo stesso numero di scritture a ogni larghezza -- 156 per registro sui
50 segmenti a 20 MHz, 78 sui 25 a 40, 42 sui 13 a 80 -- e **portano la stessa
identica sequenza di valori**: su nessuno degli 87 segmenti la sequenza di uno
degli otto differisce da quella di `0x324`. La tabella a tre set veniva dal set
precedente e non regge qui.

I valori scritti sono voci della LUT della larghezza corrispondente, piu' il
valore **54**, che e' il riferimento di preinizializzazione `0x36`.

Qui il documento diceva che a 20 MHz compaiono anche 49, 52 e 64, che non sono
nella LUT, e li attribuiva a un secondo percorso -- la configurazione di canale,
che scrive gli stessi registri. **Il confronto era con la riga grezza**: con il
bump di 4 quei tre valori sono `LUT20[0]`, `[1]` e `[6]`, e sugli 87 segmenti del
set corrente ogni valore osservato a ogni larghezza e' una entry della propria
riga piu' 4, senza eccezioni (vedi `docs/bank-0910-analysis.md`). Che i percorsi
siano due resta possibile, ma non lo si decide dal valore: va deciso dal sito.

## Registri di delta dei core 1 e 2   [MISURA]

**Tutti e quattro** i registri `0x910`-`0x913`, con **entrambi** i campi byte
(`mask=0xff00` e `mask=0x00ff`), a **ogni** larghezza. Il blocco `0xb10`-`0xb13`
e' l'equivalente per il core 2.

**Ma i due byte non portano lo stesso valore**, e questa riga diceva di si'.
Sugli 87 segmenti, delle 582 coppie alto/basso della stessa scrittura ne
coincidono 456 e **126 no**: si vedono alto `0x00` con basso `0xfb`, alto `0xfb`
con basso `0xfe`, alto `0x05` con basso `0x03`. Sono due catene, e il caso in
cui divergono e' quello da spiegare -- finche' si leggevano come un valore solo
non si poneva. Per lo stesso motivo l'insieme dei valori qui sotto va letto
come l'unione dei due byte, non come un valore per scrittura.

Questo contraddice una specifica ricavata dal 6.30, che prevede solo
`0x910`/`0x912` e solo il byte alto a 20 MHz, ed entrambi i byte a 40. Sul d6220
la distinzione fra larghezze non esiste: otto scritture uguali sempre.

I valori sono interi piccoli con segno, per larghezza. Rimisurati sugli 87
segmenti, il campione e' piu' largo di quello di prima:

| larghezza | valori osservati |
|---|---|
| 20 MHz | -6, -5, -3, -2, -1, 0, +2, +3, +5 |
| 40 MHz | -4, -2, 0, +2, +4, +6 |
| 80 MHz | -4, -2, 0, +2 |

## La procedura: tre scritture, nessun gate   [MISURA]

Ogni ciclo, su ogni canale, scrive **tre volte** il registro del core 0:

    1.  il valore lasciato dal ciclo precedente   (stato riportato)
    2.  54 = 0x36                                 (riferimento, SEMPRE)
    3.  LUT_bw[idx]                               (risultato del calcolo)

Rifatto sugli 87 segmenti: delle terne `basso/alto/basso` su `0x0324` se ne
trovano 77, e il valore in posizione 2 e' **54 in tutte e 77**.

La posizione 1 e' lo stato riportato, e le due condizioni lo mostrano da sole:
su **tutti** i 43 segmenti a freddo il primo valore e' una costante per
larghezza -- 58 a 20 MHz su 25 segmenti, 60 a 40 su 12, 61 a 80 su 6 -- cioe' il
default che un attach trova; sui 44 `up` varia, perche' li' un ciclo precedente
c'e'.

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
| D6220 | 7.14.89 | 52, **54**, 57 | 57 = `LUT20[5]` (vedi la nota qui sotto) |
| agcombo | 7.14.43 | 58, **54**, 58 | 58 = indice 5 della sua riga |
| DSL-3580L | 6.30 | **54**, 48 / 45 | 48 = `LUT20_dsl[1]`, 45 = `[0]` |

Tre versioni con righe diverse producono tre valori diversi, ognuno nella
**propria** riga: la conferma non poggia su una sola board. E `54` compare su
tutte tre, quindi e' una costante universale e non un default del d6220.

**La riga del d6220 va ricavata di nuovo.** Sul set corrente il valore finale di
`0x324` a freddo non e' uno: a 20 MHz e' 52 su 8 segmenti, 57 su 8, 58 su 7 e 55
su 2; a 40 MHz 54 su 6, 58 su 4, 60 su 2; a 80 MHz 52 su 4, 50 e 56 su uno
ciascuno. Di questi, 57 e 60 sono voci di `LUT20`, 54 e 58 lo sono di `LUT40`,
50 e 52 di `LUT80`, mentre 55 e 58 a 20 MHz non stanno in nessuna riga -- cioe'
il campione largo mette insieme il risultato del calcolo e i valori dell'altro
percorso, e separarli vuole il confronto per sito, non per segmento. Con 26
configurazioni e un solo canale per riga la distinzione non si poneva.

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

Sui 44 segmenti `up` del set corrente il PRIMO write del byte basso ri-applica
il valore FINALE del segmento precedente in **26 giunzioni su 43**; il secondo
write e' la valutazione fresca. Sul set precedente erano 45 su 51, con le sei
rotture tutte su un cambio di freq-range o di larghezza. Oggi le rotture sono
17 e i cambi di larghezza o di range ne spiegano solo una parte: restano
ch116->ch120, ch128->ch132, ch132->ch136, ch136->ch140, ch140->ch144 a 20 MHz,
dentro lo stesso range, e sono canali DFS, cioe' quelli che la ricattura ha
ripreso con il CAC completato. Prima di leggerci uno slot per (larghezza,
freq_range) va stabilito se quelle giunzioni siano giunzioni davvero o una
ripartenza dell'interfaccia dentro lo sweep: la persistenza si misura sulle
altre 26.

Lettura, con quella riserva: lo stato CRS e' persistito per slot (larghezza,
freq_range) -- il cambio di slot carica il valore di quello slot, non un default
globale. Il freddo la sostiene per un'altra via: li' non c'e' nessuno stato da
riportare e il primo valore e' il default della larghezza, uno solo per tutte e
tre.

Conseguenza per la ricerca dell'istruzione `row[idx]`: l'indice e' quasi
certamente LETTO dall'entry (lb) e ristretto/steppato, non ricalcolato da zero: i
valori osservati cambiano raramente e per passi, con isteresi. Vincoli che
il codice trovato deve soddisfare: indici osservati {4, 5, 7} nella
propria riga per-BW; differenze di offset o20-o40=1, o40-o80=3;
l'aggiustamento +7 sulla soglia a BW80 gia' tracciato.

## Chiusura end-to-end   [TODO descrizione]
