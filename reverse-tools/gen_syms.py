#!/usr/bin/env python3
"""gen_syms.py - costruisce la riga insmod di wl-diag da un dump di
/proc/kallsyms copiato dal device, e dice quali hook si risolveranno.

Il busybox dei firmware Broadcom stock non ha grep -E / awk / nm, quindi il
matching non si puo' fare sul router: si copia giu' /proc/kallsyms una volta e
lo si macina qui sul PC.

Uso:
    ssh ... "cat /proc/kallsyms" > kallsyms-dsl.txt
    ./gen_syms.py kallsyms-dsl.txt

Stampa la riga con `klookup=<indirizzo di kallsyms_lookup_name>`, che e' l'unica
strada: il modulo risolve gli altri simboli da se', e li RIRISOLVE a ogni
ricaricamento del bersaglio. Passare invece gli indirizzi a mano li fisserebbe
all'insmod di wl_diag, e dopo un `rmmod wl` punterebbero a memoria che non e'
piu' di `wl`: incompatibile con la cattura a freddo, che e' il motivo per cui
questo tracer esiste.

Il report sui singoli simboli resta, ed e' la parte utile: dice in anticipo
quali hook si risolveranno e quali no, senza dover caricare niente sul device.
La lista dei nomi combacia con hooks[] in wl-diag-2630/wl_diag.c piu'
r4k_flush_icache_range, che serve al momento dell'arm. Se cambia li',
aggiornala qui. --module wl restringe il match ai simboli di quel modulo,
utile se un nome collide con un simbolo del kernel.
"""
import argparse
import sys

WANTED = [
    # Combacia con hooks[] di wl-diag-2630/wl_diag.c, generata da la'.
    # Se si aggiunge un hook, va aggiunto qui: altrimenti il report
    # dice "tutto risolto" su una lista incompleta.
    "phy_reg_read", "phy_reg_write", "phy_reg_mod",
    "phy_reg_and", "phy_reg_or", "write_radio_reg",
    "mod_radio_reg", "si_pmu_chipcontrol", "si_pmu_regcontrol",
    "si_pmu_pllcontrol", "si_corereg", "si_gpiocontrol",
    "si_gpioout", "si_gpioouten", "wlc_phy_table_read_acphy",
    "wlc_phy_table_write_acphy", "osl_delay", "wlc_bmac_mctrl",
    "wlc_bmac_mhf", "wlc_bmac_mhf_get", "wlc_bmac_write_template_ram",
    "otp_init", "otp_read_word", "otp_read_region",
    "wlc_phy_cal_init", "wlc_bmac_bw_set", "si_get_sromctl",
    "si_set_sromctl", "wlc_phy_chanspec_set", "wlc_bmac_read_objmem",
    "wlc_bmac_write_objmem", "wlc_bmac_read_shm", "wlc_bmac_write_shm",
    "wlc_bmac_copyfrom_objmem", "wlc_bmac_copyto_objmem", "wlc_phy_chanspec_shm_set",
    "wlc_bmac_set_addrmatch", "wlc_set_addrmatch", "wlc_bmac_write_amt",
    "wlc_bmac_set_rcmta", "phy_reg_write_array", "phy_reg_read_wide",
    "phy_reg_write_wide", "wlc_bmac_write_ihr", "wlc_bmac_set_shm",
    "read_radio_reg",
    # non e' un hook: serve al momento dell'arm per il flush i-cache
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
    args = ap.parse_args()

    # Il match sui nomi degli hook si restringe al modulo se chiesto; per
    # kallsyms_lookup_name no, che e' del kernel.
    hook_table = parse_kallsyms(args.kallsyms, args.module)
    kern_table = parse_kallsyms(args.kallsyms, None)

    risolti, missing = [], []
    for name in WANTED:
        addr = hook_table.get(name)
        if addr and int(addr, 16) != 0:
            risolti.append(name)
        else:
            missing.append(name)

    print(f"# hook che si risolveranno: {len(risolti)}/{len(WANTED)}",
          file=sys.stderr)
    if missing:
        print(f"# NON risolti: {' '.join(missing)}", file=sys.stderr)
        print("# se molti mancano, questa versione di wl puo' usare nomi "
              "diversi; ispeziona il dump per gli equivalenti "
              "(phy_reg*, *radio_reg, si_corereg, wlc_phy_table_*).",
              file=sys.stderr)

    addr = kern_table.get("kallsyms_lookup_name")
    if not addr or int(addr, 16) == 0:
        print("gen_syms: kallsyms_lookup_name non trovato nel dump: senza di "
              "essa il modulo non puo' risolvere niente", file=sys.stderr)
        return 1
    print(f'insmod wl_diag.ko arm={args.arm} klookup=0x{addr}')
    return 0


if __name__ == "__main__":
    sys.exit(main())
