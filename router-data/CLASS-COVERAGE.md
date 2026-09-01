# Copertura per classe di op nelle catture

Il tracer `wl-diag` ha guadagnato hook nel tempo, quindi le catture **non hanno
tutte le stesse classi di op**. Una cattura presa prima che un hook esistesse
non contiene quelle op, e questo non dice niente sul driver.

Regola: **un'assenza vale come prova solo se la classe e' tracciata in quella
cattura.** Dedurre da un'assenza in una cattura incompleta ha prodotto tre
conclusioni sbagliate — la collocazione del chanspec `OBJ.WR 0x00a0`, la natura
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
attach a freddo isolato: 18 canali a 20 MHz, 7 a 40, 3 a 80. Si divide con

    python3 reverse-tools/split_by_mark.py <traccia> /tmp/cold --prefix cold

Verificata a freddo, non solo per etichetta: 6 letture OTP, 1 `SROMCTL`, 8 op
PMU, 1 `CAL.INIT` e un select della tabella `0x01` in **ognuno** dei 28
segmenti, e un solo chanspec programmato per segmento.

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
| indirizzo >= `0x1000` | 480 | `sel != 0`: il routing e' nell'offset alto |
| `0x0160`-`0x017f` | 16 | `sel = 0`, ma l'agcombo non le scrive |
| altre | 6 | da guardare una per una |

Le 385 condivise sono quelle che contano per il port: chanspec `0x00a0`, il
campione di rumore `0x0308`, la finestra delle statistiche, la configurazione
MAC.

Le 16 sono l'eccezione da non attribuire al `sel`: sono `PRSSID`, l'SSID della
probe response, e l'agcombo non le scrive perche' quel router non risponde alle
probe request -- non perche' il routing sia diverso. Vedi
`docs/retrace-todo.md`, dove b43 disattiva l'offload con `PRMAXTIME = 1`.

Per le 480 sopra `0x1000` sappiamo che il `sel` non e' zero ma non quale.
Servirebbe una ricattura del d6220 col decode corretto, oppure un agcombo che
catturi anche i `sel` non nulli.

## Rimosse

Diciassette catture non tracciavano `OBJ`, `TPL` e `CAL`. Sono state
cancellate: restano in git, e non vanno reintrodotte come riferimento. Fra
queste le due su cui girava la coppia di gate precedente,
`attach-to-bss-up-ch36-bw20` e `down-to-bss-ch36-bw20`.

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
