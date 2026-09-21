# router-data/tg789vac-v2 — BCM4360 su driver 7.14.89.14

Quarto board della collezione. Technicolor TG789vac v2, gateway VDSL2 su SoC
BCM63168 (BMIPS4350, due thread hardware): `wl0` e' il core N-PHY sul bus
interno (`BCM435f`, `sb/0/`), `wl1` e' il BCM4360 su PCI (`pci/2/0/`) ed e'
quello che interessa. Il file di mappa che il driver carica per `wl0` si chiama
`bcm6362_map.bin`: e' il nome del file, non il SoC del router.

```
deviceid   0x43a2      chipnum 0x4360   chiprev 0x3    chippackage 0x1
corerev    0x2a        phytype 0xb      phyrev  0x1    radiorev 0x42069
boardid    0x6d8       boardrev P120    sromrev 11     3x3 dual band
driverrev  0x70e590e = 7.14.89.14 (cpe4.16L03.0-kdb)
ucoderev   0x3a005de ~ 3.160.5.222
```

## Piattaforma software

Non e' un SDK Broadcom nudo come il D6220: e' firmware Technicolor su OpenWrt,
e cambia quel che serve per buildare i moduli fuori albero.

```
DISTRIB_RELEASE    Chaos Calmer 15.05.1, DISTRIB_REVISION r46610
DISTRIB_TARGET     brcm63xx-tch/VANTF
uname -r           3.4.11-rt19, #1 SMP PREEMPT
toolchain          gcc 4.6.4 (OpenWrt/Linaro GCC 4.6-2013.05)
/proc/config.gz    assente
```

`VANTF` e' lo stesso target e profilo delle TG799vac, quindi la `.config` del
kernel da cercare non e' "quella del TG789vac v2" ma quella di `VANTF`, e i
pacchetti per `brcm63xx-tch` valgono su entrambi.

`/proc/config.gz` assente vuol dire `CONFIG_IKCONFIG_PROC` off in questa build
(nelle config ARM dello stesso ramo Technicolor e' on, quindi dipende dal
target). Due sostituti, nessuno dei quali richiede il sorgente del vendor:

- la versione del pacchetto `kernel` (`opkg info kernel`) ha la forma
  `3.4.11~<md5>-r<rel>`, dove l'md5 e' quello delle righe `=[ym]` della
  `.config` del vendor, ordinate: `grep '=[ym]' .config | LC_ALL=C sort | md5s`
  in `include/kernel-defaults.mk` dell'albero OpenWrt. Non e' invertibile ma
  valida una config candidata in modo esatto;
- il vermagic da agganciare si legge dai `.ko` del vendor sul device
  (`strings wl.ko | grep vermagic`). Senza sezione `__versions` in quei `.ko`,
  `MODVERSIONS` e' off e quella stringa e' l'intero cancello di `insmod`.

Compilare con `CONFIG_DEBUG_PREEMPT` off in ogni caso: in 3.4
(`include/linux/preempt.h`) con quell'opzione on `preempt_disable()` diventa una
chiamata a `add_preempt_count()`, e un modulo che la richiede su un kernel che
non la esporta non carica. Al contrario non succede nulla: l'aritmetica inline
agisce sullo stesso `thread_info->preempt_count`.

Le righe `DISTRIB_*` e l'assenza di `/proc/config.gz` sono lette su questa
unita'. SoC, `uname -r` e toolchain vengono da boot log seriali di TG789vac v2
e dalla corrispondenza fra `brcm63xx-tch/VANT*` e il BCM63168: da confermare su
questa unita' con `cat /proc/cpuinfo` e `uname -a`.

## Cosa chiude

La collezione aveva il 4360 solo su 7.14.43 (agcombo) e il 7.14.89 solo sul
4352 (d6220), quindi chip e versione del driver erano confusi. Con questo la
tabella si chiude:

| | 7.14.43 | 7.14.89 |
| --- | --- | --- |
| 4352 | — | d6220, dsl3580l (6.30) |
| 4360 | agcombo | **tg789vac-v2** |

`driverrev` e' **identico** a quello del d6220, cioe' lo stesso `wl` del
segmento di riferimento del gate. L'ucode invece no -- l'immagine e' per
famiglia di chip -- quindi questo board separa anche **driver da ucode**, che
e' una coppia che il port confonde: le celle di shared memory e la finestra
delle statistiche sono contratti con l'ucode, non col driver, ed e' il motivo
per cui esiste `patches/0013`.

**I triplet rxgain sono del radio.** `5gl (3,6,1)`, `5gm (7,15,1)`,
`5gh (7,15,1)`, identici su tutte tre le catene e identici a d6220 e agcombo.
Il README dell'agcombo li chiama "default radio-side" ma con un solo 4360 era
un'asserzione; qui chip, boardtype e ramo del driver sono tutti variati e il
valore non si muove. Stesso discorso, e stessa forza, per `femctrl=6`,
`subband5gver=0x4`, `boardflags=0x10000000`, `boardflags2=0x2`, `epagain5g=0`,
`tssiposslope5g=1`, `gainctrlsph=0`, `paparambwver=0`, `xtalfreq=65535`:
identici su tutti e tre.

## Cosa si muove

| campo | d6220 | agcombo | qui |
| --- | --- | --- | --- |
| `pdgain5g` | 10 | 10 | **19** |
| `maxp5ga0` | 72,70,86,0 | 74,74,82,82 | **90,88,92,88** |
| `aga0/1/2` | 133 | 133 | **68/68/67** |
| `tempthresh` | 255 | 255 | **120** |
| `phycal_tempdelta` | 255 | 255 | **0** |
| `temps_period` / `hysteresis` | 15 | 15 | **5** |
| `watchdog` | 3000 | 3000 | **70000** |

`pdgain5g` e' il primo valore diverso della collezione su quel campo. Il port
oggi non lo legge, ma il vendor si', nel percorso della potenza TX: in una
cattura di qui quel ramo esce diverso da tutto cio' su cui il port e' tarato.

Le quattro righe della temperatura sono l'altra novita': qui la ricalibrazione
termica e' configurata, dove i due board di riferimento hanno `0xff`, cioe'
segnaposto. Su una cattura lunga il percorso termico dovrebbe girare, e sui
board attuali non si osserva.

**`watchdog=70000` va misurato prima di pianificare uno sweep.** Tutto il
modello dei giri del port -- measure block ogni dieci, dump della regione ogni
trenta, giro d'ingresso, `AC_WD_PHASE` -- e' costruito sul battito da 1.004 s
misurato sulle catture del d6220. Che quel campo governi la cadenza non e'
verificato, e 3000 non corrisponde a 1.004 s in nessuna lettura ovvia, quindi
potrebbe non essere quel parametro. Si risolve con una misura e non con un
ragionamento: sulla prima cattura, anche corta, la distanza fra due
`PHY.MOD 0x0520 mask=0xc`. Se i giri non cadono a 1.004 s, ogni regola contata
in giri va riderivata e non riusata.

## Provenienza dei file

**Questi dump sono passati per un copia-incolla da terminale**, non per un
redirect. Vanno rifatti con `ssh tg789 '<comando>' > file` quando c'e'
occasione, e sono da considerare provvisori fino a quel momento.

La trascrizione e' stata verificata in modo incrociato fra i due file
trascritti separatamente: `boardrev`, `boardtype`, `devid` e `sromrev` della
NVRAM contro `srom[65]`, `srom[2]`, `srom[48]` e l'ultimo byte; i 36 word di
`pa5ga0/1/2` contro `srom[116:128]`, `srom[136:148]`, `srom[156:168]`; i sei
`maxp5ga*` contro le coppie di byte in `srom[114-115]`, `[134-135]`,
`[154-155]`. Tutto coerente, quindi un errore di battitura dovrebbe essere
fuori da quelle regioni.

| file | contenuto |
| --- | --- |
| `tg789vac-v2_revinfo.txt` | `wl -i wl1 revinfo` |
| `tg789vac-v2_nvram.txt` | `wl -i wl1 dump nvram` |
| `wl1_srom_raw.txt` | `wl -i wl1 srdump`, 234 word, sromrev 11 |
| `wl0_srom_raw.txt` | `wl -i wl0 srdump`, 220 word, **sromrev 8**, deviceid `0x4354`, boardid `0x566`. E' il core N-PHY interno del SoC: fuori dal perimetro del port, tenuto per documentare chi e' chi |
| `wl1_otp_dump.txt` | `wl -i wl1 otpdump`. Tutto zero tranne `0x000c: 0x1fa5` e `0x0020: 0x0500` |
| `dmesg-wl.txt` | `dmesg \| grep wl` del boot |

## La SROM non e' hardware

```
wl:srom/otp not programmed, using main memory mapped srom info(wombo board)
wl: loading /etc/wlan/bcm4360_map.bin
wl: reading /etc/wlan/bcmcmn_nvramvars.bin, file size=32
```

Non c'e' ne' SROM ne' OTP programmata, ed e' il motivo per cui l'`otpdump` e'
vuoto. Quel che `srdump` stampa e' il contenuto di un file su flash mappato in
memoria, quindi **il dato di riferimento vero e' il binario**, come per il
DSL-3580L che in repo ha `bcm43b3_3580l_map.bin` da 480 byte. Mancano:

```
/etc/wlan/bcm4360_map.bin        la mappa di wl1
/etc/wlan/bcmcmn_nvramvars.bin   32 byte, letti dopo la mappa, quindi possono
                                 sovrascriverne campi: senza questo la mappa
                                 da sola non spiega la configurazione effettiva
/etc/wlan/bcm6362_map.bin        la mappa di wl0, per completezza
```

Manca anche `wl -i wl1 phytable`, nella stessa forma usata per l'agcombo.

Il `macaddr=12:13:31:f6:da:77` ha il bit locally-administered acceso e viene
dal file di mappa: e' un segnaposto, non l'indirizzo di fabbrica. Nota per chi
legge la SROM a mano: `srom[4]-[6]` **non** e' il MAC -- quelle tre word qui
sono identiche a quelle del d6220 mentre i due `macaddr` differiscono.

## Ricaricare `wl` senza uccidere il router

Su questo firmware `rmmod wl` + `insmod wl` da soli mandano il router in crash.
Non e' una sola causa, sono tre indipendenti, e la procedura che le copre tutte
e' `reverse-tools/capture_cold_tg789vac.sh`. Misurato su questa unita':

**1. La memoria dei moduli wireless e' riservata e non si libera.** Il kernel
Technicolor (variante del meccanismo `CONFIG_BCM_KF_DSP` per `dspdd` che sta in
`kernel/module.c` dell'SDK Broadcom 4.16L, mirror pubblico
`unofficial-inteno-public-mirror/bcmlinux`) seleziona `wl`, `wlemf` e `wfd` per
nome e li mette in una regione fisica contigua fuori da vmalloc: 5 MiB per i
core da `0x80b8e000` a `0x8108e000`, poi ~1.45 MiB per gli init, da
`0x8108e000` a `0x81200000`, che in `/proc/iomem` compaiono come buco. Il core
e' un bump allocator page-aligned che non torna mai indietro, l'init e' un
puntatore fisso riusato:

```
Load wl module core 80b8e000 (size 9614)      wfd
Load wl module core 80b91000 (size 36789)     wlemf
Load wl module core 80b9a000 (size 4101315)   wl      -> cursore a 0x80f84000, liberi 1.04 MiB
Load wl module core   (null) (size 4101315)   dopo un rmmod: 3.91 MiB richiesti, 1.04 liberi
```

Il cursore e' la word a **`0x80575464`** (trovata con `memfind`: unico hit del
valore `0x80f84000` in tutta la RAM, adiacente ai due puntatori dell'init a
`0x8108e000`). `wl_diag` con `bump_ptr=0x80575464 restore_alloc=1` lo riporta a
`module_core` al `GOING` di `wl`, se e solo se il blocco e' in testa, e al
`COMING` successivo verifica che il nuovo `module_core` coincida. Riuso
confermato su tre cicli consecutivi. L'indirizzo vale per questa build del
kernel e non e' portabile: si ritrova con
`memfind 0x2000 0xb8c000 0x8108e000 0x80f84000`.

**2. hostapd tiene aperto `/dev/wl_event`** (char device major 229, nodo statico
creato da `init_broadcom.sh`), la cui `file_operations` sta dentro il blocco di
`wl`; il `rmmod` passa lo stesso. Riscrivere il blocco con il ricarico produce
due firme a seconda di quando hostapd fa la `read()`: durante la rilocazione,
`Oops in vfs_read` con `epc` uguale a un offset dell'immagine (word `R_MIPS_32`
non ancora rilocata: misurato, `epc == 0x001fe34c`, 91 ms dopo `Load wl module
core`); dopo, `private_data` liberata e crash al primo evento, cioe' al primo
`up`. Per questo i cicli passavano a caso. Si ferma con
`/etc/init.d/hostapd stop` (kill del pid, nessun respawn) prima del primo
`rmmod`; con lui fermo cade anche il 2.4 GHz.

**3. `/lib/wireless/init_broadcom.sh` configura l'istanza e gira una volta per
boot** (`/tmp/hostapd_init_once`): `nar 0`, `phycal_tempdelta 40` (la nvram qui
ha 0, vedi tabella sopra), soglie radar per `wl1` con up/down, affinita' del
kthread. Un `wl` ricaricato senza rifarle ha i default Broadcom e ricalibra in
momenti diversi dal boot. Lo script riapplica nar e tempdelta per ciclo, e a
fine corsa rimuove il file e rilancia hostapd, che rifa' tutto sull'istanza che
resta in esercizio.

Da tenere d'occhio, non risolto: a ogni `rmmod` il FAP stampa
`dqmHandlerRegisterHost: Exceeded maximum number of DQM IRQ Handlers! (8)`, due
volte per interfaccia. E' un leak di handler all'unbind di `wfd` con un tetto di
8: il numero di ricarichi per boot ha un limite, lo script lo conta.

## Prima di usarle come prova, quando ci saranno le catture

1. `reverse-tools/check_class_coverage.py`, e la riga in
   `router-data/CLASS-COVERAGE.md`. Un'assenza vale come prova solo se la
   classe e' tracciata in quella cattura.
2. Le catture conterranno l'attach di `wl0`: gli hook sono sulle funzioni di
   `wl` e non per-core. Si attribuisce con la cpu e coi registri nominati
   (`0x0078`, `0x008f`, `0x00a5`-`0x00a7` sono N-PHY e di nessun altro), non
   col salto di tempo. Metodo in `dsl3580l/README.md`, che ha la stessa
   configurazione a due core.
3. Armare il tracer con `skipphyrd="0x253,0x254"`; `0x251`/`0x252` vanno
   tenuti.
4. `wlc_dfs_cacstate_init` compare nel boot log, quindi la macchina a stati
   DFS gira e i canali con guardia radar sono esercitabili.

## Cosa va toccato nel codice, quando serve

- `test/unit/main.c`: la tabella dei board (`d6220 | agcombo | dsl`), il
  readplan `0x0270` e gli override dei semi di lettura. I semi non verificati
  restano su quelli del d6220, e va detto quali;
- `test/unit/gates.sh`, se il board entra nei gate;
- le costanti di `src/` che risultassero per-board o per-ucode invece che
  per-chip: e' il motivo per cui questo board serve.
