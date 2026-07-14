# wl-diag — tracer inline-detour per il driver `wl`

Modulo kernel che aggancia gli accessor PHY/radio/PMU/MAC del driver Broadcom
`wl` senza kprobe (detour all'ingresso funzione), ed espone i record su un
misc-device. Vedi la testata di `wl_diag.c` per i dettagli del meccanismo e i
limiti (MIPS32R1, memoria modulo RWX, read con valore UNDEFINED).

Cosa cattura ora: PHY rd/wr/mod, **PHY and/or distinti** (op 19/20), radio
rd/wr/mod, PMU cc/rc/pll, GPIO ChipCommon, tabelle acphy, e il **controllo verso
il MAC** (`wlc_bmac_mctrl`, `wlc_bmac_mhf`/`_get`). `osl_delay` e' opt-in
(`delay=1`); di default staccato.

## Parametri

| param | default | effetto |
|-------|---------|---------|
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

### 0. Host: listener pronto PRIMA (UDP, i record sono 28 B << MTU)

```sh
nc -u -l -p 5555 | python3 decode-wl-diag.py | tee trace.txt
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
minor=$(awk '$2=="wl_diag"{print $1}' /proc/misc)   # o: cat /sys/class/misc/wl_diag/dev
mknod /tmp/wl_diag c 10 "$minor"
```

### 5. Avvia la pipe verso l'host (PRIMA del rescan, per non perdere record)

```sh
cat /tmp/wl_diag | nc -u <HOST> 5555 &
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
- **Consumatore a valle.** `decode-wl-diag.py` (stream 28 B) e, per la
  correlazione op->driver, `reverse-tools/correlate_trace_to_driver.py` (che
  ora riconosce `PHY.AND`/`PHY.OR`).
