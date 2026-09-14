# Copertura per classe di op nelle catture

Il tracer `wl-diag` ha guadagnato hook nel tempo, quindi le catture **non hanno
tutte le stesse classi di op**. Una cattura presa prima che un hook esistesse
non contiene quelle op, e questo non dice niente sul driver.

Regola: **un'assenza vale come prova solo se la classe e' tracciata in quella
cattura.** Dedurre da un'assenza in una cattura incompleta porta a conclusioni
sbagliate: sono in questa condizione la collocazione del chanspec
`OBJ.WR 0x00a0`, la natura
delle scritture probe-response `0x0180-0x0186`, e il rifiuto del campione di
rumore `OBJ.RD 0x0308` davanti al blocco E.

`reverse-tools/check_class_coverage.py` rigenera questa tabella e la verifica.

## Complete

| cattura | op |
| --- | --- |
| `d6220/cold-sweep.zip` | 517776 |
| `d6220/wl-diag-wl1-attach-ch36-bw20-con-preambolo.txt` | 20162 |
| `d6220/wl-diag-wl1-attach-ch36-bw20-tabelle-complete.txt` | 28284 |

`cold-sweep.zip` e' la sorgente dei gate. Un modulo ricaricato per canale --
`mod GOING` poi `mod COMING` in ogni segmento -- quindi ogni segmento e' un
attach a freddo isolato: 18 canali a 20 MHz, 7 a 40, 3 a 80. I segmenti sono
**gia' divisi** nello zip, sotto `segmenti/`, e la traccia intera resta in
`00-traccia-intera.txt` per riverificare la divisione:

    unzip -d /tmp/cold router-data/d6220/cold-sweep.zip

`split_trace.py --on mark` serve solo per rifare la divisione da capo su una cattura
nuova. Sulla traccia in archivio e' deterministico: rilanciarlo su
`00-traccia-intera.txt` riproduce `segmenti/` bit per bit.

Contabilita' della divisione: 652870 record in ingresso, 652870 nella somma dei
28 segmenti, zero persi. `cold01` e `cold02` non sono cicli ma l'armamento del
tracer prima del primo insmod, e non vanno usati come segmenti; i segmenti veri
sono 26.

Verificata a freddo, non solo per etichetta: 6 letture OTP, 1 `SROMCTL`, 8 op
PMU, 1 `CAL.INIT` e un select della tabella `0x01` in **ognuno** dei 28
segmenti, e un solo chanspec programmato per segmento.

## Le classi che il DSL ha e il d6220 no

Sette classi compaiono nelle catture DSL-3580L e in nessuna del d6220:
`AMT.WR`, `RCMTA.WR`, `OBJ.BULKW`, `OBJ.SET`, `SROMCTL.WR`, `PHY.WARR`,
`CS.SHM`. La spiegazione che questo file dava prima -- le catture sono piu'
vecchie degli hook -- **non regge**, e il conteggio lo dice:

| cattura | driver | `AMT.WR` | `OBJ.BULKW` | `OBJ.SET` | `SROMCTL.WR` | `CS.SHM` |
| --- | --- | --- | --- | --- | --- | --- |
| d6220 cold sweep | 7.14.89.14 | 0 | 0 | 0 | 0 | 0 |
| d6220 hot sweep | 7.14.89.14 | 0 | 0 | 0 | 0 | 0 |
| agcombo cold sweep | 7.14.43.21 | 0 | 0 | 0 | 0 | 0 |
| DSL-3580L | 6.30.102.7 | 122 | 7 | 1 | 4 | 1 |

Tre sweep, **due board diverse**, tutte a 7.14, tutte a zero; il 6.30 le ha
tutte. E' uno spartiacque di versione, non una data. Le stesse dieci classi
mancano in blocco, comprese `ADDRM.SET` e `IHR.WR` che mancano anche sul DSL.

## Quello che si verifica da fermi, e quello che no

Il modulo `wl` linkato si estrae dal firmware (`.chk` -> header Netgear di 60
byte -> JFFS2 big-endian -> `wl.ko`), ed e' **lo stesso codice dell'oggetto
pre-link**: `.text` e `.rodata` byte-identiche, rilocazioni identiche per
offset e tipo, cambiano solo gli indici di simbolo. Il piano hook sui due esce
riga per riga identico, quindi il prelink basta e non e' un'approssimazione.

`reverse-tools/audit_hooks.py` rifa' `pianifica()` sull'oggetto e conta anche i
**siti di chiamata**, che e' la colonna che conta: un hook si pianifica
benissimo su una funzione che nessuno chiama. GCC emette la copia out-of-line
di una funzione GLOBAL anche quando la inlinea in tutti i chiamanti, quindi
simbolo risolto, prologo agganciato, classe a zero per sempre. Sul blob 7.14
capita a due simboli:

| simbolo | siti | conseguenza |
| --- | --- | --- |
| `phy_reg_write_wide` | 0 | `PHY.WRW` non comparira' mai su questa build |
| `wlc_write_amtinfo_by_idx` | 0 | non e' agganciato, e il suo corpo e' il dispatch AMT |

**Per l'AMT l'inlining pero' non spiega niente**, e va detto perche' e' la
prima ipotesi che viene in mente: `wlc_bmac_write_amt` ha 12 chiamanti reali,
fra cui `wlc_bmac_init`. E la scrittura passa comunque da
`wlc_bmac_copyto_objmem`, 568 byte con 13 chiamanti, certamente sul percorso di
attach: se il suo hook fosse stato armato, `OBJ.BULKW` non sarebbe zero.

**La misura che manca non e' statica.** `pianifica()` stampa una riga per hook
all'insmod: rimettere il tracer corrente sul d6220 e leggere il dmesg dice se
l'hook e' stato pianificato, cosa che nessuna analisi sull'oggetto puo' dire
perche' dipende da quale binario girava quel giorno. Se risulta pianificato e
muto, il candidato da guardare e' `wlc_write_amtinfo_by_idx` inlineata, e
l'unica via e' agganciare la funzione che la contiene -- `wlc_set_addrmatch`,
gia' in tabella e che si pianifica.

## Due assenze che restano legittime

**`RCMTA.WR` non puo' comparire per il nostro core.** L'unico chiamante e'
`wlc_set_addrmatch`, che smista su una soglia: `lw 0(a0)` / `lw 16(v0)` /
`sltiu v0, 0x28` / `bne` -- sotto 40 va a `wlc_bmac_set_rcmta`, da 40 in su a
`wlc_bmac_write_amt`. E' la soglia di `brcms_b_set_addrmatch()` in brcmsmac,
che su `D11REV_GE(rev, 40)` usa l'AMT. Il core AC e' **corerev 42**. Le 54
`RCMTA.WR` della cattura DSL sono dell'altro core.

**`PHY.WARR` non ha chiamanti AC-PHY.** Dei 271 siti nel blob 7.14 nessuno e'
una funzione `*_acphy`: sono LCN, LCN40, LP, G, A, N piu' una decina di
dispatcher generici che si diramano per tipo di PHY. Nella cattura DSL tutte e
quattro le occorrenze sono `cpu0` e cadono nella finestra dell'attach di wl0.
Non e' una prova -- i dispatcher generici non sono stati seguiti dentro -- ma
le due evidenze puntano dalla stessa parte.

## `PHY.FGC`, la classe che non c'era

Il force gated clock non e' inlineato: `wlc_bmac_phyclk_fgc` e' vivo, 96 byte.
Non era **agganciato**, quindi non esisteva una classe per esso in nessuna
cattura, di nessuna versione. E dall'altra parte l'harness emette
`b43_phy_force_clock()` come commento, che `compare.py` ignora: invisibile su
tutti e due i lati del confronto.

Conta perche' i chiamanti AC sono tre e non uno -- `wlc_phy_resetcca_acphy`
una volta, `wlc_phy_cal_txiqlo_acphy` due -- mentre il port lo chiama solo da
`b43_phy_ac_reset_cca()`. Se il vendor forza il clock anche attorno alla cal
TX IQ/LO, oggi la divergenza non la vede nessuno.

L'hook e' su `wlc_bmac_phyclk_fgc` e non sul thunk `wlapi_bmac_phyclk_fgc`:
tutti e tredici i chiamanti passano dal thunk, che gli fa tail-call, e il thunk
ha `lui $t9` alla parola 0 -- il caso che ha imposto la scelta del registro di
rientro -- piu' tredici siti contro `MAX_SITES` 8. Short-j, rientro su `$t9`.

## Cosa cambia per il confronto

`AMT.WR` sta oggi in `SOLO_PORT` di `test/unit/compare.py`, con la nota che e'
"un'op giusta senza oracolo" e che la voce si chiude con una ricattura. Resta
li' finche' non si sa se l'hook si arma. `OBJ.BULKW` porta il selettore della
finestra di object memory, che e' il punto aperto sul routing qui sotto.
`CS.SHM` e' il confine per segmentare uno sweep che
`reverse-tools/split_trace.py` oggi deve ricavare dai salti temporali.
`PHY.FGC` e' l'unica classe nuova che copre un buco su **entrambi** i lati.

Il tracer per il 2.6.30 (`reverse-tools/wl-diag-2630/`) non e' stato toccato:
le correzioni qui sopra sono verificate sul blob 7.14, e per il 6.30 non c'e'
un oggetto su cui rifare il piano. Applicarle alla cieca la' significherebbe
mettere uno `shortj` su un prologo che nessuno ha guardato.

## Il campo `sel` e la ricostruzione per intersezione

Le op `OBJ` portano un `sel`, il routing della finestra di shared memory. Un
difetto del decode l'ha perso nello sweep a freddo del d6220: le sue righe `OBJ`
non hanno il campo, mentre quelle dell'agcombo lo hanno e valgono tutte
`sel=0x0000`.

Il d6220 puo' essere ricostruito per intersezione, perche' l'agcombo non ha
**nessuna** chiave `OBJ` che il d6220 non abbia -- il suo insieme e' un
sottoinsieme pulito:

| gruppo | chiavi | lettura |
| --- | --- | --- |
| condivise con l'agcombo | 385 | `sel = 0` anche sul d6220 |
| indirizzo >= `0x1000` | 480 | ignoto, vedi sotto |
| `0x0160`-`0x017f` | 16 | `sel = 0`, ma l'agcombo non le scrive |
| altre | 6 | da guardare una per una |

Il campo `sel` compare nel decoder da `b242481`; lo sweep a freddo del d6220 e'
stato decodificato prima, con `a1ac852`, e l'agcombo dopo. Ridecodificare la
traccia d6220 restituisce il campo.

Per le 480 sopra `0x1000` il selettore resta ignoto: la shared memory di questi
chip arriva oltre `0x1000` -- lo stesso port azzera `0x10f4`-`0x14b2` -- quindi
un offset alto non dice niente sul routing.

Le 385 condivise sono quelle che contano per il port: chanspec `0x00a0`, il
campione di rumore `0x0308`, la finestra delle statistiche, la configurazione
MAC.

Le 16 sono l'eccezione da non attribuire al `sel`: sono `PRSSID`, l'SSID della
probe response, e l'agcombo non le scrive perche' quel router non risponde alle
probe request -- non perche' il routing sia diverso. Vedi
`docs/retrace-todo.md`, dove b43 disattiva l'offload con `PRMAXTIME = 1`.

Le 480 sopra `0x1000` sono shared memory come le altre, vedi sopra. Una
ricattura del d6220 col decode corretto restituirebbe il campo, ma direbbe
zero: quello che serve e' cambiare l'hook, non il decode.

## Rimosse

Diciassette catture non tracciavano `OBJ`, `TPL` e `CAL`. Sono state
cancellate: restano in git, e non vanno reintrodotte come riferimento.

## Senza `CAL`

| cattura | op |
| --- | --- |
Erano le sette catture DSL-3580L, rimosse con le altre: `CAL.INIT` compare una
volta per ciclo, quindi la loro assenza era ambigua fra hook mancante e caso che
non si presenta, e su quella base non si conclude niente. Il DSL resta
rappresentato da `wl1_nvram.txt`, `wl1_srom_raw.txt` e `wl1_otp_dump.txt`, che
non sono tracce.

## Sweep

I 52 segmenti di `d6220/full-sweep.zip` sono completi: 1084 `OBJ.RD`, 900
`OBJ.WR`, 16 `TPL.RAMW`, 1 `CAL.INIT` per segmento.
