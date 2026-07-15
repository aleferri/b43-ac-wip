#!/usr/bin/env python3
"""gen_syms.py - costruisce la stringa syms= per wl-diag da un dump di
/proc/kallsyms copiato dal device.

Il busybox dei firmware Broadcom stock non ha grep -E / awk / nm, quindi il
matching non si puo' fare sul router: si copia giu' /proc/kallsyms una volta e
lo si macina qui sul PC. I simboli hook del driver wl (non esportati ma
presenti in kallsyms quando il modulo non e' strippato) vengono risolti e si
stampa la riga insmod pronta da incollare sul device.

Uso:
    ssh ... "cat /proc/kallsyms" > kallsyms-dsl.txt
    ./gen_syms.py kallsyms-dsl.txt

La lista dei nomi combacia con hooks[] in wl-diag-2630/wl_diag.c piu'
r4k_flush_icache_range, che serve al momento dell'arm. Se cambia li',
aggiornala qui. --module wl restringe il match ai simboli di quel modulo,
utile se un nome collide con un simbolo del kernel.
"""
import argparse
import sys

WANTED = [
    "phy_reg_read", "phy_reg_write", "phy_reg_mod", "phy_reg_and", "phy_reg_or",
    "write_radio_reg", "read_radio_reg", "mod_radio_reg",
    "si_pmu_chipcontrol", "si_pmu_regcontrol", "si_pmu_pllcontrol", "si_corereg",
    "si_gpiocontrol", "si_gpioout", "si_gpioouten",
    "wlc_phy_table_read_acphy", "wlc_phy_table_write_acphy",
    "wlc_bmac_mctrl", "wlc_bmac_mhf", "wlc_bmac_mhf_get", "osl_delay",
    "r4k_flush_icache_range",
]


def parse_kallsyms(path, module):
    """nome -> addr(hex str). Righe: 'ADDR TYPE NAME [ \\[MODULE\\]]'."""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            addr, _typ, name = parts[0], parts[1], parts[2]
            if module:
                tag = parts[3] if len(parts) >= 4 else None
                # tieni i simboli del modulo voluto e quelli del kernel
                # (senza tag, come r4k_flush_icache_range); scarta gli altri moduli
                if tag is not None and tag != f"[{module}]":
                    continue
            if name not in out:            # prima occorrenza: kallsyms e' ordinato per addr
                out[name] = addr
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kallsyms", help="dump di /proc/kallsyms dal device")
    ap.add_argument("--module",
                    help="restringi il match ai simboli di questo modulo (es. wl)")
    ap.add_argument("--arm", type=int, default=0,
                    help="valore del parametro arm nella riga insmod (default 0)")
    ap.add_argument("--klookup", action="store_true",
                    help="stampa la riga con klookup=<addr di kallsyms_lookup_name> "
                         "invece della lista syms= (il modulo risolve il resto)")
    args = ap.parse_args()

    table = parse_kallsyms(args.kallsyms, args.module if not args.klookup else None)

    if args.klookup:
        addr = table.get("kallsyms_lookup_name")
        if not addr or int(addr, 16) == 0:
            print("gen_syms: kallsyms_lookup_name non trovato nel dump",
                  file=sys.stderr)
            return 1
        print(f'insmod wl_diag.ko arm={args.arm} klookup=0x{addr}')
        return 0

    pairs = []
    missing = []
    for name in WANTED:
        addr = table.get(name)
        if addr and int(addr, 16) != 0:
            pairs.append(f"{name}:{addr}")
        else:
            missing.append(name)

    if missing:
        print(f"gen_syms: NON risolti: {' '.join(missing)}", file=sys.stderr)
        print("gen_syms: se molti mancano, questa versione di wl puo' usare "
              "nomi diversi; ispeziona il dump per gli equivalenti "
              "(phy_reg*, *radio_reg, si_corereg, wlc_phy_table_*).",
              file=sys.stderr)
    if not pairs:
        print("gen_syms: nessun simbolo risolto", file=sys.stderr)
        return 1

    print(f'insmod wl_diag.ko arm={args.arm} syms="{",".join(pairs)}"')
    return 0


if __name__ == "__main__":
    sys.exit(main())
