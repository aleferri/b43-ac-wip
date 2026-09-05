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
| #5   | 0x4000 | Write dello stato di mute delle altre catene (vedi §4) |
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

La lettura "tono spento a tone_mode=0" e' **falsificata** dai valori misurati:
vedi §6, dove gli accumulatori a `0x0` sono cinque volte piu' grandi. Resta che
quattro configurazioni distinte con tre bit attivi sono troppo strutturate per
un interruttore on/off, che di call ne richiederebbe due.

## 4. Registri `0x0720`-`0x0729`: non sono coefficienti

Le quattro scritture per catena che seguono la prima misura di precisione sono
lo stato **save/restore del mute** delle catene non misurate, non coefficienti
di compensazione. `0x0720`-`0x0729` e' la pagina AFE -- gli stessi registri che
`switch_analog` salva nel preambolo di bring-up -- ed e' per questo che compare
50-60 volte per cattura.

I coefficienti veri sono **due per catena**, in `0x06a0`/`0x06a1` (+`core×0x200`),
formato s10 come sull'N-PHY, scritti 4-5 volte per cattura. Derivazione e
verifica bit-exact in §8.

## 5. Confronto col reference N-PHY (brcmsmac)

| Aspetto | N-PHY (`phy_n.c`) | AC-PHY (dalla trace) |
|---------|-------------------|----------------------|
| Misure per calibrazione | 1 | 4 (sweep) + 2 (precisione) |
| Variabile di sweep | nessuna | `0x0734` tone_mode {4,2,1,0} |
| Campioni per misura | `0x4000` fisso | `0x0400` sweep -> `0x4000` precisione |
| Registri accumulatore | `0x12b`/`0x12a`/`0x129` | `0x0272`/`0x0271`/`0x0270` |
| Layout risultati | 3 x 32b per catena (i2, q2, iq) | idem, confermato |
| Coefficienti | 2 per catena (a, b) 10 bit, `0x9a`-`0x9d` | 2 per catena, `0x06a0`/`0x06a1` + `core×0x200` |
| Formato coeff | gain e fase fusi in a/b | idem, confermato |

L'estimator hardware usa lo stesso layout dell'N-PHY, con lo stesso ordine di
lettura hi-prima-di-lo per le tre coppie. La differenza e' **a monte**: sweep a
quattro configurazioni invece di una misura sola. A valle no, e il solve in
`b43_phy_ac_rx_iq_comp_update` riproduce i coefficienti bit-exact.

## 6. Cosa i dati hanno escluso

Due letture sono state provate e vanno registrate perche' sono costate tempo.

**`tone_mode=0` non e' il noise floor.** Accumulatori della catena 0
nell'agcombo rescan, che ha i retval:

| tone_mode | i_pwr | q_pwr | iq_prod |
|---|---|---|---|
| `0x4` | 9 499 843 | 9 691 680 | 20 787 |
| `0x2` | 8 878 062 | 9 150 008 | -142 124 |
| `0x1` | 5 726 503 | 7 049 264 | -49 295 |
| `0x0` | **47 717 674** | **58 550 641** | 560 273 |

A `tone_mode=0` la misura e' **cinque volte piu' grande**, non ordini di
grandezza piu' piccola: e' la configurazione col segnale piu' forte delle
quattro. Nessuna delle quattro e' un noise floor, e il tono non si spegne.

**`tone_mode` non e' un codice di attenuazione.** Le quattro modalita' non sono
una scala monotona (9.5M, 8.9M, 5.7M, 47.7M per 4/2/1/0). Cosa selezionino resta
da stabilire.

Il mapping delle coppie hi/lo e' invece **confermato**: a `tone_mode=4` `i_pwr` e
`q_pwr` differiscono del 2%, e `iq_prod` e' tre ordini di grandezza piu' piccolo
in tutte e quattro le modalita' -- quel che si aspetta da un prodotto incrociato
su un chip poco squilibrato.

## 7. Cosa resta aperto

1. Riempire `rxcal_phy_setup` / `radio_setup` / `cleanup` (~300 op di RMW) a
   pezzi verificati col correlatore.
2. Determinare lo scopo dello sweep tone-mode. I coefficienti **non** ne
   dipendono numericamente -- li riproducono le sole misure di precisione -- ma
   il driver stock lo esegue sempre. Ipotesi: sanity check o warm-up; per
   discriminare servono catture con retval di un caso di fallimento.
3. Confermare la scala di `B43_PHY_AC_MIN_RXIQ_PWR`, mai esercitata: nelle
   catture le potenze sono ordini di grandezza sopra la soglia.
4. Arrotondamento a frazione esattamente 0.5, non presente nei tre vettori
   disponibili.

## 8. Verifica con la cattura agcombo a return value

Cattura: `router-data/agcombo/agcombo-wl1-4360-rescan-to-bss-ch36.txt`
(wl-diag "capture ret val", 6453 RETVAL), mergiata con
`reverse-tools/trace_filter.py --retvals`. Finestra cal: #29788–#32120.
Board agcombo = BCM4360 3x3: terza catena (registri 0x0aXX/0x0bXX)
attiva, a differenza di DSL-3580L/D6220.

### 8.1 Struttura confermata

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
   coefficienti", vedi §4), modificati per mutare il
   core, e ripristinati dopo la misura. Il gain (0x?25/0x?39/0x?3a)
   viene solo salvato e ripristinato: mai modificato.
4. **Solve + write-back** (#32043-32048): 2 coefficienti per core in
   0x?a0 (a) / 0x?a1 (b), s10.

### 8.2 Vettori misura → coefficienti

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
- **Mapping accumulatori** : +3,+2 =
  i², +5,+4 = q², +1,+0 = i·q, hi prima di lo. Con qualunque swap il
  solve non riproduce i coefficienti vendor.

### 8.3 Cosa i retval hanno chiuso

- `|iq|` sta sotto `0.07 · i2` in ogni punto, con segno (la catena 0 lo inverte
  fra i round): coerente con un prodotto incrociato.
- I registri `0x0720`-`0x0729` sono identici fra board **perche' non sono
  coefficienti** (vedi §4). I coefficienti veri, `0x?a0`/`0x?a1`, sono diversi
  per catena e quindi per istanza di silicio, come si aspetta da una
  calibrazione reale.
- Se la struttura sia stabile fra canali resta non indirizzato: questa cattura
  copre un canale solo.

### 8.4 Riscontro nel driver

`b43_phy_ac_rx_iq_comp_update` (src/rxiqcal_phy_ac.c) implementa il
solve confermato; il flow `rxiq_comp` del test harness
(`./ac_trace rxiq_comp agcombo`) inietta i 36 valori raw degli
accumulatori come read plan e verifica che il codice emetta esattamente
le sei scritture vendor, azzeramento iniziale compreso.

## 9. Banco 0x0910 / 0x0b10

Il banco viene scritto durante questa fase, ma non c'e' evidenza che consumi il
risultato della misura: sta qui per vicinanza nel flusso, non per dipendenza
dimostrata. Reperti strutturali, ipotesi escluse e la relazione con la soglia
CRS sono in [`bank-0910-analysis.md`](bank-0910-analysis.md), che e' il file
dedicato.

## 10. Stato reale del port: la calibrazione e' un replay

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
  §9, per esempio) studia valori che il port non calcola;
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

## 11. Ricerca del guadagno di loopback

Criterio del driver stock, ricostruito dai dati e implementato in
`b43_phy_ac_loopback_step` / `b43_phy_ac_loopback_gain_search`.

La potenza media per campione e' `round(ii/1024) + round(qq/1024)` (1024 = il
numero di campioni, `0x400`) e va portata nella finestra `[0xb57, 0x169e]` =
`[2903, 5790]`. Sotto si alza l'indice, sopra si abbassa, con clamp `[1,10]`.
L'indice di partenza e' 4 in 5 GHz e 0 in 2 GHz, e sta in `0x0734 + core*0x200`.

Sequenza letta intercalata alle misure (d6220 ch36 BW20, segmento 01):

| `0x734` | potenza | esito |
|---|---|---|
| 4 | 18823 | sopra |
| 2 | 15485 | sopra |
| 1 | 8480 | sopra |
| 0 | 4142 | **dentro**, si ferma |

Due correzioni che i dati impongono sulla descrizione di riferimento:

- il passo **non** e' `-1`: la sequenza 4, 2, 1, 0 e' un dimezzamento, coerente
  col fatto che altrove `0x734` sia mascherato con `0x7`, cioe' tre bit;
- l'indice arriva a 0, quindi il minimo su questo registro e' 0 e non 1.

La finestra invece e' confermata: 4142 vi cade dentro e le tre misure
precedenti sono tutte sopra.

Verificato su **tutti i 52 segmenti** dello sweep d6220: la simulazione del
criterio riproduce la sequenza di indici osservata. La convergenza dipende dalla
larghezza, e i dati la spiegano:

| BW | ultimo passo | esito |
|---|---|---|
| 20 | idx 1 -> ~8400 | sopra, scende a idx 0 e converge |
| 40 | idx 1 -> ~5650 | dentro, converge a idx 1 |
| 80 | idx 1 -> ~5700 | dentro, converge a idx 1 |

A banda larga la potenza per campione e' piu' bassa e la finestra si raggiunge
un passo prima. La scrittura di 0 che segue anche a 40/80 MHz **non** e' un
passo di ricerca: e' la chiusura, e a 80 MHz e' preceduta dalle misure della
seconda stima (valori attorno a 70000 e 40000, col loopback smontato).
Confondere le due cose fa sembrare che il criterio sbagli su 5 segmenti.

**Non coperto dai dati:** cosa faccia il driver stock se a indice 0 la potenza
e' ancora sopra la finestra, o al massimo e' ancora sotto. Il port esce
accettando l'indice estremo -- conservativo, non un ciclo infinito e non un
valore fuori campo -- ma non e' misurato.

Con `AC_READ_ORACLE` il port riceve le stesse letture del driver stock e
converge sugli stessi indici senza hardware.

## 12. Measure block: struttura e dipendenza dalla larghezza

Blocco ricorrente (393 op) eseguito dopo ogni probe cycle della finalize, in
nove sotto-blocchi: RX AFE per-core reconfig (86), radio 2069 second IQ-cal
(68), rxcal cleanup preamble (5), tail perchan (18), arm tone gen (3), poll
blocks TX AFE (163), reset gain regs PHY (29), radio reset (14), finalize (5),
piu' 2 MAC toggle di arm.

Non e' esclusivo della cal RXIQ: a regime il driver stock lo riesegue tal quale
dentro il tick periodico ~5 s (`b43_phy_ac_watchdog`), con lo stesso
inquadramento interno e **senza** i due MAC toggle finali. E' quindi la tornata
di misura rumore/RSSI del PHY, usata dalla finalize in 4 round convergenti e dal
watchdog in round singoli.

I 4 campi gain dell'arming RX sono selezionati dalla **larghezza**, identici fra
fase di cal e tick periodico. Misurato sullo sweep d6220 (52 segmenti,
26 configurazioni):

| campo | BW20 | BW40 | BW80 |
|---|---|---|---|
| `0x?73a` mask `0x0007` | `0x0003` | `0x0002` | `0x0000` |
| `0x?739` mask `0x007e` | `0x007a` | `0x007a` | `0x007e` |
| `0x?73a` mask `0x0008` | `0x0000` | `0x0000` | `0x0008` |
| `0x?73a` mask `0x0060` | `0x0040` | `0x0000` | `0x0000` |

Il port cabla la colonna BW20, coerente col fatto che `switch_channel` rifiuta
BW40/80. Le altre colonne vanno prese da qui quando il supporto arrivera'.

## 13. Tabelle `0x0042`/`0x0062`/`0x0082`: coefficienti TX IQ/LO, non default di gain

Le tre tabelle riempite da `b43_phy_ac_rxcal_afe_finalize_gain_luts` non sono
default di gain e non c'e' una formula da trovare: sono i coefficienti di
compensazione TX IQ/LO, popolati nel driver stock da
`wlc_phy_populate_tx_loft_comp_tbl_acphy` (696 B) e prodotti da
`wlc_phy_cal_txiqlo_acphy` (13144 B) -- identificati dai **simboli del blob**,
non per inferenza.

I valori cambiano a ogni corsa perche' sono stato accumulato. In brcmsmac
(`wlc_phy_cal_txiqlo_nphy`, GPL-2.0 e in-tree) la scelta e':

    if (!fullcal && coeffsvalid)  ->  nphy_txiqlocal_bestc
    else                          ->  tbl_tx_iqlo_cal_startcoefs

Una ricerca sistematica non trova la sorgente nelle catture: 119592
trasformazioni su 3624 sorgenti (ogni valore letto e scritto nel segmento e in
quello precedente), nessuna relazione nemmeno al 50%. Coerente con "e' stato
interno", non con "e' derivato da una lettura".

Sul core 2 del d6220 i valori sono fuori scala -- `0x8bed` = `(-117,-19)` come
coppia di byte con segno, contro `(-1,1)..(2,1)` sui core collegati e `(-6,-3)`
sul core 2 dell'agcombo, che ha tre catene: senza catena RF la cal non ha
segnale da annullare. Il DSL con wl 6.30 scrive zero sui core oltre il primo, il
7.14 no.

Cablare questi numeri e' sbagliato su una board a tre catene, dove contano. Il
primo stadio della cal -- la ricerca del guadagno di loopback, §11 -- e'
implementato e collegato.
