# AC-PHY RXIQ calibration — analisi dalla trace

Analisi del blocco di calibrazione RX-IQ osservato nelle trace `wl` OEM per
BCM4352 AC-PHY rev 1 / radio 2069 rev 4. Trace di riferimento: DSL-3580L
ch36 (`ch36-annotated.txt`, episodio #50526–52448) e D6220 ch36
(`d6220-trace2-annotated.txt`, episodio #82271–84501).


## 1. Localizzazione nella sequenza `down → bss-up`

Il blocco RXIQ compare una sola volta nel percorso deterministico, dopo
`txpwr_by_index` e prima di `rxgainctrl_regs`:

```
... txpwrctrl_setup → txpwr_by_index → rx_gate(release)
───── RXIQ block (#50526 – #52213, DSL ch36) ─────
rxgainctrl_regs → rx_enable → mac_enable
```

Il pattern è identico nella trace D6220 (#82271–#84501), confermando che
non è specifico della board.


## 2. Struttura del blocco: due fasi distinte

Dalla trace emergono **6 invocazioni** dell'estimator HW (registro di
comando 0x0270), divise in due fasi nette.

### Fase 1 — sweep a 4 configurazioni, 1024 campioni ciascuna

| Call | SAMCNT (0x0272) | 0x0734 C0 | 0x0934 C1 | 0x0b34 C2 |
|------|-----------------|-----------|-----------|-----------|
| #1   | 0x0400          | 4         | 4         | 4         |
| #2   | 0x0400          | 2         | 2         | 1         |
| #3   | 0x0400          | 1         | 1         | 0         |
| #4   | 0x0400          | 0         | 0         | —         |

L'unica variabile indipendente tra le 4 misure è il registro 0x0734 (e i
corrispondenti per core 1/2 a stride +0x200). Frequenza del tono (0x0730 =
0x00b0), ampiezza massima (0x0731 = 0x0004) e gain di misura (0x0739 =
0x007a, 0x073a = 0x01d3, 0x0725 = 0x07e2) sono **identici** in tutte e 4
le call.

### Fase 2 — misura di precisione, 16384 campioni

| Call | SAMCNT | Cosa succede dopo |
|------|--------|-------------------|
| #5   | 0x4000 | Write di coefficienti di compensazione (vedi §4) |
| #6   | 0x4000 | Ri-write degli stessi coefficienti (verifica)     |

Le call #5 e #6 hanno il tone-mode fermo a 0 (invariato dalla call #4).

### Fase 3 — cleanup

Ripristino dei registri di compensazione ai valori default e spegnimento
del tone engine (freq=0, fmax=0, tone=0).


## 3. Register 0x0734: un selettore di configurazione, non un parametro continuo

I valori osservati per core 0 e 1 sono {4, 2, 1, 0}. In binario: {100, 010,
001, 000}. Core 2 segue una sequenza diversa: {4, 1, 0, —}.

Osservazioni:

- I valori sono discreti e a un solo bit attivo (tranne 4 = 100). Non
  compaiono mai valori intermedi (3, 5, 6, 7).
- Il range è piccolo (3 bit). Non è plausibile come frequenza, ampiezza, o
  angolo continuo.
- Core 2 salta il valore 2 e termina un passo prima, il che suggerisce che
  il significato dipende dalla topologia delle catene RF.

**Interpretazione più probabile**: il campo seleziona quale path del
loopback interno è attivo per la misura. Ogni valore isola un contributo
diverso al segnale ricevuto (es. accoppiamento da una catena specifica),
permettendo all'algoritmo di decomporre lo squilibrio IQ in componenti
ortogonali.

Osservazione: la misura con tone_mode=0 (call #4) è seguita immediatamente
dalla fase di precisione senza cambio di gain.

L'ipotesi che tone_mode=0 fosse "tono spento" — e quindi la call #4 una misura
di riferimento del rumore, con le #1–#3 a isolare le componenti del loopback —
è stata **falsificata** dai valori misurati (vedi P1 in §7): a tone_mode=0 gli
accumulatori sono cinque volte più grandi, non più piccoli. Il parallelo col
metodo rt2800, dove `g_imb` e `ph_rx` si decompongono da misure separate di
`sigma_i`, `sigma_q` e `r_iq`, resta una analogia possibile per la struttura a
quattro misure, ma non poggia più su quella lettura di tone_mode.

**Evidenza contraria da escludere**: se tone_mode fosse un semplice
on/off, basterebbero 2 call (tono acceso + tono spento), non 4 con valori
distinti. Il pattern a 4 step con 3 bit attivi è troppo strutturato per
un interruttore.


## 4. Coefficienti di compensazione: write-back e formato

**SUPERATO — vedi §9.** La cattura agcombo con retval mostra che questi 4
registri non sono coefficienti: sono lo stato save/restore del mute dei
core non misurati. I coefficienti veri sono 2 per core in 0x?a0/0x?a1,
formato s10 come N-PHY. Il testo sotto è conservato come storia
dell'ipotesi.

Dopo la call #5 (prima misura a 16384 campioni) il blob scrive 4 registri
per core:

| Registro       | Valore calibrato | Valore default (pre/post cal) |
|----------------|------------------|-------------------------------|
| 0x0720 / 0x0920 | 0x0182         | 0x0180                        |
| 0x0721 / 0x0921 | 0x7761         | 0x5000                        |
| 0x0728 / 0x0928 | 0x0080         | 0x0880                        |
| 0x0729 / 0x0929 | 0x0321         | 0x1000                        |

Osservazioni:

- I valori sono **identici** per entrambi i core e identici tra le due
  trace (DSL e D6220). Due board diverse, stesso chip: i coefficienti
  dipendono dal silicio, non dalla board.
- Il delta dal default è minuscolo per 0x0720 (+2) e molto grande per
  0x0721 (+0x2761) e 0x0728 (–0x0800). Questo è incompatibile con un
  singolo formato `(a, b)` a 10 bit come nell'N-PHY.
- I 4 registri (non 2) per core suggeriscono che il formato AC-PHY
  codifica gain e fase separatamente, non fusi in `a`/`b`.
- Dopo la call #6 (verifica), gli stessi valori vengono ri-scritti: la
  seconda misura conferma la prima.


## 5. Confronto col reference N-PHY (brcmsmac)

| Aspetto | N-PHY (`phy_n.c`) | AC-PHY (dalla trace) |
|---------|-------------------|----------------------|
| Misure per calibrazione | 1 | 4 (sweep) + 2 (precisione) |
| Variabile di sweep | nessuna (misura singola) | 0x0734 tone_mode {4,2,1,0} |
| Campioni per misura | 0x4000 (fisso) | 0x0400 (sweep) → 0x4000 (precisione) |
| Registri accumulatore | 0x12b/0x12a/0x129 (cmd) | 0x0272/0x0271/0x0270 (cmd) |
| Layout risultati | 3 × 32b per core (i²,q²,iq) a 0x06c0+core×0x200 | idem, confermato |
| Coefficienti | 2 per core (a, b) 10-bit, 0x9a-0x9d | **2 per core (a, b) 10-bit, 0x06a0/0x06a1 + core×0x200** |
| Formato coeff | gain+fase fusi in a/b | idem, confermato |

L'estimator HW (§readback 0x06c0) usa lo stesso layout dell'N-PHY — la
trace mostra lo stesso ordine di lettura hi-before-lo per le 3 coppie. La
differenza è a monte: sweep a 4 configurazioni invece di una misura sola.
A valle no: i coefficienti sono due per core come sull'N-PHY, e il solve in
`b43_phy_ac_rx_iq_comp_update` li riproduce bit-exact contro l'agcombo.

La riga precedente diceva "4 per core, 16-bit, 0x0720-0x0729". È sbagliata:
`0x0720-0x0729` è la pagina AFE — sono i registri che `switch_analog` salva nel
preambolo di bring-up — e nelle catture compare 50-60 volte per quel motivo,
non come coefficienti. I coefficienti sono a `0x06a0`/`0x06a1` (+`core×0x200`),
scritti 4-5 volte per cattura, una per aggiornamento.


## 6. Ipotesi dell'algoritmo completo

**SUPERATO — vedi §9** per l'algoritmo confermato dai retval: compute()
consuma solo le misure di precisione per-core (con gli altri core mutati),
non lo sweep, e produce 2 coefficienti s10 per core in 0x?a0/0x?a1.

Dalla struttura della trace si ricostruisce:

```
rxiq_cal(dev):
    save gain/tone/comp registers

    arm tone engine (tx_tone)
    apply gain override (0x0739/0x073a/0x0725)

    for tone_mode in {4, 2, 1, 0}:
        set 0x0734[core] = tone_mode     ← sweep
        est[tone_mode] = rxiq_est(1024 samples)

    coeffs = compute(est[4], est[2], est[1], est[0])   ← chiave

    write coeffs to 0x0720-0x0729 per-core
    verify = rxiq_est(16384 samples)
    if verify OK:
        re-write coeffs (conferma)
    else:
        retry or fallback

    restore registers
```

La funzione `compute()` riceve 4 set di accumulatori (i², q², iq) — uno
per configurazione di loopback — e ne estrae coefficienti di compensazione
a 4 componenti. Il fatto che siano 4 misure in input e 4 registri in
output suggerisce una relazione diretta: ogni misura contribuisce
prevalentemente a uno dei 4 coefficienti.


## 7. Predizioni falsificabili

Il debug helper (`b43_phy_ac_rxiq_est_debug`) logga i 3 accumulatori per
core a ogni tone_mode. Le seguenti predizioni discriminano tra questa
ipotesi e le alternative.

### P1: tone_mode=0 è noise floor — **FALSIFICATA**

Gli accumulatori del core 0 nella cattura agcombo rescan (che ha i retval):

| tone_mode | i_pwr | q_pwr | iq_prod |
|---|---|---|---|
| 0x4 | 9 499 843 | 9 691 680 | 20 787 |
| 0x2 | 8 878 062 | 9 150 008 | −142 124 |
| 0x1 | 5 726 503 | 7 049 264 | −49 295 |
| 0x0 | **47 717 674** | **58 550 641** | 560 273 |

A tone_mode=0 la misura e' **cinque volte piu' grande**, non ordini di
grandezza piu' piccola. Il tono non e' spento: quella e' la configurazione con
il segnale piu' forte delle quattro. Nessuna delle quattro misure e' un noise
floor.

Nota anche che le quattro modalita' non sono una scala monotona di ampiezza
(9.5M, 8.9M, 5.7M, 47.7M per 4/2/1/0), quindi `tone_mode` non e' un codice di
attenuazione. Cosa selezioni resta da stabilire.

### P2: i_pwr e q_pwr sono dello stesso ordine a tone_mode=4 — **CONFERMATA**

A tone_mode=4: `i_pwr` = 9 499 843, `q_pwr` = 9 691 680, differenza **2%**,
dentro il <20% previsto. E `iq_prod` e' tre ordini di grandezza piu' piccolo in
tutte e quattro le modalita', come si aspetta da un prodotto incrociato su un
chip poco squilibrato.

Il mapping delle coppie hi/lo e' quindi corretto, coerentemente con quanto dice
`b43_phy_ac_rx_iq_comp_update`, il cui solve riproduce bit-exact i coefficienti
che il driver stock scrive.

### P3: iq_prod è signed e piccolo rispetto a i_pwr

Nella formulazione standard, `iq_prod = Σ(I·Q)` è proporzionale a
sin(φ) dove φ è l'errore di fase. Per mismatch tipici (< 5°),
|iq_prod| < 0.1 · i_pwr. Se il bit 31 del valore ricostruito
(hi<<16|lo) indica il segno, il risultato è coerente.

Se falsificata (|iq_prod| > i_pwr): il registro letto come iq_prod è in
realtà i² o q², e quello letto come i² è iq_prod.

### P4: i coefficienti non dipendono dalla board

Le due trace (DSL-3580L e D6220) producono gli stessi coefficienti calibrati
(0x0182, 0x7761, 0x0080, 0x0321). Se il debug helper su una terza board
dello stesso stepping (es. agcombo BCM4360) produce valori diversi ma con la
stessa struttura (4 write per core, delta piccolo su 0x0720, delta grande su
0x0721), l'algoritmo è confermato come chip-dipendente, non board-dipendente.

### P5: la sequenza è stabile tra canali

Se la struttura {4 sweep + 2 precision} si ripete identica su un canale
diverso (es. ch44) ma con coefficienti numericamente diversi, la
calibrazione è channel-dependent come atteso per un compensatore IQ.

Se falsificata (stessi coefficienti su canali diversi): la cal è
one-shot e i coefficienti sono una proprietà del chip, non del canale.
Il decorrelation CSV classifica tutti questi registri come `DYN-runtime`,
coerente con channel-dependence, ma non conclusivo senza valori.


## 8. Prossimi passi

**AGGIORNATO dopo §9.** Restano aperti:

1. Riempire rxcal_phy_setup/radio_setup/cleanup (bulk ~300 op RMW,
   #82151-82478 su d6220) a pezzi verificati col correlatore.
2. Determinare lo scopo dello sweep tone-mode {4,2,1,0}: i coefficienti
   non ne dipendono numericamente (riprodotti dalle sole misure di
   precisione), ma il blob lo esegue sempre. Ipotesi: sanity/linearity
   check software o warm-up; servono catture con retval di un caso di
   fallimento per discriminare.
3. Confermare la scala di B43_PHY_AC_MIN_RXIQ_PWR (mai esercitata:
   nelle catture le potenze sono ordini di grandezza sopra).
4. Osservare il comportamento di arrotondamento a frazione esattamente
   0.5 (non presente nei 3 vettori disponibili).


## 9. Verifica con la cattura agcombo a return value

Cattura: `router-data/agcombo/agcombo-wl1-4360-rescan-to-bss-ch36.txt`
(wl-diag "capture ret val", 6453 RETVAL), mergiata con
`reverse-tools/merge_retvals.py`. Finestra cal: #29788–#32120.
Board agcombo = BCM4360 3x3: terza catena (registri 0x0aXX/0x0bXX)
attiva, a differenza di DSL-3580L/D6220.

### 9.1 Struttura confermata

10 run dell'estimator (start su 0x0270), in tre fasi:

1. **Azzeramento coeff** (#28801-28806, prima della finestra): WR
   0x?a0/0x?a1 = 0 per i 3 core.
2. **Sweep** (it0–it3): 4 misure a 0x0400 campioni, tutti i core insieme,
   tone_mode 0x?34 = {4, 2, 1, 0}. Unica variabile tra le iterazioni:
   tone_mode (diff strutturale: 6 op su 322). Le potenze scendono
   monotonicamente (~0x91 → 0x2b ·10⁴ su core 0) ma nessun valore
   derivato da queste misure viene scritto.
3. **Precisione per-core** (it4–it9): 2 round × 3 core a 0x4000 campioni.
   Per ogni misura, i registri 0x?20/0x?21/0x?28/0x?29 degli ALTRI due
   core vengono letti (retval: 0x0182/0x7761/0x0080/0x0321 — i "4
   coefficienti" della vecchia ipotesi §4), modificati per mutare il
   core, e ripristinati dopo la misura. Il gain (0x?25/0x?39/0x?3a)
   viene solo salvato e ripristinato: mai modificato.
4. **Solve + write-back** (#32043-32048): 2 coefficienti per core in
   0x?a0 (a) / 0x?a1 (b), s10.

### 9.2 Vettori misura → coefficienti

Somma dei due round per core (hi<<16|lo dagli accumulatori):

| Core | ii (Σ i²) | qq (Σ q²) | iq (Σ i·q) | a scritto | b scritto |
|------|-----------|-----------|------------|-----------|-----------|
| 0 | 0x05af2b1e | 0x06f83860 | +243638 | 0x3fd (−3) | 0x06e (110) |
| 1 | 0x05e53106 | 0x06a37eb1 | −7911361 | 0x052 (82) | 0x03c (60) |
| 2 | 0x043ccde7 | 0x051ec43f | −10145289 | 0x092 (146) | 0x05c (92) |

Solve confermato bit-exact su tutti e sei i valori:

```
a = round(−iq · 2¹⁰ / ii)
b = round(√(qq/ii · 2²⁰ − a²)) − 2¹⁰
```

Dettagli discriminati dai vettori:

- **Somma dei round, non media dei coefficienti**: core 0 dà a=+7
  (round 1) e a=−12 (round 2); il vendor scrive −3 = solve(Σ round).
- **Arrotondamento al più vicino, non floor/troncamento N-PHY**:
  core 0 a=−2.61→−3 (trunc darebbe −2), core 0 b=109.97→110 e core 1
  b=59.83→60 (floor darebbe 109 e 59). Il fixed-point brcmsmac
  produce ±1 su 3 valori su 6.
- **Mapping accumulatori** (chiude il DA VERIFICARE storico): +3,+2 =
  i², +5,+4 = q², +1,+0 = i·q, hi prima di lo. Con qualunque swap il
  solve non riproduce i coefficienti vendor.

### 9.3 Risoluzione delle predizioni (§7)

- **P1 falsificata**: a tone_mode=0 le potenze sono ~3.4× sotto
  tone_mode=4, non ordini di grandezza: tutte e 4 le misure vedono
  segnale; tone_mode è un selettore di ampiezza/configurazione, non un
  interruttore. La misura di precisione avviene con tone_mode=0.
- **P2 confermata**: a tone_mode=4, i²≈q² entro il 2-6% su tutti i core.
- **P3 confermata**: |iq| < 0.07·i² ovunque, con segno (core 0 lo
  inverte tra i round).
- **P4 riformulata**: i registri 0x0720-0x0729 sono identici tra board
  perché NON sono coefficienti (sono config preesistente, save/restore).
  I coefficienti veri (0x?a0/0x?a1) sono diversi per core e quindi
  per istanza di silicio, come atteso da una calibrazione reale.
- **P5 non indirizzata** da questa cattura (singolo canale).

### 9.4 Riscontro nel driver

`b43_phy_ac_rx_iq_comp_update` (src/rxiqcal_phy_ac.c) implementa il
solve confermato; il flow `rxiq_comp` del test harness
(`./ac_trace rxiq_comp agcombo`) inietta i 36 valori raw degli
accumulatori come read plan e verifica che il codice emetta esattamente
le sei scritture vendor, azzeramento iniziale compreso.

## 8. Banco 0x0910 / 0x0b10

Il banco viene scritto durante questa fase, ma non c'e' evidenza che consumi il
risultato della misura -- vedi "Cosa non e' il valore" sotto. Sta qui per
vicinanza nel flusso, non per dipendenza dimostrata.

Struttura accertata: N-1 banchi per N core, uno per coppia adiacente, collocati
nella pagina del core di indice piu' alto -- `0x0910` per la coppia (0,1),
`0x0b10` per la (1,2). `0x0710` non esiste su nessuna board, quindi non e' una
grandezza per-core. Quattro registri per banco, lo stesso scalare in entrambi i
byte, ordine pari hi-poi-lo e dispari lo-poi-hi. Write-only: il driver stock non
lo rilegge mai.

### Cosa non e' il valore

Ipotesi **respinta**: `b // 10`, dove `b` e' il coefficiente RXIQ comp del core
alto della coppia. Correlava su 10 punti di 13, ma i tre controesempi la
smentiscono da entrambi i lati:

    ch36    #52556   banco = 0   con b del core 1 = 55, scritto a #52255
    DSL     #155080  banco = 0   con b del core 1 = 48, scritto a #154855
    d2u     #3155    banco = 3   con tutti i coefficienti a zero

Non e' ordine di esecuzione: nella ch36 e nel DSL il coefficiente e' scritto
centinaia di episodi prima del banco. La correlazione era temporale -- banco e
coefficienti partono da zero e crescono insieme.

Altri candidati esclusi provando contro le catture:

- il base index idle-TSSI (`0x0645 + core*0x200`): sul d6220 darebbe la
  differenza giusta fra core (5) ma sull'agcombo -10 dove il banco vale 6, e sul
  DSL -5 dove vale 0;
- ogni coppia di registri per-core `0x06xx`/`0x08xx` letta o scritta prima del
  banco, provate tutte automaticamente;
- i campi SROM per-core (`pdoffset*`, `rxgainerr*`): identici su tutte e tre le
  board, quindi darebbero lo stesso valore su tutte;
- `0x073d + core*0x200`, che viene letto per core ma legge **zero in tutte e
  quattro le catture**.

Il port lo scrive come costante (0 sul 4352, 0xf sul 4360) e riproduce solo la
prima occorrenza di alcune catture. La funzione si chiamava
`noise_floor_clear`, nome senza fonte, ora `prog_bank_0910`.

### Cross-interferenza fra core: ipotesi non sostenuta

L'indirizzamento N-1 per coppie adiacenti suggerisce una compensazione fra core,
ma tre fatti la contraddicono.

Primo: il driver stock scrive **lo stesso scalare in tutti e otto i campi byte**
del banco. Un coefficiente di accoppiamento e' complesso, con modulo e fase, e
per direzione: se il banco portasse fasori i campi sarebbero diversi. Uno
scalare uniforme e' un azzeramento o una soglia.

Secondo: **non esiste una misura di leakage fra core nella cattura**. Gli arm del
tono (`0x0394` = `0x0110`/`0x0111`/`0x0112`, indice di core) stanno a
`#11930-#17774`; le letture degli accumulatori a `#29862-#31149`, dodicimila
episodi dopo, in una fase separata. Non c'e' nessun punto in cui si legge
l'accumulatore di un core mentre un altro core ha il tono.

Terzo: nessuna espressione geometrica sui coefficienti riproduce i valori.
Testate su tre punti (`nf` = 6, 9, 5):

    |v_high|                  101.6  172.6   72.0
    |v_low|                   110.0  101.6   77.7
    |v_high - v_low|           98.6   71.6   33.8
    |v_high| - |v_low|         -8.4   71.0   -5.7
    atan2(a,b) gradi           53.8   57.8  -37.7
    delta fase gradi           55.4    4.0  -25.8
    rapporto moduli x10         9.2   17.0    9.3
    sqrt(|prodotto scalare|)   79.7  132.3   71.0

La piu' vicina, `|v_high| / 17`, da' 6, 10, 4. L'unica che torna esatta resta
`b_high // 10`, che e' aritmetica e non geometrica -- il che e' evidenza
**contro** la lettura come accoppiamento, non a favore di una formula.

Nota metodologica: l'ipotesi cross-core e' rimasta a lungo in piedi sul solo
indirizzamento, e diversi tentativi hanno adattato numeri a quella premessa
senza verificarla. Prima di ripartire da qui, stabilire cosa sia il banco.

## 9. Stato reale del port: la calibrazione e' un replay

Il solve dei coefficienti esiste e la sua matematica e' verificata bit-exact
(`b43_phy_ac_rx_iq_comp_update`), ma **non viene mai invocato**: zero marker
`FN` nel flow completo. L'unico chiamante e' `b43_phy_ac_rxiqcal()`, che ritorna
subito perche' il suo register-map non e' compilato.

I coefficienti che il port scrive sono costanti:

    src/phy_ac.c  static const u16 coeffs[2][2] = {
                      { 0x03f2, 0x004c },   core 0
                      { 0x03db, 0x0037 },   core 1
                  };

e non combaciano con la cattura di attach del d6220, che scrive `0x03ef`,
`0x004d`, `0x03d4`, `0x003b`. Sono vicini ma diversi di pochi LSB, coerente con
l'essere presi da una cattura diversa: i coefficienti sono **misurati**, quindi
variano di sessione in sessione.

Conseguenze:

- il gate `switch_channel` non verifica la calibrazione, verifica che il port
  riproduca le op di *una* sessione. Su qualunque altra sessione i coefficienti
  divergerebbero, e con loro tutto cio' che li consuma;
- ogni indagine su grandezze *derivate* dai coefficienti (il banco 0x0910 in
  §8, per esempio) studia valori che il port non calcola;
- collegare il solve e' la modifica che trasforma questa fase da replay a
  calibrazione, e **non** rompe il confronto op-per-op: lo rafforza. L'oracolo
  serve le stesse letture del driver stock, quindi dallo stesso ingresso il
  solve deve produrre lo stesso coefficiente, e il valore combacerebbe per
  calcolo invece che per copia.

### Verifica offline del solve sui valori d'attach

Eseguito a mano sui valori che l'oracolo serve nella cattura d6220
attach-to-bss-up, sommando i **due** round di stima come il codice documenta:

    core 0 (accum 0x06c0-0x06c5)   a = -17   b = 77   ->  0x3ef  0x04d
    core 1 (accum 0x08c0-0x08c5)   a = -44   b = 59   ->  0x3d4  0x03b

Il driver stock scrive `0x3ef 0x04d` e `0x3d4 0x03b`. **Bit-exact su entrambi i
core.** Con un solo round di stima il core 0 darebbe `a = -19, b = 76`, quindi
la somma dei due round e' anche l'aggregazione giusta.

Il che significa che collegare il solve e' una modifica a rischio zero sul
gate: sostituisce due costanti con un calcolo che, sugli stessi ingressi,
produce le stesse uscite -- e che a differenza delle costanti funziona anche su
un'altra sessione.

Il register-map che manca a `b43_phy_ac_rxiqcal()` resta l'unico ostacolo al
percorso completo, ma non serve per questo passo: gli accumulatori sono gia'
letti da `iqcal_meas_post_dds_apply_v2` subito prima di
`rxiq_apply_coefficients`, che e' dove stanno le costanti.
