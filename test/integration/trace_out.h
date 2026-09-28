/* SPDX-License-Identifier: GPL-2.0 */
/*
 * L'emissione delle op, nel formato di ../unit/wrap.c.
 *
 * Una riga per op, come la produce l'harness delle unit: `cpuN <classe>
 * <operandi>`. Il confronto con la cattura del vendor lo fanno gli stessi
 * strumenti (unit/compare.py, reverse-tools/tracelib.py), quindi il formato
 * non e' una scelta libera.
 *
 * Le letture: senza oracolo ritornano zero, e per un bit di stato
 * auto-azzerante zero e' il valore che fa girare un poll fino al timeout. E'
 * lo stesso problema che ../unit risolve con AC_READ_ORACLE e i read plan, e
 * qui va risolto con gli stessi, non reinventato.
 */
#ifndef B43_TEST_TRACE_OUT_H
#define B43_TEST_TRACE_OUT_H

#include <linux/types.h>

void b43_trace_op(const char *cls, u16 addr, u32 val, u16 mask, int has_mask);
void b43_trace_macctl(u32 val);
void b43_trace_gpio(const char *cls, u32 val, u32 mask);
void b43_trace_op32(const char *cls, u16 addr, u32 val, u32 mask);
void b43_trace_shm(const char *cls, u32 routing_off, u32 val, int width);
void b43_trace_raw(const char *cls, u16 off, u32 val, int width);
void b43_trace_block(const char *cls, u16 off, unsigned long count,
		     u8 reg_width);
void b43_trace_fill(void *buf, unsigned long count, u16 off, u8 reg_width);
u32  b43_trace_read(const char *cls, u16 addr, int width);
u32  b43_trace_read_raw(u16 off, int width);
/* Se la cattura porta almeno una lettura di (cls, addr). */
int  b43_trace_has(const char *cls, u16 addr);
/* Le letture di shared memory, con la coda dello spazio `routing` (i 16 bit
 * alti di B43_MMIO_SHM_CONTROL, il `sel=` della cattura). */
u32  b43_trace_read_shm(u32 routing, u16 addr, int width);
/* Consuma la voce in testa a quella coda se vale `val`: la lettura l'ha
 * servita un modello, ed e' la stessa che il vendor ha fatto. */
void b43_trace_consume_shm_if(u32 routing, u16 addr, u32 val);

/*
 * Lettura di una variabile d'ambiente intera, per gli stub che ne hanno
 * bisogno ma sono compilati coi flag del kernel e non hanno getenv:
 * subsystem_stub.c la usa per il canale e la larghezza. Accetta decimale e
 * 0x...; se la variabile manca o non e' un numero ritorna `def`.
 */
const char *b43_test_env(const char *name);
long b43_test_env_long(const char *name, long def);

/* Una riga di diagnostica della suite su stderr, con un solo intero. */
void b43_trace_note(const char *fmt, int arg);

/*
 * La timeline dell'ambiente, da B43_TIMELINE: il file che
 * reverse-tools/timeline.py scrive, `<t> <ep> <EVENTO>` per riga. Gli istanti
 * sono in microsecondi. _wd() da' il primo e l'ultimo giro del watchdog del
 * vendor e quanti sono; _next() l'evento successivo, 0 alla fine.
 */
int b43_test_timeline_wd(long long *first_us, long long *last_us, int *n);
int b43_test_timeline_next(long long *t_us, char *kind, int len);

#endif /* B43_TEST_TRACE_OUT_H */
