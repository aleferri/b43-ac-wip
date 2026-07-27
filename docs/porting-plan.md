# Piano di porting

## Criterio di correttezza

Il port è corretto quando riproduce **op per op** la sequenza del driver stock,
non quando raggiunge una percentuale di copertura. Le metriche aggregate del
correlatore sono servite a orientarsi all'inizio e non sono più il criterio:
dicono quante op somigliano a qualcosa, non se il driver fa la cosa giusta
nell'ordine giusto.

Gli oracoli sono le catture in `router-data/`. La cattura di riferimento è
`d6220/wl-diag-wl1-attach-to-bss-up-ch36-bw20.txt`: parte da episodio 1, ha i
valori di ritorno (`RETVAL`, da ripiegare con `reverse-tools/merge_retvals.py`)
ed è sul chip di target. Le catture `down→bss` restano utili per la fase di
bring-up successivo, che è una fase distinta — vedi *Fasi* sotto.

## Gate attivi

Da tenere verdi a ogni modifica. Comandi in `README.md` e `test/README.md`.

| gate | oracolo | atteso |
|---|---|---|
| `set_channel` d6220 ch36 BW20 | `attach-to-bss-ch36`, range `32887:55154` | MATCH (22268/22268) |
| `tables_init` | `attach-to-bss-up`, primo bring-up | 3714/3714, zero gap |
| `rfkill` d6220 e agcombo | `down-to-bss` | 100% per funzione, zero gap |
| `init_regs` primo bring-up | `attach-to-bss-up` | 17/17 |
| `init_regs` bring-up successivo | `down-to-bss` | 17/17 d6220, 33/33 agcombo |
| SROM rev 11 | `sprom-rev11/harness` | 77/74/75 PASS, 0 FAIL |
| build | `-O2 -Wall -Wextra` | zero warning |

Il flow `full` (`./ac_trace full d6220`) esegue la catena continua nell'ordine
di `b43_phy_init` — `switch_analog` → `software_rfkill` → `ops->init` →
`switch_channel` — e si confronta sull'intero range. Non è ancora un gate: la
prima divergenza è a `@40` e serve a misurare l'avanzamento, non a bloccare.

## Fasi, e perché contano

Il driver stock ha **due percorsi** dove b43 ne ha uno: attach da freddo e
`down→up`. La stessa funzione scrive costanti diverse nelle due fasi, e diverse
regressioni sono nate dal confondere le due cose con una differenza di chip.

b43 la distinzione la ha già: `phy->do_full_init`, `true` fino al primo
`ops->init` riuscito, gestito da `b43_phy_init()` — la N-PHY lo usa allo stesso
modo per le sue tabelle statiche. Il port lo usa per la coppia di gain ADC in
`init_regs`, per il campo di `0x02e4` e per il preambolo analogico.

Regola operativa: prima di attribuire una divergenza al chip, verificare che
non sia la fase. Prima di attribuirla alla board, verificare che non sia la
versione del blob.

## Cosa resta

### Trascrizione — determinato dalle catture

| blocco | dove | note |
|---|---|---|
| Blocco PMU/GPIO di attach | fra preambolo analogico e corpo radio | `PMU.RC` bracchettata set/clear, `GPIO.CTL`/`OUT`/`OE`, MHF e MACCTL intercalate. Parzialmente fatto (`frontend_gpio_setup`); resta la coda dopo `@40` |
| `BCMA_CC_PMU_CTL` | patch 0007, non `phy_ac` | `val=0x01770381` con un argomento — da decodificare contro i bit del registro |
| Seconda emissione dell'unità analog-on | apertura attach | il save è dell'entrata, non dell'unità: serve separare save e corpo |

### Derivazione — serve la formula, non la trascrizione

| blocco | stato |
|---|---|
| `ppr[24]` power reduction per-rate | hardcoded dalla cattura ch36; va derivato da `mcsbw*po` SROM |
| Base index idle-TSSI | seed catturato, il readback viene scartato |
| `recalc_txpower` / `adjust_txpower` | stub vuoti: nessuna calibrazione periodica |
| Generalizzazione canale | 50 voci in channeltab, solo ch36 validato op-per-op — piano in `channel-generalization.md` |

Per i blocchi di calibrazione la logica del loop — quante iterazioni, cosa
scrivere in funzione delle letture — non si ricava dalla sequenza catturata:
una trace mostra un caso, non la regola. Le sequenze oggi combaciano su ch36
perché sono trascritte; su un altro canale o board divergeranno.

### Proprietà altrui — non va scritto qui

Op che il driver esegue da codice fuori da `phy_ac`. Prima di riprodurle,
verificare **chi le esegue e se nell'ordine giusto**; se l'ordine è corretto la
soluzione è uno skip mirato nel confronto, non duplicare codice.

| op | proprietario | stato |
|---|---|---|
| `BCMA_CLKCTLST` su core PCIe Gen2 | `b43_bcma_wireless_core_reset` → `bcma_core_set_clockmode` | ordine verificato, skip in `compare.py` |
| Ombre `SI.COREREG` di `PMU.*`/`GPIO.*` | coppie addr/data di regcontrol e pllcontrol, registri GPIO ChipCommon | ridondanza del tracer, skip |
| `BCMA_CC_PMU_CTL` | bcma PMU | nessuno la esegue: manca, va nella 0007 |

## Fuori scope MVP

BW40/BW80 (`-EOPNOTSUPP`), 2.4 GHz (`-EOPNOTSUPP` in `op_switch_channel`),
HT/VHT, MIMO. Lo split static/volatile di `tables_init` non è un fix: finché
non c'è cambio banda nessuna tabella ha input che possano variare fra due
bring-up.

## DSL-3580L

Quattro divergenze note, tutte candidate allo stesso pattern — funzionalità
assente nella versione più vecchia del driver stock, non differenza di board.
Una è già accertata così (vcofreq frazionario, vedi `retrace-todo.md`); per le
altre il primo controllo è il confronto dei simboli fra i due blob, non una
cattura nuova.

Manca una cattura di attach del DSL. È la cosa singola che sbloccherebbe di
più: validerebbe l'ordine del load tabelle sul suo blob e direbbe cosa fa in
una fase che oggi non vediamo affatto.
