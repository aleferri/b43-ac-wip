# DSL-3580L — catture

Board 4352, driver **6.30.102.7** (`cpe4.12L07.0`), kernel 2.6.30.
Flusso `down->up` per ogni canale, da `reverse-tools/capture_plan.sh`.

C'e' un solo `wl` per router, quindi quella versione vale per tutti i core che
il modulo serve, wl0 e wl1: `router_info.txt` la stampa una volta sola per
questo, non perche' descriva solo wl0.

E' anche la differenza che spiega la corsa di azzeramento sopra `0x1000`:
`0x10a4-0x1402` (432 word) qui contro `0x10f4-0x14b2` (480) sul d6220, che ha
lo stesso chip 4352, lo stesso `boardtype 0x668` e la stessa NVRAM sui campi
che contano, ma driver `7.14.89.14`. Non e' un dato di board: e' quello che il
driver piu' nuovo azzera in piu'. Il port segue il 7.14, che e' la versione del
segmento di riferimento del gate.

Armate con `skipphyrd="0x253,0x254"`: sui canali DFS il rivelatore radar
interroga quei due registri in continuo, fino all'85% di tutte le letture PHY,
e non c'entrano con la configurazione del canale. `0x251`/`0x252` sono tenuti.

| file | contenuto |
|---|---|
| `cold01-ch36-bw20.txt` | 41583 op, decodificata e coi RETVAL ripiegati. La cattura di riferimento |
| `out-cold-dsl-decodificata.txt` | 46318 op, la stessa corsa senza ripiegamento |
| `out-cold-dsl-merged.txt` | 41677 op, ripiegata |
| `full-sweep.zip` | 61 voci, 46 segmenti in `20a/`, `20b/`, `40/`, `80/`, piu' la decodificata intera per larghezza |
| `wl1_otp_dump.txt` | `wl -i wl1 otpdump`. Quasi tutto zero: le sole word non nulle nelle prime 0x50 sono `0x000c: 0x1b08`, `0x0020: 0x0500`, `0x003c: 0x4352 0x4001` |
| `wl1_srom_raw.txt`, `wl1_nvram.txt` | SROM grezza e NVRAM. Sui campi che contano l'NVRAM e' identica a quella del d6220, `boardtype 0x668` compreso; differisce solo `boardnum` |
| `wl1_phytable_5gl.txt` | dump delle tabelle PHY, banda 5 GHz bassa |
| `router_info.txt` | **descrive wl0**, il core N-PHY integrato nel 6362 (`chipnum 0x6362`, `corerev 0x16`, `phytype 0x4`): non contiene nessuna revinfo di wl1 |
| `dsl3580l_pmu-trace.txt` | traccia PMU |
| `bcm43b3_3580l_map.bin` | 480 byte, mappa |

**Le catture contengono l'attach di wl0.** `wl` fa l'attach di entrambi i core
a ogni caricamento, e gli hook sono sulle funzioni di `wl`, non per-core:
il pezzo iniziale di ogni cattura e' quindi dell'N-PHY di wl0. Su
`cold01-ch36-bw20.txt` la finestra e' `#295-#404`, chiusa da un salto di 2,27 s.

Si attribuisce con due criteri, non col salto da solo:

- **la cpu**: le op di wl0 sono `cpu0`, il bring-up del nostro core `cpu1`;
- **i registri nominati**: `0x0078`, `0x008f`, `0x00a5`, `0x00a6`, `0x00a7` sono
  `RFCTL_CMD`, `AFECTL_OVER1`, `AFECTL_OVER`, `AFECTL_C1`, `AFECTL_C2` in
  `b43/phy_n.h` e non stanno in `phy_common.h`, quindi sono N-PHY e di nessun
  altro. Il `clr 0x0400` su `0x0078` e' `RFCTL_CMD_CHIP0PU`, cioe' wl0 che
  porta giu' la radio dopo l'attach. In quella finestra le op RAD sono zero.

Ci cadono dentro `OTP.RDR`, `OTP.INIT`, `SROMCTL.RD/WR` e le due `PHY.WARR`:
**non sono nostre e non vanno portate.** Il port non emette nessuno dei cinque
registri, verificato confrontando i registri della sua traccia con quelli della
finestra.

**Il bring-up completo si vede solo su UNII-1.** I segmenti con `TBL.WR` ~5000
sono i canali 36-48 a 20 MHz e 36/44 a 40 MHz; tutti i DFS si fermano a ~143.
Sui DFS il driver non arriva ai caricamenti di tabella, coerente con la CAC
obbligatoria prima di trasmettere. Per la decorrelazione del setup completo
c'e' quindi solo UNII-1.

**Le due catture bw20/bw40 sostituiscono quelle precedenti**, fatte con una
build che non agganciava object memory, template RAM, chanspec e OTP: mancavano
~1600 op per cattura. Usarle come oracolo mostrava ogni `OBJ.WR` del port come
divergenza.

**I readplan dipendono dai tempi.** `readplan_0270_dsl` e' passato da 45 a 52
valori con la sostituzione: `0x0270` e' un poll sul bit 0 seguito da un peek, e
il numero di iterazioni dipende da quanto l'hardware tarda a completare. Nessuna
delle due sequenze e' piu' giusta. Il piano serve solo ai flussi **senza**
`AC_READ_ORACLE`; con l'oracolo resta a `iter=0`.
