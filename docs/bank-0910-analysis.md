# Il banco `0x0910`: reperti, precedenti, e cosa serve per chiuderlo

Otto scritture per catena che il driver stock emette in coda al channel
programming. Il valore era il residuo non spiegato dei due gate `full` e
`switch_channel` su d6220 ch36 BW20: 8 op sul primo bring-up, 16 sul successivo.

**Risolto per la configurazione validata.** Il banco porta un **offset sulla
scala della soglia CRS** scritta immediatamente prima: la quantità con
significato è la somma `crs + off`, cioè una soglia assoluta, e l'offset è
`max(0, target − crs)`. Con `target` per sito di chiamata e per chip il banco
combacia op-per-op su entrambi i gate. La derivazione è in §4.

Il §5 identifica cosa sono quelle soglie: **entry di un ladder discreto**,
trovato nel blob D6220 a `.rodata +0x040e84` e attribuito a
`wlc_phy_crs_min_pwr_cal_acphy`. La cal misura una potenza di rumore e ne
ricava un **indice** nella riga.

**Lo sweep chiude la questione dell'indice, in senso negativo: non e' una
funzione del canale.** Le 32 osservazioni su 16 canali BW20 danno il ladder
completo, `{49, 52, 57, 64}`, e mostrano che il valore finale **cambia fra le
due corse dello stesso canale** su ch100, ch108 e ch116 — rispettivamente
`[57, 52]`, `[57, 52]` e `[52, 57]`. I canali bassi finiscono sempre su 52 e
ch132-140 sempre su 57; quelli in mezzo oscillano fra i due gradini.

E' la firma di una potenza di rumore misurata vicino a un confine di
quantizzazione. Non c'e' quindi una mappa canale -> indice da trovare, ed e'
inutile cercarla: serve la cal.

Conseguenza per il banco. Il banco vale `target - crs`, e **entrambi i termini
sono esiti della stessa cal sullo stesso ladder** — il che spiega perche' le
somme cadono sempre su due gradini e perche' il `target` sembrava dipendere
dalla sotto-banda. Non e' chiudibile per trascrizione: e' chiudibile solo
implementando la misura di rumore.

Cio' che lo sweep ha comunque corretto nel port e' l'aritmetica: l'offset e'
**con segno**. Il vendor scrive `0xfb`, cioe' -5, su ch100-140, dove il CRS
vale 57 e il target 52; la vecchia forma `(target > crs) ? target - crs : 0`
azzerava quel caso, e sembrava giusta solo perche' su ch36 la differenza cade
dal lato positivo.

Nota metodologica, perché è il punto che ha ritardato la soluzione: la
correlazione con il CRS era stata **cercata e scartata a torto**, confrontando
solo l'ultimo valore CRS con il valore del banco, in cerca di un'uguaglianza.
La relazione è additiva e su due siti con soglie diverse: cercare l'uguaglianza
non la vedeva. Vedi la riga corretta in §3.

## 1. Reperti strutturali

Misurati su tutte le 12 catture in `router-data/`, 21 blocchi in totale.

**Indirizzi.** `0x0910`–`0x0913` (54 occorrenze per registro) e, solo sul 4360 a
tre catene, `0x0b10`–`0x0b13` (18 per registro). `0x0710`–`0x0713` **non
compare mai**, in nessuna cattura, né in lettura né in scrittura.

Lo stride per-catena di questo PHY è `0x200`, usato in tutto il resto del
programming (`0x0644`/`0x0844`, `0x06c0`/`0x08c0`, `0x0720`/`0x0920`). Su
quella scala `0x0710` sarebbe la catena 0. Il numero di banchi scritti è quindi
`catene − 1`: uno sui due testimoni 4352 (2 catene), due sull'agcombo (3).

**Forma.** Otto campi byte per banco — 4 registri × hi/lo — ognuno scritto con
una RMW separata (`mask=0xff00` oppure `0x00ff`). L'ordine è **invariante su
tutti i 21 blocchi**:

    reg+0 hi, reg+0 lo,  reg+2 hi, reg+2 lo,
    reg+1 lo, reg+1 hi,  reg+3 lo, reg+3 hi

L'asimmetria (pari hi-poi-lo, dispari lo-poi-hi) è costante e non dipende da
board, canale, banda o fase.

**Contenuto.** Hi è sempre uguale a lo. Tutti e quattro i registri di un banco
portano lo stesso scalare. Banchi diversi possono portare scalari diversi.

**Write-only.** Nelle due catture con i RETVAL ripiegati non viene mai letto.

**Accoppiamento col blocco CRS.** In tutte e 21 le occorrenze il banco è
preceduto **esattamente 8 episodi prima** da un blocco di 8 `MOD 0x03xx
mask=0xff` (le soglie del clip detector, stride `0xc`, interleaved fra le
catene). Sono parti adiacenti della stessa sequenza, non due eventi
indipendenti. Il valore CRS varia per passata e per cattura: `0x2c`, `0x2d`,
`0x30`, `0x31`, `0x34`, `0x36`, `0x3a`.

**Valori osservati.** Sempre interi piccoli, `0`–`0x19` (0–25):

| cattura | blocchi (scalare per banco) |
|---|---|
| d6220 attach ch36 BW20 | `0`, `5` |
| d6220 down→bss ch36 BW20 | `3`, `5` |
| d6220 attach ch36 BW40 | `0`, `6`, `0` |
| d6220 attach ch44 | `0`, `0` |
| d6220 down→up (`_delay_only`) | `0`, `0`, `0` |
| agcombo attach ch36 | `0`/`0`, `6`/`9`, `9`/`6` |
| agcombo rescan ch36 | `0`/`0`, `6`/`9` |
| agcombo down→bss ch36 | `0xf`/`0xf`, `0x12`/`0x12` |
| agcombo down→bss ch36 BW40 | `0x13`/`0x19` |
| agcombo down→bss ch100 BW80 | `0xb`/`0xe` |
| DSL down→bss ch36 BW20 | `0`, `0`, `0` |
| DSL down→bss ch36 BW40 | `0`, `0`, `0` |

Da leggere insieme:

- **Non è costante entro una sessione.** Cambia a ogni invocazione, con
  incrementi non uniformi (+5, +2, +6/+3, +3).
- **Non è rumore.** È riproducibile fra sessioni sulla stessa board e canale: la
  seconda invocazione su d6220 ch36 BW20 vale `5` sia sull'attach sia sul
  down→up; la seconda su agcombo ch36 vale `6`/`9` sia sull'attach sia sul
  rescan.
- **La prima invocazione dopo un attach da freddo è sempre `0`** (d6220 su tre
  canali/bande, agcombo su due catture). Sul down→up del d6220 la prima vale
  invece `3`.
- **Il DSL con wl 6.30 scrive sempre `0`**, su tutte e sei le occorrenze.
- **Non correla con la soglia CRS scritta immediatamente prima.** Il blocco è
  preceduto da otto `MOD 0x03xx mask=0xff` con un valore fra `0x2c` e `0x3a`,
  ma CRS `0x3a` compare sia con banco `0` sia con banco `6`/`9`, e CRS `0x34`
  sia con `0` sia con `5`.
- **La `_delay_only` è anomala anche qui**: tre blocchi tutti a zero dove la
  `down-to-bss-ch36-bw20` mostra `3` e `5`. Coerente con le altre due anomalie
  al ribasso già documentate in [`retrace-todo.md`](retrace-todo.md); resta una
  cattura da non usare per i conteggi.

## 2. Ipotesi escluse

Con la prova che le esclude, perché rifarle costa più che leggerle.

| ipotesi | come è caduta |
|---|---|
| `0x0c02 >> 10 = 3` da tabella 7 | combacia con la prima occorrenza, ma in tutta la cattura non esiste una lettura di tabella 7 con `>>10 = 5`. Coincidenza su un valore piccolo |
| `b // 10` dai coefficienti | correlava su 10 punti di 13, con tre controesempi da entrambi i lati. La correlazione era temporale: banco e coefficienti partono da zero e crescono insieme |
| base index idle-TSSI | provato contro le catture, non combacia |
| ogni coppia per-core `0x06xx`/`0x08xx` letta o scritta prima del banco | nessuna dà il valore |
| campi SROM per-core | identici su tutte e tre le board, mentre il valore no |
| `0x073d + core*0x200` | legge zero in tutte e quattro le occorrenze |
| ~~soglia CRS precedente: nessuna correlazione~~ | **conclusione errata.** Il test confrontava l'ultimo valore CRS col valore del banco cercando un'uguaglianza. La relazione è additiva (`crs + off` costante) e il target cambia fra i due siti di chiamata, quindi quel test non poteva vederla. Vedi §4 |
| **`X[c] − X[0]`, differenza per-catena rispetto alla catena 0** | cercata su tutti i **575** indirizzi per-catena noti prima del blocco nell'agcombo rescan (l'unica cattura 3-catene con RETVAL, banco `6`/`9`): **0 hit** |

L'ultima nasceva dal reperto "mai la catena 0": se la catena 0 è il
riferimento, il valore sarebbe un delta e va cercata una differenza, non un
registro che valga il numero. Non ha prodotto nulla.

Il punto in comune delle prime sette è che cercano il valore **in un
registro**, per uguaglianza. Il valore non è in un registro: è la distanza fra
due entry di un ladder (§5).

## 3. Precedenti: chi programma registri con questa funzione

### b43 / brcmsmac N-PHY — la famiglia di registri c'è, il calcolo no

`b43/phy_n.h` definisce, per la N-PHY:

    0x27D  CRSMINPOWER0     0x27E  CRSMINPOWER1
    0x280  CRSMINPOWERL0    0x281  CRSMINPOWERL1
    0x283  CRSMINPOWERU0

Tre varianti di sotto-banda — 20 MHz, 40-lower, 40-upper — ognuna con un
indice di catena, campi byte.

L'unico scrittore in brcmsmac è `wlc_phy_adjust_crsminpwr_nphy`, ed è un
workaround statico per spurie: su una singola combinazione canale+BW40 mette
una costante (`NPHY_ADJUSTED_MINCRSPOWER`, `0x1e` = 30) nel byte basso delle tre
varianti, salvando il byte precedente in `nphy_crsminpwr[]` per ripristinarlo
quando la condizione cade. Nessuna derivazione da una misura.

Il dettaglio che conta è un altro: `noise_crsminpwr_index` è **dichiarato in
`phy_int.h` e mai referenziato in tutto il tree**. La parte adattiva è stata
tolta dal rilascio open-source. Cercarla in brcmsmac non dà nulla non perché
non esista nel driver vendor, ma perché è stata rimossa — che è lo stesso
pattern già accertato una volta su questo porting (il supporto vcofreq
frazionario assente nella wl 6.30 del DSL, vedi `retrace-todo.md`).

Nota di scala: `0x1e` = 30, contro i nostri 0–25.

### ath9k — lì il calcolo è aperto

Per la quantità equivalente ath9k ha tutta la catena, e la sua forma è quella
che interessa:

- noise floor letto **per catena**;
- un ring buffer degli ultimi 5 campioni per catena, con indice che avvolge
  (`AR_PHY_CCA_FILTERWINDOW_LENGTH` = 5);
- il valore usato è la **mediana** del buffer, non l'ultimo campione;
- clampato contro limiti nominal/min/max per banda;
- riscritto per catena in un campo a 9 bit, valore shiftato di uno;
- il commento nel sorgente dichiara che l'operazione carica il valore filtrato
  dal software nella variabile interna di baseband `minCCApwr`: il registro è
  una **porta di scrittura verso stato del baseband**, non un registro da
  rileggere;
- al cambio di canale i dati di calibrazione vengono azzerati e la history
  reinizializzata, con un contatore che fa usare il campione grezzo invece
  della mediana per le prime invocazioni.

`minCCApwr` e `crsminpwr` sono lo stesso concetto con due nomi: la potenza
minima perché il baseband dichiari carrier sense.

## 4. La relazione: offset sulla scala CRS

Mettendo in fila la soglia CRS del blocco che precede e il valore del banco,
**a BW20 le somme non-zero sono esattamente due per chip**, e sono le stesse su
sessioni con baseline CRS diverse:

| board | canale | passata | CRS | banco A | banco B | CRS+A | CRS+B |
|---|---|---|---|---|---|---|---|
| agcombo | ch36 attach | 1 | 58 | 0 | 0 | — | — |
| agcombo | ch36 attach | 2 | 58 | 6 | 9 | **64** | **67** |
| agcombo | ch36 attach | 3 | 58 | 9 | 6 | **67** | **64** |
| agcombo | ch36 rescan | 1 | 58 | 0 | 0 | — | — |
| agcombo | ch36 rescan | 2 | 58 | 6 | 9 | **64** | **67** |
| agcombo | ch36 down→bss | 1 | 49 | 15 | 15 | **64** | **64** |
| agcombo | ch36 down→bss | 2 | 49 | 18 | 18 | **67** | **67** |
| d6220 | ch36 attach | 1 | 58 | 0 | — | — | — |
| d6220 | ch36 attach | 2 | 52 | 5 | — | **57** | — |
| d6220 | ch36 down→bss | 1 | 49 | 3 | — | **52** | — |
| d6220 | ch36 down→bss | 2 | 52 | 5 | — | **57** | — |

Sei punti agcombo su due sessioni con baseline 58 e 49 cadono su `{64, 67}`.
Tre punti d6220 su due sessioni con baseline 58, 52 e 49 cadono su `{52, 57}`.
Il banco non è quindi una quantità autonoma: è la **distanza fra il CRS
corrente e una soglia assoluta**.

### La forma implementata, e la sua direzione causale

Nel port:

    off = max(0, target - crs)

**Attenzione: la causalità è al contrario rispetto al modello del §5.4.** Lì la
soglia assoluta è il primario e il CRS è il derivato (`crs = soglia - off`);
qui il CRS è calcolato da una regola su BW e fase e l'offset ne discende. La
forma riproduce i numeri sulla configurazione validata perché **entrambi** i
termini sono fittati su di essa, non perché descriva il meccanismo.

Il clamp a zero copre tutti gli zeri osservati senza casi speciali. Il `target`
è per sito di chiamata — `chanspec_tail` e il blocco E di `rxiqcal_finalize`
hanno soglie diverse — e per chip:

| chip | chanspec_tail | blocco E |
|---|---|---|
| 4352 (d6220, DSL) | 52 | 57 |
| 4360 (agcombo) | 64 | 67 |

Verifica sui quattro punti dei due gate d6220 ch36 BW20, tutti e quattro
riprodotti:

| | CRS | target | off previsto | osservato |
|---|---|---|---|---|
| attach, chanspec_tail | 58 | 52 | 0 (clamp) | 0 |
| attach, blocco E | 52 | 57 | 5 | 5 |
| down→bss, chanspec_tail | 49 | 52 | 3 | 3 |
| down→bss, blocco E | 52 | 57 | 5 | 5 |

### Perché era invisibile

L'input era già in ambito. `crs` è una variabile locale della stessa funzione
che chiama il banco, sulla riga precedente, e il port la calcolava già
correttamente (`0x3a` al primo bring-up BW20, `0x31` a un channel setup
successivo, `0x36` fuori BW20). Mancava solo di passarla.

## 5. La tabella: trovata e attribuita

Il plurale in "crs_min thresholds" indica un insieme discreto, quindi una
tabella. C'è, nel blob D6220 (`wlD6220.o_save`, MIPS32 BE rilocabile, non
strippato), a `.rodata +0x040e84`: **tre righe da 15 entry `u8`, NUL-padded a
stride 16**, monotone con passi 2..3.

    +0x040e84   2d 30 33 35 36 39 3c 3f 42 44 46 48 4b 4e 50   (45..80)
    +0x040e94   2c 2e 30 32 34 36 38 3a 3c 3f 42 45 47 4a 4c   (44..76)
    +0x040ea4   29 2c 2e 30 32 34 37 39 3c 3f 41 44 46 48 4a   (41..74)

**Attribuzione**: le tre righe sono referenziate da **una sola** funzione,
`wlc_phy_crs_min_pwr_cal_acphy`. Vedi §5.2 per come si ottiene senza
disassemblare.

Oltre alle tabelle, la funzione carica:

    wlc_phy_get_chan_freq_range_acphy    <- selettore di riga
    wlc_phy_set_crs_min_pwr_acphy        <- setter
    wlapi_suspend_mac_and_wait / wlapi_enable_mac
    wlc_phyreg_enter / wlc_phyreg_exit

Quindi **le righe sono indicizzate per sotto-banda di frequenza** (5GL/5GM/5GH,
tre valori, tre righe), non per larghezza di banda. Coerente col dato: ch36 e
ch44 stanno nella stessa sotto-banda e usano la stessa riga, ma con **indice
diverso** — e l'indice è l'uscita della calibrazione.

I valori osservati cadono dentro:

| osservato | riga | indice |
|---|---|---|
| `0x3a` = 58 | `+0x40e94` | 7 |
| `0x36` = 54 | `+0x40e94` | 5 |
| `0x34` = 52 | `+0x40e94` | 4 |
| `0x30` = 48 | `+0x40e94` | 2 |
| `0x2c` = 44 | `+0x40e94` | 0 |
| `0x2d` = 45 | `+0x40e84` | 0 (agcombo ch100) |
| `0x31` = 49 | **nessuna riga** | — |

Tutti i valori d6220 stanno nella riga **centrale**, non nella prima. L'ordine
delle righe rispetto a 5GL/5GM/5GH **non è stabilito**: non assumerlo.

`0x31` = 49 non è in nessuna riga e compare solo sulla prima passata del
down->up. Candidato: il desense sommato sopra da `wlc_phy_desense_apply_acphy`,
non una quarta riga — dopo `+0x40eb3` ricominciano i puntatori.

### 5.1 Perché la ricerca per nome non poteva trovarla

È l'initializer di un **array locale**, quindi `.rodata` **anonima**: nessun
simbolo. Enumerare i simboli `OBJECT` — anche i 2859 `LOCAL` e i 779 con
suffisso `.NNNNN` degli static di funzione — non la raggiunge per costruzione.

Peggio: per array piccoli GCC può non usare `.rodata` affatto, e materializzare
l'initializer come **store immediati** nel codice. In quel caso nessuno scan di
`.rodata`/`.data` lo trova, e l'unica via è cercare gli immediati nelle
istruzioni. Qui non serve, ma va tenuto presente prima di dichiarare "assente".

### 5.2 Come cercarla, e come attribuirla

**Cercarla per forma.** Un ladder di soglie è monotono con passi piccoli e
regolari, e soprattutto **senza tratti a passo 1** — è quel vincolo che elimina
le liste di indici (le `rate_sets_*`, che dominano i falsi positivi). Con
dominio generoso `0x10..0x80`, `len >= 8`, passi in `2..8` e almeno due passi
distinti:

| larghezza | passi | candidati |
|---|---|---|
| u8 | 1..8 | 185 |
| u8 | 1..4 | 123 |
| u8 | **2..8** | **8** |
| u16 | 1..8 | 18 |
| u16 | **2..8** | **1 tabella distinta** |

Il limite superiore conta: a `2..4` la tabella u16 risultava troncata a 16
entry, a `2..8` si vede intera (25). Fra gli 8 superstiti u8 ci sono le tre
righe più `acphy_tx_evm_tbl_rev0` e `lpphy_rev2_gain_table`: il filtro
seleziona la classe giusta di oggetto, ma **non separa soglie da gain**. Serve
l'attribuzione.

**Attribuirla per indirizzo codificato.** Su MIPS un indirizzo a 32 bit si
carica con `lui %hi` + `addiu %lo`, e le due istruzioni portano rilocazioni
`R_MIPS_HI16` / `R_MIPS_LO16`. Non serve disassemblare: si legge `.rel.text`, si
appaiano le due rilocazioni sullo stesso simbolo, si ricostruisce
`(hi << 16) + (s16)lo + valore_del_simbolo`, e si guarda quale simbolo `FUNC`
contiene l'offset della rilocazione.

Su questo blob: 77662 rilocazioni in `.rel.text`, 28554 `HI16`, 8537 indirizzi
ricostruiti. Le tre righe risultano tutte di `wlc_phy_crs_min_pwr_cal_acphy`.

Il metodo si controlla da sé: l'altro candidato anonimo emerso dal filtro,
`+0x051b58`, risulta di `wlc_phy_elna_gainctrl_workaround`. Attribuisce blocchi
diversi a funzioni diverse, non collassa tutto su un nome.

### 5.3 Cosa dice il resto del blob sulla calibrazione

Le stringhe di dump:

    crs_min_pwr cal:
      ACI desense is on:  crs_min_pwr cal DID NOT run
       crsmin_cal ran %d times for channel %d:
       Noise power used for setting crs_min thresholds :

Più i simboli `wlc_phy_set_crs_min_pwr_higain_acphy`,
`wlc_phy_force_crsmin_acphy`, `wlc_phy_noise_sample_request_crsmincal`, e la
famiglia `wlc_phy_desense_*_acphy`.

Quindi: la cal misura una potenza di rumore, ne ricava un **indice** nella riga
della sotto-banda corrente, tiene un contatore di esecuzioni per canale, e non
gira se l'ACI desense è attivo. Il valore cambia fra invocazioni della stessa
sessione perché cambia l'indice, non perché sia un continuo.

Per confronto, la N-PHY ha lo stesso oggetto **con un nome**:
`NPHY_ofdm_desense_lut_rev3to6`, 25 entry `u16` in `.data` con una copia `const`
in `.rodata`, passi che crescono da 2 a 7 — la firma di una ladder in dB. Per
l'AC-PHY nessun desense LUT ha un nome.

### 5.4 La direzione giusta: la somma è la entry, il CRS è il derivato

Test: per ogni blocco, `CRS + banco` è una entry del ladder?

| blocco | CRS | banco | somma | somma nel ladder | CRS nel ladder |
|---|---|---|---|---|---|
| d6220-att #0 | 58 | 0 | 58 | `B[7]` | `B[7]` |
| d6220-att #1 | 52 | 5 | 57 | `A[5]` `C[7]` | `B[4]` `C[5]` |
| d6220-d2u #0 | 49 | 3 | 52 | `B[4]` `C[5]` | **nessuna** |
| d6220-d2u #1 | 52 | 5 | 57 | `A[5]` `C[7]` | `B[4]` `C[5]` |

**4/4 sul d6220.** E la terza riga risolve l'anomalia aperta da tutta l'analisi:
`CRS = 49` non è in nessuna riga del ladder, ma `49 + 3 = 52` sì. Quindi non è
il CRS a venire dal ladder con il banco come correzione: è la **somma** a essere
la entry.

    misura per catena  ->  indice salvato  ->  soglia = ladder[indice]
                                               banco  = offset per catena
                                               CRS    = soglia - offset

Il CRS porta la parte comune (uniforme sugli 8 registri), il banco la correzione
per catena, e l'hardware somma. Spiega perché il CRS può cadere fuori dal ladder
mentre la soglia effettiva no.

Sulle altre board: il **DSL** dà 6/6 ma è banale (banco 0, quindi somma = CRS) e
non porta evidenza indipendente. L'**agcombo** dà 2/4: le due somme non-zero
(`64`, `67`) non sono nel ladder — che però è quello letto dal blob **del
d6220**, mentre l'agcombo è un 4360 con un driver diverso. Un ladder diverso è
l'ipotesi più economica; serve il blob agcombo per verificare.

### 5.5 Le due incognite residue, e una respinta

**L'indice.** Non è ricavabile dalle catture: l'harness consuma ogni lettura che
contengono (`0x000b` a parte, due letture dello stesso valore costante), e la
misura arriva da un campionamento dedicato che le catture non vedono.

**L'offset per catena.** Vale 0 finché i coefficienti RXIQ non sono misurati e
non-zero dopo; sull'agcombo si separa `6`/`9` fra due catene. I coefficienti
per catena `0x?a1` sono **letti** subito prima del banco esattamente nei blocchi
dove l'offset è non-zero, e mai dove è zero — 13 casi su 14, con l'unica
eccezione in una cattura che parte a metà sessione. La correlazione posizionale
è forte; la trasformazione no.

**Respinta: `offset = b / 10`** con `b` il coefficiente letto. Torna su 4 casi
(57, 59, 60, 92 -> 5, 5, 6, 9) ma non è credibile: una divisione per 10 non è
un'operazione da firmware PHY, sono 4 punti con 3 uscite distinte, e se la
relazione passa per un indice in un ladder è **a tratti** — su un intervallo
stretto di `b` si approssima con qualunque cosa. Va nella lista del §2.

### La parametrizzazione è in NVRAM, e su questi board è vuota

Il blob espone una famiglia di variabili NVRAM per questa calibrazione:

    noise_cal_enable_{2g,5g}        noise_cal_ref_{2g,5g}
    noise_cal_ref_40_{2g,5g}        noise_cal_adj_{2g,5g}
    noise_cal_po_{2g,5g}            noise_cal_po_40_{2g,5g}
    noise_cal_po_bias_{2g,5g}       noise_cal_nf_substract_val[_{2g,5g}]
    noise_cal_high_gain[_{2g,5g}]   noise_cal_deltamin / noise_cal_deltamax
    noise_cal_update                noise_cal_dbg

`noise_cal_ref_5g` è il riferimento, cioè il "target" del §4. `deltamin` e
`deltamax` sono il clamp sul delta, che spiega il `max(0, ...)` ricavato
empiricamente.

**Nessuna di queste è impostata in nessuna delle tre NVRAM** in
`router-data/`. Il driver cade quindi sui default compilati, il che spiega
perché il comportamento è consistente fra i due 4352 e perché i valori sono
quelli.

### Cosa questo implica per il port

Le quattro soglie del §4 non sono celle di una tabella: sono **l'uscita a regime
di una calibrazione di rumore sul canale validato**, per i due siti. Non sono
derivabili in senso deterministico, perché sono l'esito di una misura.

Due strade, entrambe legittime:

1. **Tenere le costanti**, etichettate per quello che sono. Riproducono ch36
   BW20 op-per-op e non pretendono di più.
2. **Implementare la cal**, che richiede il percorso di campionamento del rumore
   — e quindi piu' copertura degli accessor di I/O per validarla, entro il
   vincolo di metodo in `porting-plan.md`.

### La scorciatoia per i target degli altri canali

`wlc_phy_force_crsmin_acphy` ha accanto la stringa `phy_force_crsmin`, nella
regione dove stanno i nomi degli iovar. **Da provare** sull'hardware:
`wl -i wl1 phy_force_crsmin` dovrebbe permettere di leggere o forzare il
valore per canale. Se funziona, i target degli altri canali si ottengono
interrogando il driver stock, senza né una cattura nuova né disassemblare —
che è il modo più economico di chiudere i controesempi qui sotto.

### I controesempi, come uscite della cal su canali diversi

- **d6220 ch44 BW20, seconda passata**: CRS 52, banco 0, dove il target 57
  darebbe 5. Su ch36 con lo stesso CRS 52 il banco vale 5. Quindi il target
  dipende dal canale, e con una sola cattura ch44 non è ricavabile.
- **d6220 ch36 BW40**: tre blocchi con CRS 54 e banchi 0, 6, 0 (somme 54, 60,
  54). Il 60 non è in `{52, 57}` e la terza passata torna a zero. Su BW40 il
  vendor emette tre blocchi contro i due di BW20, quindi anche
  l'indicizzazione per sito va rivista prima di poter confrontare.
- **agcombo BW40 e BW80**: somme `{67, 73}` e `{56, 59}`, contro `{64, 67}` di
  BW20. Il 67 ricorre, il resto no.

Le costanti in `phy_ac.c` sono marcate `TODO(formula)` per questo: la forma è
derivata, i quattro valori no. Servono almeno un secondo canale a BW20 per
separare la dipendenza dal canale, e la mappatura dei siti a BW40 per capire
perché le passate sono tre.

**Il primo/secondo banco a parità di CRS.** Sull'agcombo la prima passata di
attach e rescan scrive 0 con CRS 58, mentre la seconda con lo stesso CRS 58
scrive 6/9. Con target 64/67 la prima dovrebbe dare 6/9. Il clamp non basta:
la prima passata dopo un attach da freddo scrive zero comunque. Coerente col
comportamento descritto nel §6 (history non ancora popolata), ma non modellato:
sul 4352 il caso non si presenta perché il CRS della prima passata (58) è già
sopra il target.

**La ripartizione fra catene.** Sull'agcombo i due banchi portano `6`/`9` e la
loro assegnazione **si scambia** fra la seconda e la terza passata dell'attach
(`6`/`9` poi `9`/`6`), mentre sul down→bss portano lo stesso valore su entrambe.
La forma del §4 dà un solo offset e non spiega né la differenza fra catene né
lo scambio. Sul 4352, con un solo banco, il punto non si presenta.

**Mai la catena 0.** `0x0710`–`0x0713` non compare in nessuna cattura, mentre
sia in N-PHY sia in ath9k la quantità equivalente esiste per ogni catena. Due
letture, entrambe **SALAME**: o la catena 0 è il riferimento, o il blocco è
indicizzato per coppia adiacente (2 catene → 1 coppia, 3 → 2, che riproduce
`banchi = catene − 1`).

## 6. Cosa resta da fare

I precedenti del §3 restano rilevanti per i punti aperti, e ora si leggono
meglio: confermano che una soglia CRS/CCA per-catena si programma esattamente
così, per differenza rispetto a un valore corrente e con un clamp.

Il pezzo di ath9k che resta interessante è il **filtro**: mediana su una
finestra di cinque campioni per catena, con un contatore che fa usare il
campione grezzo invece della mediana per le prime invocazioni dopo un cambio di
canale. È il candidato naturale per i due punti aperti che la forma del §4 non
copre — la prima passata a zero anche quando il target lo permetterebbe, e la
differenza fra catene con lo scambio fra passate. **SALAME**: analogia fra chip
diversi, non verificata su AC-PHY rev 1.

Se è così, il `target` del §4 non è una costante ma il risultato del filtro, e
le quattro costanti attuali sono il suo valore a regime sul canale validato.
Questo predice che su un canale con rumore diverso i target cambino — che è
esattamente il controesempio di ch44.

## 7. Nota sul nome

Il nome per indirizzo (`prog_bank_0910`) resta la scelta corretta finche' la
semantica non e' confermata sul chip: non promette quello che non sappiamo. Il
candidato semantico -- `CRSMINPOWER` in N-PHY, `minCCApwr` in ath9k, vedi §3 --
sta qui, non nel nome del simbolo.
