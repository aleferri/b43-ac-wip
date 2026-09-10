# Censimento delle letture scartate

Il port emette le letture del vendor per allineare la traccia, ma in molti siti
il valore letto finisce in un `dummy`, un `discard` o un `read_log` senza
destinazione, e la scrittura che segue porta una costante presa da `cold01`.
Sul d6220 a ch36 la costante e' giusta per costruzione; su un'altra board, un
altro canale o un'altra corsa e' un valore inventato. Questo file elenca dove
succede e cosa il vendor fa con quel valore.

## Metodo

Due strumenti, incrociati:

- `reverse-tools/reads.py consumers` legge la cattura vendor ripiegata (con i
  retval) e per ogni lettura cerca la scrittura che ne consuma il valore nelle
  600 op successive: lo stesso valore scritto altrove (**copy**), la stessa
  cella riscritta uguale (**rmw(=)**, cioe' save/restore) o con una relazione
  additiva o di bit (**rmw(+k)**, **rmw(xor k)**), oppure una rilettura senza
  scrittura in mezzo (**poll**). Il resto e' **unused**. Valori banali
  (0, 1, 0xffff) non fanno da traccia.
- `test/unit/read_perturb.py` fa girare il flow a freddo con una lettura per volta
  perturbata nell'oracolo, celle di tabella comprese
  (`AC_READ_PERTURB_KIND=tbl:<id>`), e guarda se le op emesse cambiano.
  **CONSUMED** se cambiano, **DISCARDED** se no. Le maschere si passano con
  `MASKS`, e **una sola maschera non basta**: un bit che il consumatore
  scarta e' invisibile, quindi il default a `0x0001` sotto-riporta il consumo.
  Con le dieci maschere di `MASKS` venti letture passano da DISCARDED a
  CONSUMED -- fra cui `PHY 0x0012`, consumato come `read >> 2`, e i quattordici
  registri radio con relazione `rmw(=)` o `xor`.

Su `cold01`: 448 indirizzi letti, 305 con una relazione nel vendor; di questi
il port ne consuma **224** e ne scarta **81**. Il verdetto non dipende dalla
finestra dell'oracolo: con partenza all'insmod (`#424`) o all'inizio del
confronto (`#528`) i conteggi sono identici.

Le righe qui sotto sono gli scartati con una relazione che non e' solo `poll` o
`mod` (le `PHY.MOD` sono RMW che l'harness non legge, quindi il test non le
vede: non sono un difetto). Le `OBJ` sono la shared memory del MAC, fuori dal
PHY, e stanno in coda.

### Due limiti del perturbatore, da tenere presenti leggendo i verdetti

- **E' per indirizzo, non per sito.** Un registro consumato da una funzione e
  scartato da un'altra risulta CONSUMED, e il secondo sito resta invisibile.
  Il caso trovato cosi' e' `b43_radio_2069_afecal()`, che salvava la terna
  `0x0739/0x073a/0x0725` e la riscriveva costante mentre gli altri siti degli
  stessi registri la consumavano. Per trovarne altri serve il conteggio delle
  letture per funzione (`AC_FN_MARKERS=1`), non il verdetto per indirizzo.
- **Un campo riscritto per intero non e' perturbabile.** Se l'RMW del vendor
  copre tutti i bit della parola letta, nessuna maschera arriva all'uscita e la
  lettura resta DISCARDED per costruzione. E' il caso della parola alta del
  TX-LPF (`0x07[0x0362-0x037a]`), che il codice consuma davvero.

## Le famiglie, per pericolo

### 1. bbmult: il moltiplicatore TX in banda base

| lettura vendor | cosa ne fa | dove il port la scarta |
| --- | --- | --- |
| `TBL 0x20[0x00]` | scrive il valore in IQLOCAL `0x63`, `0x67`, `0x6b` (bbmult per core) ×11 | `phy_ac.c:5483`, `:8789` |
| `TBL 0x20[0x14]`, `[0x1e]` | scrive in `0x63` e `0x67` ×2 | `:6893`, `:6895`, `:7815`, `:7817` |
| `TBL 0x20[0x40]` | scrive in `0x63` e `0x67` ×2 | `:9466` |
| `TBL 0x0c[0x63]` | save/restore ×13; `xor 0x35` ×6 | `:1244`, `:7034`, `:7153`, `:8232`, `:8832` |
| `TBL 0x0c[0x67]` | copiato in `0x63` ×6 e in PHY `0x0462` ×2; save/restore ×6 | `:1249`, `:7064`, `:7155`, `:8234`, `:8834` |

Il port scriveva `0x0044`, `0x0040`, `0x003c`, `0x0035` come `static const`.
Sono il bbmult, che decide la potenza trasmessa: la tabella `0x20` e'
GAINCTRLBBMULT, il bbmult per indice di potenza, IQLOCAL `0x63/0x67/0x6b` la
cella applicata, e il vendor la salva e ripristina attorno alle cal.

**Chiusa.** Lo stato porta `bbmult_cal[core]` (la voce di `0x20` letta per la
cal TX, indici 0x14 e 0x1e), `bbmult_meas` (la voce a indice 0, letta per la
misura RX-IQ) e `bbmult_saved[core]` (la cella prima di una cal). Ogni
scrittura di `0x63/0x67/0x6b` viene da uno di questi o dalla lettura di
`0x20[idx]` che la precede nella stessa funzione; le costanti sono sparite.
Il perturbatore da' CONSUMED su tutte e sei le celle (`0x20[0x00/0x14/0x1e/
0x40]`, `0x0c[0x63/0x67]`); gate `cold01` e periodico invariati. Restano tre
letture di `0x63/0x67` in `dummy` (`:7153`, `:8232`, `:8832`): sono i
salvataggi interni il cui ripristino e' la stessa parola di `bbmult_cal` o
`bbmult_meas`, gia' scritta da quella.

### 2. Registri di guadagno per core `0x0720-0x0747` e strides

| lettura vendor | cosa ne fa | dove il port la scarta |
| --- | --- | --- |
| PHY `0x0732`, `0x0733`, `0x0747` (+`0x0932`...) | save/restore ×6 | `:1166-1168`, `:1182-1184` |
| PHY `0x073c`, `0x093c` | save/restore ×6 | `:1172`, `:1188`, `:4214` |
| PHY `0x073e`, `0x093e`, `0x0b3e` | save/restore ×3, `xor 0x0440` ×3 | `:4210`, `:4941`, `:6801`, `:8031`, `:8566` |
| PHY `0x093a` | copiato in `0x073a`; save/restore ×17 | `:1311`, `:4223`, `:7914` |
| PHY `0x0727`, `0x0927` | copiato in RAD `0x0021`; `0x0927` copiato in `0x0727` ×6 | `:1171`, `:1187`, `:4213`, `:6824` |
| PHY `0x0720` | copiato in `0x1720` (alias broadcast) | `:4217`, `:6372`, `:6816`, `:8940` |
| PHY `0x0725` | copiato in `0x1725`; `xor 0x0004` ×17 | consumato in un sito, non negli altri |
| PHY `0x0724`, `0x0924` | `+0x03ff` ×5 | -- |
| PHY `0x0730/0x0731`, `0x0930/0x0931` | riscritti con un'altra parola ×1 | -- |
| PHY `0x0736`, `0x0936` | riscritti `+0x154/+0x152/+0x22a` | -- |
| PHY `0x0739`, `0x0939` | `xor 0x0080` ×17 | -- |

Il vendor legge lo stato del gain control prima della cal e lo rimette dopo;
il port rimetteva lo stato di `cold01`.

**Chiuse le due sequenze locali**: `idle_tssi_meas()` (0x0747, 0x0732, 0x0733,
0x0727, 0x073c salvati e rimessi; 0x0739, 0x073a, 0x0725 salvati, pilotati con
`saved | 0x80`, `saved`, `saved | 0x04` e rimessi) e `rxgain_perchan_tail()`
(la stessa terna, per core, rimessa in ordine inverso). Sono variabili locali,
non stato nascosto: lettura e scrittura stanno nella stessa funzione. Il
perturbatore da' CONSUMED su 0x0725/0x0727/0x0732/0x0739/0x073a/0x073c/0x0747.
`rxgain_defaults_pulse()` invece **non e' un restore**: nessuna lettura nei
mille op che la precedono, scrive default.

**Chiuse anche le due coppie a distanza**, trovate cercando i blocchi di
scritture consecutive e le letture degli stessi registri prima:

- `rx_gain_regs_program()` legge 14 registri per core in un ordine fisso
  (`b43_phy_ac_rxgain_regs[]`) e `measure_block()` li riscrive nello stesso
  ordine ~500 op dopo, 3 volte per corsa: `rxgain_saved[core][14]`.
- `rxgain_config_readback()` legge 25 registri piu' `0x073e` e le tre righe
  RFSEQ `0x07[0x100/0x103/0x106 + core]`, e `rxiq_teardown_apply_defaults()`
  riscrive tutti e 26 (0x0727 prima di 0x0726) e le tre righe due volte:
  `rxgain_cfg_saved[core][26]` e `rfseq_gain_saved[core][3]`. Il nome
  "defaults" era sbagliato: i valori sono quelli letti.

Perturbatore: CONSUMED su tutti i `0x0720-0x073e` con una relazione e sulle
`0x07[0x100-0x107]`, che erano le prime righe della famiglia 3. Le copie
`0x0927 -> 0x0727` e `0x0727 -> RAD 0x0021` sono coincidenze di valore
(`0x0004` su entrambi i lati): il restore di `0x0727` viene dalla propria
lettura. Gli alias `0x0720 -> 0x1720` e `0x0725 -> 0x1725` stanno nella
famiglia 4.

A caldo i tre gate perdono ~0.2 punti (86.26 -> 86.04, 89.50 -> 89.28,
86.14 -> 85.93): la' la coda dell'oracolo per indirizzo su PHY `0x0725`,
`0x073a`, `0x0720`, `0x0728` e' fuori passo -- il port legge quei registri un
numero diverso di volte del vendor -- quindi il valore salvato e' quello di
un'altra lettura e il restore lo propaga; le costanti di `cold01`
coincidevano per caso. E' il limite dell'oracolo piatto, non del driver: sul
ferro la lettura torna il registro.

Le righe con `--` non hanno un sito trovato per indirizzo letterale: la
lettura e' emessa con offset calcolato (`0x0720 + stride`), va cercata per
funzione.

**Chiusa anche la terza sequenza sulla stessa terna**: `b43_radio_2069_afecal()`
legge `0x0739/0x073a/0x0725` per core nel primo giro, le due masksets di arming
ne alzano `0x0080` e `0x0004`, e il secondo giro le riscrive dal salvataggio
(`gain_saved[core][3]`, locale alla funzione perche' i due giri stanno la'
dentro). Il perturbatore non poteva dirlo: gli altri siti della terna la
consumavano gia', quindi l'indirizzo risultava CONSUMED con questo sito ancora
a costanti. Sui 26 segmenti a freddo e sui 52 a caldo i valori letti sono
sempre `0x0000/0x0180/0x0600`, cioe' esattamente le costanti che c'erano: il
confronto non si muove, cambia solo che ora il valore viene dal registro.

### 3. RFSEQ, tabella `0x07`: chiusa, in tre modi diversi

| lettura vendor | cosa ne fa | esito |
| --- | --- | --- |
| `0x07[0x100-0x107]` | save/restore ×3 | chiuso con la famiglia 2 (`rfseq_gain_saved`) |
| `0x07[0x362-0x37a]`, `[0x360/0x361/0x370/0x371]` | riscritte `0x0101 -> 0x0151`, `0 -> 0x0150` | **non un difetto**: sono le parole alte del TX-LPF a 25 bit; `set_analog_tx_lpf_locked()` legge lo/hi e riscrive i campi f9/f17 dal cap di rccal, e i campi coprono l'intera parola alta. Il "+0x50" era la coincidenza fra campi scritti e bit letti |
| `0x07[0x3fa]`, `[0x06a]` | `0x002c -> 0x002e` | **non un difetto**: stage 8 del dacbuf, RMW che tiene bit 5 e 6-11 e scrive il cap |
| `0x07[0x3cd]`, `[0x3dd]` | `0x0c02 -> 0x04c2/0x04e2` | lettura uguale su d6220 freddo, caldo e agcombo, scrittura uguale: nessun dato distingue campo da costante. Resta costante con un SALAME nel commento |

Le prime due righe sono un limite del perturbatore, non del codice: il bit
perturbato (`0x0001`, e anche `0x0100`) cade in un campo che l'RMW riscrive
comunque, quindi l'uscita non cambia e la lettura viene segnata DISCARDED. La
prova che il valore letto conta sta nei bit fuori dal campo, e va fatta con
la maschera giusta per cella; `read_perturb.py` accetta `AC_READ_PERTURB_MASK`.

### 4. Copie fra core e alias broadcast: chiusa

Ogni coppia verificata sulla sequenza vendor, a freddo e a caldo, prima di
toccare il codice: tre erano copie, quattro coincidenze di valore.

| lettura vendor | cosa ne fa | esito |
| --- | --- | --- |
| PHY `0x06d8` -> `0x16d8` | `RD 0x6d8=0x1431; WR 0x16d8=0xffff; ... WR 0x16d8=0x1431` | **copia**: il registro del core 0 salvato attorno alla parentesi e rimesso su tutti i core via alias. Chiuso |
| PHY `0x0601` -> `0x1601` | `RD 0x601=0x49; WR 0x1601=0x49`, due op dopo | **copia**: il valore del core 0 propagato a tutti i core. Chiuso |
| PHY `0x06dc/0x08dc/0x0adc` | per core: `RD 0x?dc; WR 0x?dc=<letto>; WR 0x?dd=0x604` | **non una catena fra core**: ogni core riscrive il proprio `0x?dc` con cio' che ha letto. Chiuso (`0x?dd` non ha una lettura prima, resta il suo valore) |
| PHY `0x0720/0x0725` -> `0x1720/0x1725` | scritture broadcast senza lettura prima su `cold01` | coincidenza: default |
| `TBL 0x45/0x65/0x85[0x20]` | `WR 0x2; RD` per ogni tabella | write-verify di una costante, non una catena: la scrittura precede la lettura |
| RAD `0x090b` -> RAD `0x054b` | `RD 0x90b=0x100` e, altrove, `RD 0x54b -> WR 0x54b|0x100 -> RD -> WR |0x1` | coincidenza: `0x54b` segue le proprie letture (nel port e' una `MOD`), `0x90b` e' inutilizzata |
| RAD `0x0548` | `RD -> WR letto|1 ... RD -> WR 0` | nel port e' `radio_maskset`, cioe' la stessa RMW: non un difetto del driver, il test non vede le MOD |

Perturbatore: CONSUMED su `0x0601`, `0x06d8`, `0x06dc/0x08dc/0x0adc`.

### 5. AFE cal del radio: chiusa

| lettura vendor | cosa ne fa | esito |
| --- | --- | --- |
| RAD `0x0122` (+stride `0x0322/0x0522`) | `WR <letto> \| 0x000f` per armare, `WR <letto>` per disarmare | il port scriveva un `switch` sulla larghezza (`0x5830/0x5030/0x4230`) con il campo alto ricavato dallo sweep e il commento che diceva di non sapere cosa selezionasse. Ora entrambe le scritture vengono dal readback: niente tabella, niente campo da spiegare |

Il verdetto passa a CONSUMED su `0x0122` e `0x0322` con ogni maschera provata,
il gate su `cold01` resta `28552/28574` e il periodico resta `MATCH`. Sui
segmenti a 40 e 80 MHz il valore letto e' quello che il `switch` restituiva,
quindi il punteggio non si muove nemmeno la': quello che cambia e' che la
larghezza non entra piu' nel calcolo, e il campo alto arriva dal ferro anche su
una board dove non vale quei tre valori.

### 6. Da guardare, relazione incerta

Il set di maschere ha chiuso da se' meta' di questa sezione: `PHY 0x0012`,
`0x0401`, `0x06c1`, `0x06c3`, `0x08c1`, `0x08c5`, `RAD 0x0043/0x0243/0x0443`,
`0x0065/0x0265/0x0465`, `0x0127/0x0327/0x0527`, `0x040b`, `0x0548`, `0x08d0` e
`TBL 0x07[0x006a]/[0x03fa]` sono CONSUMED, cioe' il port li consumava gia' e il
perturbatore a un bit non lo vedeva. Restano:

- PHY `0x0393` `xor 0x8000` ×18, `0x0394` `+0x105/+0x106` ×9: sono i registri
  della potenza per indice; la relazione additiva puo' essere una coincidenza
  fra valori piccoli. Il port li scrive costanti.
- PHY `0x0550` riscritto `xor 0x0dd1` ×1, save/restore ×1.
- RAD `0x0020/0x0022/0x003a` e i loro stride `0x02xx/0x04xx`: save/restore ×1
  ciascuno, e solo sui sette segmenti lunghi.
- PHY `0x0070` `mod 0xe000`, `0x016c` `mod 0x0040`, `0x03a9` `mod 0x007f`,
  `0x040f` `mod 0x0200` ×12, `0x0678/0x0878/0x0a78` `mod 0x0001`: RMW che nel
  port sono `PHY.MOD`, quindi non perturbabili. Vanno guardati per sito.

### 7. OBJ, shared memory del MAC

- `OBJ 0x0314` -> `OBJ 0x00cc` ×10 (valore `0x0045`).
- `OBJ 0x030a/0x030e/0x0310/0x0312` save/restore ×19; `0x0308/0x030c`
  riscritti con un'altra parola.
- `OBJ 0x099a ... 0x0a26` save/restore ×3, di cui il port ne consuma 2.
- `OBJ 0x0a3a/0x0a56/0x0a72/0x0a8e` save/restore ×2, `xor 0x00d0` ×1.
- `OBJ 0x0040` -> `0x05d4`, `0x0002` -> `0x0000`, `0x014e/0x07da/0x0782` ->
  `0x0048`.

Sono la finestra `0x0308-0x0314` del rumore e le celle di rate/power del
MAC; il port ne rilegge alcune (`phy_ac.c:9959`, `:10106`) e ne scrive
costanti. Fuori dal perimetro del PHY, ma la stessa natura.

## Ordine di lavoro: quali costanti sono gia' sbagliate altrove

Uno scartato e' innocuo finche' il valore che il port ha cablato e' lo stesso su
tutte le configurazioni, quindi il primo filtro e' la varianza: il valore che il
vendor legge, sui 26 segmenti a freddo, per ogni chiave scartata. Ma **la
varianza da sola produce falsi positivi**, e il secondo filtro non e'
sostituibile: bisogna guardare il sito del port. Le tre forme che la varianza
non distingue sono

- il port scrive una **costante** dove il vendor scrive una funzione del letto:
  e' il debito vero;
- il port scrive un valore che **deriva** da SROM, larghezza o canale, e la
  lettura del vendor e' un peek prima della propria scrittura calcolata: non c'e'
  niente da chiudere, anche se il valore varia;
- la scrittura che segue e' una `MOD`, che sul ferro preserva i bit non
  toccati: la lettura non ha nessun consumatore.

Delle sei chiavi che variano fra segmenti a valore stabile, l'audit per sito ne
ha promossa una sola:

| chiave | valori sui 26 | sito del port | verdetto |
| --- | --- | --- | --- |
| `RAD 0x0122` (+stride) | `0x4230/0x5030/0x5830` | `b43_radio_2069_afecal()` | costante da tabella BW: **chiusa**, vedi famiglia 5 |
| `PHY 0x06a1` (7 lunghi) | `0x004d/0x4f/0x50/0x51` | blocco D di `post_cal_finalize` | readback della **propria** scrittura: il primo apply scrive il coefficiente e il peek lo rilegge. La `copy -> OBJ 0x0a24` del censimento e' una coincidenza di valore -- `0x0a24` vale `0x0050` sia prima sia dopo la lettura. Non si chiude, e non va chiusa: prendere il letto per il secondo apply farebbe sparire dal punteggio il residuo del solver, che e' l'unico posto dove si vede |
| `PHY 0x0070` | `0x0100/0xe500` | blocco E | peek, e le nove op che seguono sono tutte `PHY.MOD`: sul ferro preservano i bit non toccati, quindi non c'e' un valore da consumare |
| `OBJ 0x099a`..`0x09fe`, `0x0a12/0x0a26` | 4-5 valori | `b43_phy_ac_prb_rsp_rate_po()` | il port calcola il valore da `mcsbw5g_po` dello SROM; la lettura e' il peek che il vendor fa prima. `rmw(=)` e' la coincidenza fra il calcolato e il letto a regime |
| `OBJ 0x0a3a/0x0a56/0x0a72/0x0a8e` | 6 valori | `b43_phy_ac_cck_rate_po()` | qui la tabella e' trascritta dal d6220 per davvero, ed e' dichiarata tale nel commento. Fuori priorita' per la regola dell'MVP: i valori CCK divergono fra freddo e caldo e la divergenza va annotata, non risolta ora |
| `OBJ 0x014e`, `0x07da` | 89 valori | shared memory del MAC | fuori dal perimetro del PHY, vedi famiglia 7 |

**Varia perche' e' una misura o un poll** -- scartare il valore e' corretto e non
c'e' niente da chiudere: `OBJ 0x0308/0x030c` (425 e 385 valori, il campione di
rumore), `PHY 0x0013` (`poll` x168), `PHY 0x0380` (`poll` x743), `PHY 0x0270`
(`poll` x123), `OBJ 0x0000/0x0002` (`0x55aa`/`0xaa55`, il self-test di
`b43_validate_chipaccess`).

**Costante su tutti e 26** -- fragile per board, non per canale, e sta in fondo
alla coda: il blocco `TBL 0x07[0x0362-0x0379]` a `0x0101`, `[0x03cd]/[0x03dd]`
a `0x0c02`, il write-verify `TBL 0x45/0x65/0x85[0x20]`, `RAD 0x0020/0x0022/
0x003a` piu' stride, `PHY 0x016c/0x0394/0x03a9/0x040f/0x06dd/0x08dd/0x0add/
0x0b3e`, `OBJ 0x024e-0x02ae`, `OBJ 0x030a/0x030e/0x0310/0x0312/0x0314`.

Il censimento e i verdetti si rifanno cosi':

```sh
GATE_TMP=/tmp/gate test/unit/gates.sh            # tiene /tmp/gate/merged
python3 reverse-tools/reads.py consumers /tmp/gate/merged > /tmp/census.txt
python3 test/unit/read_perturb.py /tmp/census.txt /tmp/gate/merged | sort
```

La varianza si legge invece dai 26 segmenti ripiegati, cercando i valori di ogni
chiave scartata. Senza `GATE_TMP` i file di lavoro di `gates.sh` stanno in una
directory temporanea per run e spariscono alla fine; con `GATE_TMP` la stessa
directory viene riscritta a ogni lancio, quindi un audit fatto dopo aver
lanciato il gate su un altro segmento legge i file di quel segmento.

## Cosa consumano gia' bene

224 letture cambiano l'uscita se perturbate: gli accumulatori RX-IQ, i
readback della cal TX IQ/LO (`0x0c[0x80-0x93]` -> `0x40-0x55`), il blocco D e
la coda del finalize, i LO DAC, le `OBJ 0x1d2/1d4/1d6 -> 0x1f2/1f4/1f6`, e le
RMW sui registri radio `0x000e/0x0017/0x001a/0x0049/0x004e/0x015f/0x0161`.

## Come chiuderle

L'ordine e' quello delle famiglie. Per ciascuna il lavoro e' lo stesso fatto
per le LOFT LUT: tenere il valore letto in uno stato con un nome, e scrivere
da quello -- copia, save/restore o somma -- al posto della costante. Il gate
su `cold01` non si muove per costruzione; la prova e' il perturbatore, che
deve passare da DISCARDED a CONSUMED, e i segmenti a caldo, dove le costanti
di `cold01` sono gia' sbagliate.
