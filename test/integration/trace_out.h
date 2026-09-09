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
void b43_trace_shm(const char *cls, u32 routing_off, u32 val, int width);
void b43_trace_raw(const char *cls, u16 off, u32 val, int width);
void b43_trace_block(const char *cls, u16 off, unsigned long count,
		     u8 reg_width);
void b43_trace_fill(void *buf, unsigned long count, u16 off, u8 reg_width);
u32  b43_trace_read(const char *cls, u16 addr, int width);
u32  b43_trace_read_raw(u16 off, int width);

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

#endif /* B43_TEST_TRACE_OUT_H */
