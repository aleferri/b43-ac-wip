# wl-diag — tracer inline-detour per il driver `wl`

Modulo kernel che aggancia gli accessor PHY/radio/PMU/MAC del driver Broadcom
`wl` senza kprobe (detour all'ingresso funzione), ed espone i record su un
misc-device. Vedi la testata di `wl_diag.c` per i dettagli del meccanismo e i
limiti (MIPS32R1, memoria modulo RWX, read con valore UNDEFINED).

Cosa cattura ora: PHY rd/wr/mod, **PHY and/or distinti** (op 19/20), radio
rd/wr/mod, PMU cc/rc/pll, GPIO ChipCommon, tabelle acphy, e il **controllo verso
il MAC** (`wlc_bmac_mctrl`, `wlc_bmac_mhf`) e la **object memory del MAC**
(`wlc_bmac_read_objmem16`/`_write_objmem16`, op `OBJ.RD`/`OBJ.WR`): il uCode
vi deposita fra l'altro il campione di potenza di rumore che la
`crs_min_pwr` cal legge, e quella lettura non passa da un registro PHY.

**Due hook non sono applicabili col detour a 4 parole**, ed e' bene saperlo
prima di flashare invece di scoprirlo dal log:

| funzione | perche' |
|---|---|
| `wlc_bmac_mhf_get` | `beq` alla parola 1: nemmeno lo short-j sta in piedi. Risolta patchando i **siti di chiamata**, vedi sotto. |
| `wlc_bmac_read_shm` / `_write_shm` | wrapper di 16 e 20 byte con `jr` alla parola 2. Per questo si aggancia il bersaglio della tail call, `read/write_objmem16`, che ha prologo pulito. |

### Prologhi non agganciabili, e le due vie

Alcune funzioni hanno un branch nella finestra del detour, quindi ne' il detour
a 4 parole ne' lo short-j: `wlc_bmac_mhf_get` ha un `beq` alla parola 1, ed e'
per questo che `MAC.MHF.RD` e' 0 su 215 `MAC.MHF` in tutte le catture.

**Patch dei siti di chiamata** (preferita). Il modulo e' `-mabicalls`: zero
`jal` in `.text`, le chiamate sono `lui`/`addiu` + `jalr`, o `jr $t9` per le
tail call. Si riscrive la coppia perche' carichi lo stub; la funzione resta
intatta e funziona su entrambi i kernel. Tre condizioni verificate a runtime:
indirizzo esatto, salto sullo stesso registro, `addiu` non condiviso.
`reverse-tools/callsites_pic.py` fa la stessa analisi offline su un blob.

| funzione | 6.30 | 7.14 |
|---|---|---|
| `wlc_bmac_mhf_get` | 2 siti, 2 ok | 3 siti, 3 ok |
| object memory | `read_objmem`, 4 siti, 2 ok | `read_objmem16`, 4 siti, 2 ok |

**Percorso a `break`** (solo 3.4). Una parola, die notifier su `DIE_BREAK`:
`do_bp` lo chiama per `BRK_KPROBE_BP` fuori da `CONFIG_KPROBES`. Su 2.6.30 non
c'e' e si compila via -- `do_bp` va diretto a `do_trap_or_bp` e
`set_except_vector` non e' esportata.

**I nomi cambiano fra versioni** (`read_objmem` / `read_objmem16`, offset di
struct `0x84` / `0x88`) e agganciare il nome sbagliato non da' errore: l'hook
viene saltato e la cattura esce senza quella classe di op.

### Run unica su piu' canali

`reverse-tools/capture_plan.sh` fa lo sweep, e
`reverse-tools/split_by_chanspec.py` taglia la traccia decodificata in un file
per chanspec:

Le fasi sono **separate**, perche' una run intera produce troppi byte per
raccoglierli in un colpo. Una fase per volta, ognuna col suo file:

```sh
sh capture_plan.sh 20a          # sul device, con arm=1
sh capture_plan.sh 20b
sh capture_plan.sh 40
sh capture_plan.sh 80
python3 reverse-tools/split_by_chanspec.py trace-20a.txt split/
```

A 40 e 80 MHz il **comando** vuole il canale basso del blocco (`5g36/40`, non
`5g38/40`), mentre il **chanspec** che ne risulta porta il canale **centrale**
(`ch=38 bw=40`). Sono due livelli diversi, entrambi veri.

Due cose viste alle prime catture, che rendono i record `CHANSPEC` inservibili
come confine:

- sono **in ritardo di un ciclo**: nella fase 20b compaiono `ch=40`, `ch=36`,
  `ch=60`, che sono della 20a. `wlc_phy_chanspec_set` viene invocata col
  chanspec corrente, non col nuovo. Per marcare i confini servirebbe agganciare
  anche `wlc_phy_chanspec_set_acphy`.
- ne arriva **uno solo per fase**, o zero.

Percio' `split_by_chanspec.py` taglia sui **salti temporali** e prende i nomi
dall'ordine della fase, riportando i `CHANSPEC` trovati per verifica. Soglia di
default 1.03 s: lo `sleep 1` fra i cicli lascia 1.06 s, mentre il campionamento
periodico del rumore lascia buchi di 1.00 s esatti fra due `OBJ.RD`, e per
durata sarebbero indistinguibili.

**Il listener non va fermato fra i canali.** La fifo tiene `FIFO_RECS` = 32768
record; a circa 2500 record/s sono ~13 secondi di margine. Fermandolo, la coda
si riempie e il modulo emette un record `OP_DROP` col conteggio: utile per
sapere quanto e' andato perso, ma perso resta.

Il ciclo e' `{chanspec; up; attesa; down}`, quindi ogni canale ottiene un
**down->up**, non un attach. Su 3.4 la traccia puo' contenere 1 attach iniziale
(dal re-probe) + N down->up; su 2.6.30 solo N down->up, perche' il rescan
scarica `wl` e l'attach non e' catturabile in quel modo.

I segmenti sono tutti nella stessa fase e quindi confrontabili fra loro, ma
vanno confrontati col gate `switch_channel` e `AC_FIRST_INIT=0`, non con `full`
che modella l'attach. La distinzione non e' cosmetica: cal `crs_min_pwr`, primo
blocco del banco `0x0910` e doppia programmazione dell'analogico si comportano
diversamente nelle due fasi.

L'attesa dopo `up` lascia completare le calibrazioni asincrone -- crsmin, PAPD,
fdiqi -- che sono quelle che leggono dalla shared memory e dal template RAM.


`wlc_phy_chanspec_set` e' agganciata e emette `CHANSPEC` a ogni cambio, con il
chanspec grezzo in `addr`. Il decoder lo espande:

    CHANSPEC ch=36 bw=20 band=5g raw=0xd024

Cosi' una sola cattura puo' coprire 36/20, 36/40, 36/80 ... 140/20, 140/40,
140/80 e si taglia a posteriori sui record `CHANSPEC`. Si aggancia la generica
e non la variante acphy perche' scatta per ogni PHY ed e' piccola (96B su 6.30,
324B su 7.14, prologo pulito su entrambe).

Il decode usa il formato 802.11ac standard (`chan` bit 0-7, `bw` 0x3800, `band`
0xc000). **Assunzione, non verificata su cattura** -- nessuna delle catture
attuali contiene record `CHANSPEC`. Il campo `raw` c'e' apposta: se il decode
non torna, il valore grezzo resta leggibile.

### Copertura dell'I/O

Per spazio di memoria, non per nome di funzione:

| spazio | stato |
|---|---|
| registri PHY, radio, tabelle PHY | coperto |
| object memory / SHM | coperto (`OBJ.*`) |
| MAC mctrl + MHF, PMU, GPIO, core reg | coperto |
| **template RAM** | coperto da questo giro (`TPL.*`) |
| **OTP** | coperto (`OTP.*`) |
| I/O inline via `R_REG`/`W_REG` | **non agganciabile**: sono macro, non funzioni |

Il template RAM e' dove il PHY carica le forme d'onda dei toni, ingresso di
RXIQ, PAPD e `do_dummy_tx` (`wlc_phy_loadsampletable_acphy`,
`wlc_phy_sample_data_acphy` con 17 siti su `templateptr_wreg`). Non era coperto
da nessuna classe di op, quindi quei dati mancavano del tutto dalle catture --
lo stesso tipo di buco dello SHM.

Su 6.30 gli accessor `templateptr`/`templatedata` non esistono: la' si aggancia
solo `write_template_ram`.

L'OTP si aggancia al **livello generico** (`otp_init`, `otp_read_word`,
`otp_read_region`), che ha gli stessi nomi su 6.30 e 7.14 e prologo pulito,
mentre `hndotp_*`/`ipxotp_*` cambiano fra versioni. Il contenuto e' l'immagine
SROM -- statica e gia' nota dai dump raw -- quindi serve per sapere **quando**
viene letta e **quali word**, cioe' dove i valori vengono consumati. Il valore
finisce in un puntatore, non nel ritorno, quindi nel record c'e' il numero di
word e non il dato.

### Conservare la fifo sui canali DFS

Sui canali DFS il rivelatore radar interroga `PHY.RD 0x0253` e `0x0254` in
continuo: **192000 e 194000 letture** nelle quattro fasi, l'85% di tutte le
letture PHY, e col `retcap` attivo il doppio. Non c'entrano con la
configurazione del canale, quindi per lo sweep di massa si filtrano prima della
fifo:

```sh
insmod wl_diag.ko arm=1 skipphyrd="0x253,0x254"
```

Due restrizioni deliberate, entrambe da verifiche sui dati.

Il filtro vale **solo per `OP_PHY_R`**: gli spazi di indirizzamento sono
separati
per classe, e nelle stesse catture ci sono 32 `OBJ.WR` a `0x252` e 32 a `0x254`
che sono offset di object memory, non registri PHY. Un filtro sul solo indirizzo
li avrebbe buttati in silenzio.

E si filtrano **solo `0x253`/`0x254`**. La testa del blocco -- `0x251` e
`0x252`,
lette 1558 volte in tutto, una per blocco -- e' plausibilmente lo stato e i dati
dell'impulso, cioe' la parte utile: costa poco e si tiene.

I record filtrati hanno un contatore separato dai persi, cosi' gli `OP_DROP`
restano un indicatore di perdita vera. A fine corsa:

    wl_diag: scaricato (persi: 0, filtrati: 148392)

**Le catture DFS vanno fatte senza filtro**, due o tre, perche' quelle letture
sono l'unico materiale sul rivelatore. Non serve di piu': il classificatore
ETSI/FCC Linux lo ha gia' in `drivers/net/wireless/ath/dfs_pattern_detector.c`
(377 righe), che consuma `struct pulse_event {ts, freq, width, rssi, chirp}` --
e i campi che il driver Broadcom stampa (`min_pw`, `pri`, `fm_min`/`fm_max`,
`nconsecq_pulses`) mappano su quelli. Quindi dei 24 KB di
`wlc_phy_radar_detect_run` serve solo il **formato dei quattro registri**, non
la
classificazione.

Attenzione alle unita': `pri=44258` e' troppo grande per essere microsecondi,
dato che i PRI della normativa stanno fra 200 e 3000 us. Da stabilire dalla
cattura, non per ipotesi.

### Se non arriva nessun RETVAL

Sintomo: tutte le letture hanno `val=UNDEFINED` e nella traccia non c'e' un solo
record `RETVAL`, quindi si ha la sequenza delle letture ma non i valori.

Prima verifica, una riga:

```sh
dmesg | grep 'trampolino ritorno'
```

Manca -> nessun hook eleggibile risultava `retcap`, e il trampolino non e' stato
costruito. C'e' -> e' costruito ma non viene raggiunto, e il sospetto e' il pool
di `wl_diag_enter_ret`, che restituisce `orig_ra` in silenzio quando le entry
sono esaurite.

**E' successo per davvero**, causa: tre campi di stato (`use_bp`, `use_sites`,
`bp_stub`) inseriti nella `struct hook` **fra `shortj` e `retcap`**. La tabella
usa inizializzatori posizionali, quindi il `true` destinato a `retcap` finiva in
`use_bp` e `retcap` restava falso per ogni hook. I campi nuovi ora stanno in
coda, e gli inizializzatori usano la forma designata (`.retcap = true`) che e'
immune al riordino.

### Il rumore passa dall'object memory

    wlc_phy_noise_read_shmem -> wlapi_bmac_read_shm -> wlc_bmac_read_shm
                             -> wlc_bmac_read_objmem[16]

Quindi `OBJ.RD` cattura anche il campione di rumore della `crs_min_pwr` cal.
Nel 7.14 `noise_read_shmem` chiama `wlc_phy_crs_min_pwr_cal_acphy`, nel 6.30
no: e' il motivo per cui il DSL scrive sempre zero nel banco `0x0910`.

`MAC.MHF.RD` invece e' ridondante: `mhf_get` e' un getter puro e tutte le
scritture sono gia' catturate (215 record, indici 0..4), quindi lo stato si
ricostruisce offline.


## Parametri

| param | default | effetto |
|-------|---------|---------|
| `skipphyrd` | vuoto | letture di **registro PHY** da non registrare, es. `"0x253,0x254"` |
| `arm`   | `0` | `0` = dry-run (logga solo il piano hook); `1` = applica le patch |
| `delay` | `0` | `1` = aggancia anche `osl_delay` (rumoroso, usec inaffidabile) |

## Build

Fuori albero, contro il kernel 3.4 del device (stesso `.config`, stessi
`Module.symvers`, altrimenti vermagic/CRC non combaciano e `insmod` rifiuta):

```sh
make KDIR=/path/al/kernel-3.4-rt ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j
```

Copia `wl_diag.ko` sul device e `decode-wl-diag.py` sull'host di raccolta.

## Workflow di cattura (target 5 GHz, wl1 = `0x14e4:0x43b3`)

Il modulo `wl` resta **caricato** per tutta la procedura: `kallsyms` deve vedere
i suoi simboli al momento dell'`insmod`. Si stacca il *device* (funzione PCI),
non il modulo. La regola d'oro: **armare mentre il device e' giu'**, cosi' il
re-probe successivo esegue l'attach attraverso gli hook.

**Su 2.6.30 il rescan scarica `wl`.** Non e' il kernel: rimuovere una funzione
PCI chiama `remove()` del driver ma non scarica il modulo -- ed e' per questo
che
su 3.4 `wl` resta caricato quando si rimuove il device. Su 2.6.30 e' lo spazio
utente del vendor (hotplug o script `rc`) a fare `rmmod wl`.

Due difese, in ordine:

**Riferimento sul bersaglio.** All'arming `wl_diag` prende un riferimento su
`wl`, quindi `rmmod wl` fallisce con `-EBUSY` e il modulo non se ne va;
remove/probe del device continuano a funzionare. Lo script del vendor loggera'
un errore sul suo `rmmod`: e' atteso. Per scaricare `wl` va prima scaricato
`wl_diag`.

**Notifier di riserva**, se il riferimento non si e' potuto prendere: su
`MODULE_STATE_GOING` del bersaglio gli hook vengono abbandonati **senza
ripristino** -- la memoria sta per essere liberata e riscriverla sarebbe peggio.

Dopo un re-`insmod` di `wl` va comunque ricaricato `wl_diag`: gli indirizzi
cambiano.

### Checkout su Windows: CRLF

`.gitattributes` forza `eol=lf`. Se il clone e' anteriore, gli script arrivano
sul router con CRLF e non partono: `set -u` seguito da CR e' un'opzione
illegale, e il CR riporta il cursore a colonna 0 mangiando l'inizio dei
messaggi. Si riconosce da errori come `: not foundn.sh: line 27:` invece di
`capture_plan.sh: line 27: X: not found`, o da un
`syntax error: unexpected end of file (expecting "done")` su uno script
corretto. Rimedio:

```sh
tr -d '\r' < capture_plan.sh > /tmp/cp.sh && sh /tmp/cp.sh 20a
```

### 0. Host: listener pronto PRIMA

TCP, non UDP: il `nc` di busybox sul router non ha `-u`, e con TCP non si
perdono record ne' si spezzano a meta'.

Linux/macOS -- `ncat` (pacchetto `nmap-ncat`); il `nc` di netcat-openbsd va
bene anche lui in TCP:

```sh
ncat -l 5555 | python3 decode-wl-diag.py | tee trace.txt
```

Windows: `ncat` esiste anche per Windows ed e' la via piu' semplice. Senza di
quello, un `TcpListener` in PowerShell che scrive un `.bin` grezzo -- la pipe di
PowerShell passa **oggetti**, non byte, quindi non ci si puo' infilare python
direttamente:

```powershell
$l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, 5555)
$l.Start(); $c = $l.AcceptTcpClient(); $ns = $c.GetStream()
$fs = [System.IO.File]::Create("$PWD\trace.bin")
$buf = New-Object byte[] 4096
try {
    while (($n = $ns.Read($buf, 0, $buf.Length)) -gt 0) {
        $fs.Write($buf, 0, $n); $fs.Flush()
    }
} finally { $fs.Close(); $c.Close(); $l.Stop() }
```

poi:

```powershell
python decode-wl-diag.py < trace.bin > trace.txt
```

### 1. Dry-run: verifica il piano hook

```sh
insmod wl_diag.ko                 # arm=0
dmesg | grep wl_diag              # "piano hook '<nome>' @..." per ognuno
rmmod wl_diag
```

Controlla che i simboli attesi risultino agganciabili (nessun "non trovato" /
"branch non rilocabile" / "stub fuori regione j 256MB" sui simboli che ti
servono — in particolare i nuovi `phy_reg_and/or`, `wlc_bmac_*`).

### 2. Porta giu' il device (PCI down)

Trova la funzione PCI di wl1 e rimuovila (su BCM63xx il bus e' 1 — adatta il
path al tuo SoC):

```sh
grep -il 14e4 /sys/bus/pci/devices/*/vendor        # individua il nodo
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove  # detach: wl.remove() gira
```

### 3. Arma il tracer (device ancora giu')

```sh
insmod wl_diag.ko arm=1           # + delay=1 se serve la temporizzazione
dmesg | tail                      # "wl_diag: ARMATO (N hook) -> /dev/wl_diag"
```

### 4. mknod in /tmp (niente udev su questo userspace)

Il misc-device ha major 10 e minor dinamico; il rootfs e' spesso read-only, per
questo il nodo va in `/tmp` (tmpfs):

```sh
minor=$(awk '$2=="wl_diag"{print $1}' /proc/misc) # o: cat
/sys/class/misc/wl_diag/dev
mknod /tmp/wl_diag c 10 "$minor"
```

### 5. Avvia la pipe verso l'host (PRIMA del rescan, per non perdere record)

```sh
cat /tmp/wl_diag | nc <HOST> 5555 &
```

### 6. Rescan PCI: il re-probe esegue l'attach sotto gli hook

```sh
echo 1 > /sys/bus/pci/rescan      # wl ri-probe 0x43b3 -> wlc_attach/init/up
```

Per la variante "down-to-bss-up" invece del rescan, con il device gia' su.
Attenzione: `wl up` da solo fa **solo attach** (equivalente al primo scan), NON
porta su la bss. Servono in sequenza il set-ssid e poi `bss up`:

```sh
wl -i wl1 down
wl -i wl1 up          # solo attach (come il primo scan): niente bss ancora
wl -i wl1 ssid <SSID> # configura l'SSID (senza portarla su)
wl -i wl1 bss up      # QUESTO porta su la bss (beacon/join)
```

### 7. Chiudi e decodifica

```sh
rmmod wl_diag                     # ripristina i prologhi + synchronize_sched
```

Sull'host, `Ctrl-C` su `nc`: `trace.txt` contiene la trace decodificata. Se
compaiono righe `** DROP ** persi=N`, la FIFO kernel (32768 record) e' andata in
overrun: reader troppo lento o burst troppo denso.

## Note

- **Sacrificale.** `arm=1` scrive nella memoria del modulo (RWX) e fa
  `flush_icache_range`; assunzione MIPS32R1 da confermare sul device.
- **Read = UNDEFINED.** Gli hook catturano solo gli argomenti d'ingresso: il
  valore restituito dalle read non c'e' (mai `0x0000` inventato). Per l'and/or
  PHY questo non e' un problema: l'operando e' `a2`, catturato, e il decoder
  rende la maschera effettiva (`clr ~val` / `set val`).
- **Ordine.** Listener su, poi arma a device giu', poi pipe, poi rescan. Armare
  a device gia' su perde l'inizio dell'attach.
- **Consumatore a valle.** `decode-wl-diag.py` (stream 28 B).
