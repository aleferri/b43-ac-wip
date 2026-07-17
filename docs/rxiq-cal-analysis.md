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

Evidenza a supporto: la misura con tone_mode=0 (call #4) è seguita
immediatamente dalla fase di precisione senza cambio di gain. Se
tone_mode=0 fosse "tono spento", la call #4 sarebbe una misura di noise
floor di riferimento, e le call #1–#3 isolerebbero ognuna una componente
del segnale di loopback. Questa struttura ha un diretto parallelo col
metodo rt2800, dove `g_imb` (guadagno) e `ph_rx` (fase) vengono
decomposti da misure separate di `sigma_i`, `sigma_q` e `r_iq`.

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
| Coefficienti | 2 per core (a, b) 10-bit, 0x9a-0x9d | 4 per core, 16-bit, 0x0720-0x0729 |
| Formato coeff | gain+fase fusi in a/b | gain e fase presumibilmente separati |

L'estimator HW (§readback 0x06c0) usa lo stesso layout dell'N-PHY — la
trace mostra lo stesso ordine di lettura hi-before-lo per le 3 coppie. La
differenza è a monte (sweep a 4 configurazioni) e a valle (4 registri
invece di 2).


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

### P1: tone_mode=0 è noise floor

Se tone_mode=0 spegne il tono, allora `i_pwr` e `q_pwr` alla call #4
devono essere ordini di grandezza più piccoli che alle call #1–#3, e
`iq_prod ≈ 0`.

Se falsificata (valori comparabili): tone_mode non è un interruttore
ma un selettore di fase, e tutte e 4 le misure vedono segnale.

### P2: i_pwr e q_pwr sono dello stesso ordine a tone_mode=4

Se il register-map è corretto (coppie hi/lo → i², q², iq come nell'N-PHY),
a tone_mode=4 (tono a piena potenza, entrambi i path) ci si aspetta
`i_pwr ≈ q_pwr` con differenza < 20% (lo squilibrio tipico di un chip è
pochi percento).

Se falsificata (uno dei due ~0): le coppie hi/lo sono assegnate in modo
sbagliato (swap i²↔iq o i²↔q²).

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
(`./rxiq_trace rxiq_comp agcombo`) inietta i 36 valori raw degli
accumulatori come read plan e verifica che il codice emetta esattamente
le sei scritture vendor, azzeramento iniziale compreso.
