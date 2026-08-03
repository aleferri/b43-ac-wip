# DSL-3580L — catture

Board 4352, driver **6.30.102.7** (`cpe4.12L07.0`), kernel 2.6.30.
Flusso `down->up` per ogni canale, da `reverse-tools/capture_plan.sh`.

Armate con `skipphyrd="0x253,0x254"`: sui canali DFS il rivelatore radar
interroga quei due registri in continuo, fino all'85% di tutte le letture PHY,
e non c'entrano con la configurazione del canale. `0x251`/`0x252` sono tenuti.

| cattura | righe | RETVAL | TBL.WR | note |
|---|---|---|---|---|
| `wl-diag-wl1-down-to-bss-ch100-bw20.txt` | 37007 | 21354 | 143 | UNII-2e/3, breve |
| `wl-diag-wl1-down-to-bss-ch116-bw80.txt` | 88600 | 72893 | 146 | UNII-2e/3, breve |
| `wl-diag-wl1-down-to-bss-ch140-bw20.txt` | 47023 | 36797 | 143 | UNII-2e/3, breve |
| `wl-diag-wl1-down-to-bss-ch36-bw20.txt` | 39952 | 3984 | 5026 | UNII-1, **completo** |
| `wl-diag-wl1-down-to-bss-ch36-bw40.txt` | 41231 | 3914 | 5026 | UNII-1, **completo** |
| `wl-diag-wl1-down-to-bss-ch48-bw20.txt` | 39924 | 3980 | 5026 | UNII-1, **completo** |
| `wl-diag-wl1-down-to-bss-ch52-bw20.txt` | 26259 | 10570 | 143 | UNII-2, breve |

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
