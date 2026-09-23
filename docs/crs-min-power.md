# Soglie CRS min-power e banco `0x0910`

Il meccanismo che programma la soglia di carrier sense dell'AC-PHY: una soglia
comune sugli otto registri CRS, e per ogni catena oltre la 0 un offset nel
banco `0x0910`. Nel port sta in `src/phy_ac.c`, da `b43_phy_ac_crs_ladder[]` a
`b43_phy_ac_prog_bank_0910()`.

Ogni affermazione porta la sua fonte, e le categorie non sono intercambiabili:

- **[BLOB]** letto dal binario di riferimento, D6220 7.14.89.14;
- **[MISURA]** verificato sulle catture: d6220 a freddo (43 segmenti) e a caldo
  (44 `up`), agcombo a freddo (26) e a caldo (26);
- **[APERTO]** non stabilito, da non implementare come se lo fosse.

## Cosa si scrive   [MISURA]

**La soglia comune.** Il byte basso di `0x0321`, `0x0324`, `0x0327`, `0x032a`,
`0x032d`, `0x0330`, `0x0333`, `0x0336` -- le soglie del clip detector, passo
`0xc` e interlacciate fra le catene. Gli otto portano sempre la stessa sequenza
di valori.

**Il banco per catena.** Quattro registri per catena a passo `0x200`: `0x0910`-
`0x0913` per la catena 1, `0x0b10`-`0x0b13` per la 2. `0x0710`, che sarebbe la
catena 0, non compare in nessuna cattura: i banchi sono uno per ogni catena
cablata oltre la 0, quindi uno sul d6220 (tre core, due catene) e due
sull'agcombo. Otto RMW per banco, in un ordine che non cambia mai:

    reg+0 hi, reg+0 lo,  reg+2 hi, reg+2 lo,
    reg+1 lo, reg+1 hi,  reg+3 lo, reg+3 hi

Il valore e' un byte con segno (`0xfb` e' -5), lo stesso in entrambi i byte di
tutti e quattro i registri: 1356 coppie alto/basso sui quattro sweep, nessuna
eccezione. Il banco e' write-only.

## La scala   [BLOB]

`.rodata` del D6220, tre righe di 15 voci u8 terminate da zero, passo 16:

| offset | larghezza | voci |
|---|---|---|
| `+0x40e84` | 20 MHz | 45 48 51 53 54 57 60 63 66 68 70 72 75 78 80 |
| `+0x40e94` | 40 MHz | 44 46 48 50 52 54 56 58 60 63 66 69 71 74 76 |
| `+0x40ea4` | 80 MHz | 41 44 46 48 50 52 55 57 60 63 65 68 70 72 74 |

Le tre righe sono referenziate da una sola funzione,
`wlc_phy_crs_min_pwr_cal_acphy`. L'agcombo (7.14.43) usa le stesse: ogni suo
valore, soglia o banco, e' una voce di queste righe [MISURA].

Nel blob del DSL-3580L (6.30.102.7) le righe stanno a `0x1fa19c`, `0x1fa1a8`,
`0x1fa1b4`, passo 12 e undici voci, e i valori differiscono: la riga a 40 MHz e'
identica, quella a 20 per due voci, quella a 80 per nove. Il port usa quella del
D6220 per ogni board.

## Il campione   [MISURA]

Il watchdog latcha la finestra SHM `0x0308`-`0x0314`: una cella da 32 bit per
catena, `0x0308` la 0, `0x030c` la 1, `0x0310` la 2. Sui 4352 la terza vale zero,
sull'agcombo no. Le parole alte sono sempre zero e `0x0314` vale sempre `0x45`.

Il campione e' una potenza integrata sul canale e scala con la banda: mediana
1384 a 20 MHz, 2525 a 40, 3832 a 80.

## La regola   [MISURA]

Per ogni catena e per ogni latch:

    n      = campione >> passo di banda           (0 a 20 MHz, 1 a 40, 2 a 80)
    v      = quante soglie {1024, 1536, 2048, 3072, 4096} n raggiunge
    indice = {0, 1, 3, 4, 6, 7}[v] + ancora         (0 a 20 MHz, +2 a 40 e 80)

L'indice va in un ring di quattro per catena, e l'indice in forza e' la media
del ring **arrotondata per eccesso**. Poi:

    soglia comune   = riga[indice catena 0] + 4 a freddo
    banco catena c  = riga[indice catena c] - riga[indice catena 0]

Il bump di 4 vale per le prime due calibrazioni della sessione e si elide nel
banco. La somma `soglia + banco` e' la soglia della catena c: il vendor scrive la
comune per tutte e la correzione per le altre, e l'hardware somma.

Gli indici del singolo campione hanno dei buchi, 2 e 5, e li riempie la media:
55 a 20 MHz e 56 a 40 compaiono solo dietro una finestra in cui il rumore sta a
cavallo di 1536.

### Verifica

Su ogni blocco CRS che abbia almeno un campione davanti:

| cattura | soglie comuni | valori del banco |
|---|---|---|
| d6220 freddo | 63/63 | 63/63 |
| d6220 caldo | 39/39 | 39/39 |
| agcombo freddo | 13/13 | 26/26 |
| agcombo caldo | 10/10 | 20/20 |

La regola e' stata trovata sul freddo del d6220; caldo e agcombo sono serviti da
verifica, salvo le due voci piu' alte (qui sotto). Sul freddo del d6220 la
finestra e' netta: con 3, 5 o 6 campioni si sbagliano da 3 a 10 indici su 126;
la regola precedente -- soglie 1020/1526/2100 sul solo ultimo campione -- ne
sbagliava 18.

Quanto e' vincolato ogni soglia, dai soli blocchi il cui ring contiene un
campione, cioe' dove un campione decide l'indice da solo:

| soglia | piu' alto sotto | piu' basso sopra |
|---|---|---|
| 1024 | 1001 | 1025 |
| 1536 | 1535 | 1538 |
| 2048 | 2047 | 2049 |
| 3072 | 2914 | 3147 |
| 4096 | 3907 | 4279 |

- la seconda e la terza sono inchiodate a tre unita', la prima a una da sopra;
- la voce 4096 -> 7 poggia su un solo campione sopra, 4279 dell'agcombo a
  caldo;
- sopra 4096 nessuna cattura dice come continui. **SALAME**: le soglie cadono
  sulle mezze ottave e gli indici salgono di tre per ottava, cioe' circa uno
  per dB, e la scala proseguirebbe cosi'.

I livelli che le catture percorrono, per larghezza, sono in
`b43_phy_ac_crs_noise_seen[]`: fuori di li' il driver avvisa una volta.

## Dove si scrive   [MISURA]

- **`chanspec_tail()`**, dentro il channel setup. Al primo bring-up la soglia e'
  un letterale per larghezza, 58, 60 e 61 -- cioe' `LUT20[4]`, `LUT40[6]`,
  `LUT80[7]` piu' 4 -- e il banco e' zero perche' i ring sono vuoti.
- **Il blocco E**, subito dopo il latch di un campione: la soglia comune e il
  banco dalla regola.
- **`pwork_60sec()`**, solo la soglia comune e solo se cambia.

## Aperto

**Quando il vendor emette il blocco E.** Il valore torna ovunque i due lati
scrivono nello stesso punto; il punto no. Sui 43 segmenti a freddo la sequenza
CRS+banco del port coincide con quella del vendor su 31. Negli altri 12:

| segmenti | differenza |
|---|---|
| cold12, cold13, cold16 | il vendor scrive dopo 26, 53 e 21 latch, il port dopo il primo |
| cold07, cold09, cold28, cold35 | il vendor ha un blocco in piu' in mezzo |
| cold17, cold18 | il port ha un blocco in piu' |
| cold14, cold15, cold32 | radar-meteo, cattura vecchia: il vendor ha solo il primo blocco |

Sono tutti canali con la guardia radar. L'ultimo blocco del vendor cade 150-170
ms dopo il bss-up; gli altri non hanno un evento della timeline che li preceda
sempre.

**Il reset dei ring.** Il port li svuota al cambio di sotto-banda. Il freddo non
dice niente, perche' ogni segmento e' un caricamento. Sul caldo il primo blocco
di un `up` non si spiega ne' con i ring vuoti ne' con i ring del segmento
precedente, e in mezzo c'e' quello che sta in `hot-sweep.zip!hot-gaps/`.

**Il DSL-3580L.** Con la 6.30 scrive il banco sempre a zero, su tutte le
occorrenze, e ha una scala propria. Non e' verificato se la 6.30 abbia il filtro.

## Come e' stato trovato, e cosa non era

Il banco e' stato cercato a lungo come un valore in un registro, per
uguaglianza. Le ipotesi cadute:

| ipotesi | perche' e' caduta |
|---|---|
| `0x0c02 >> 10` da tabella 7 | combacia su una occorrenza, poi il valore non esiste in tutta la cattura |
| `b / 10` dai coefficienti RX IQ | correlazione temporale: banco e coefficienti partono da zero e crescono insieme |
| base index idle-TSSI | non combacia |
| coppie per-core `0x06xx`/`0x08xx` vicine al banco | nessuna da' il valore |
| campi SROM per-core | identici su tre board mentre il valore no |
| `X[c] - X[0]` su un registro per-catena | 575 indirizzi provati sull'agcombo, nessuno |
| `target - crs` con target per chip e per sito | vale su ch36 a 20 MHz; sul set intero la somma non e' costante in nessuna larghezza tranne 80 MHz |

L'ultima era la forma implementata fino a questa versione. La chiave e' stata
vedere che la finestra di rumore ha una cella per catena, e confrontare la
catena 1 con la somma `soglia + banco` invece che il banco da solo.

### Come si trova e si attribuisce la scala nel blob

La scala e' l'initializer di un array locale, quindi `.rodata` anonima: nessun
simbolo, e un'enumerazione dei simboli `OBJECT` non la raggiunge per
costruzione. Si trova per forma -- monotona, passi in 2..8, nessun tratto a passo
1, che e' il vincolo che elimina le liste di indici -- e restano 8 candidati u8:
le tre righe, `acphy_tx_evm_tbl_rev0`, `lpphy_rev2_gain_table` e tre altre.

Si attribuisce senza disassemblare, dalle rilocazioni: su MIPS un indirizzo si
carica con `lui %hi` + `addiu %lo`, cioe' una coppia `R_MIPS_HI16`/`R_MIPS_LO16`
sullo stesso simbolo in `.rel.text`. Ricostruito l'indirizzo, il simbolo `FUNC`
che contiene la rilocazione e' la funzione che lo carica. Sul blob sono 8537
indirizzi ricostruiti; le tre righe risultano di `wlc_phy_crs_min_pwr_cal_acphy`,
e l'altro candidato anonimo, `+0x051b58`, di `wlc_phy_elna_gainctrl_workaround`.

Per array piccoli GCC puo' materializzare l'initializer come store immediati:
prima di dichiarare assente una tabella vanno cercati anche gli immediati.

### Il resto del blob sulla calibrazione   [BLOB]

Le stringhe di dump nominano il rumore usato per le soglie, un contatore di
esecuzioni per canale e la condizione "ACI desense attivo, la cal non gira". Ci
sono `wlc_phy_set_crs_min_pwr_higain_acphy`, `wlc_phy_force_crsmin_acphy` con
l'iovar `phy_force_crsmin`, e `wlc_phy_noise_sample_request_crsmincal`.

La parametrizzazione e' in NVRAM, e su nessuna delle board e' impostata, quindi
il driver usa i default compilati:

    noise_cal_enable_{2g,5g}        noise_cal_ref_{2g,5g}
    noise_cal_ref_40_{2g,5g}        noise_cal_adj_{2g,5g}
    noise_cal_po_{2g,5g}            noise_cal_po_40_{2g,5g}
    noise_cal_po_bias_{2g,5g}       noise_cal_nf_substract_val[_{2g,5g}]
    noise_cal_high_gain[_{2g,5g}]   noise_cal_deltamin / noise_cal_deltamax
    noise_cal_update                noise_cal_dbg

### Precedenti

In N-PHY la stessa famiglia di registri esiste con un nome, `CRSMINPOWER0/1`,
`CRSMINPOWERL0/L1`, `CRSMINPOWERU0` in `b43/phy_n.h`, ma brcmsmac ci scrive solo
una costante di workaround per spurie (`NPHY_ADJUSTED_MINCRSPOWER`), e
`noise_crsminpwr_index` e' dichiarato e mai usato: la parte adattiva e' stata
tolta dal rilascio open-source. ath9k ha la stessa grandezza come `minCCApwr`,
con un filtro per catena su una finestra di cinque campioni (mediana), che e'
lo stesso disegno con un'altra statistica.

Il nome del simbolo resta per indirizzo, `prog_bank_0910`, finche' la semantica
non e' confermata sul chip.
