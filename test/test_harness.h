/* SPDX-License-Identifier: GPL-2.0
 * Test-harness public API. Included by main.c and wrap.c only; the
 * scratch driver code must NOT depend on this file.
 *
 * Read plans: before invoking a flow, register a fixed sequence of
 * return values for each address that the code polls or otherwise
 * consumes. The i-th read of that address returns results[i] and bumps
 * the cursor; past `cap` the read returns 0.
 *
 * Registering the same address twice REPLACES the plan and resets the
 * cursor.
 */
#ifndef _TEST_HARNESS_H
#define _TEST_HARNESS_H

#include <stdio.h>
#include "b43.h"

void b43_test_plan_phy_reads(u16 addr, const u16 *results, int cap);
/* Plan per cella di tabella: indipendente dalla posizione nella coda 0x000f. */
void b43_test_plan_table_cell(u16 id, u16 off, const u16 *vals, int n);
void b43_test_plan_radio_reads(u16 addr, const u16 *results, int cap);

/* Address match table: un record per riga, la granularita' del tracer.
 * Doppione di b43_amt_write() di patches/0011, che vive nel core e che
 * l'harness non compila. */
void b43_test_emit_amt(u16 idx, u16 flags);
void b43_test_plan_mmio_reads(u16 addr, const u16 *results, int cap);
void b43_test_plans_reset(void);
void b43_test_plans_report(FILE *f);

/*
 * Pre-populate the radio mirror with a real hardware value (used when a
 * flow relies on state established by an earlier init pass we don't
 * re-execute — e.g. R2069_RCCAL_E/F filled by radio_2069_init before
 * switch_channel runs).
 */
void b43_test_mirror_radio_set(u16 reg, u16 val);
void b43_test_mirror_phy_set(u16 reg, u16 val);
u16 b43_test_mirror_phy_get(u16 reg);
void b43_test_trace_to(FILE *f);

/*
 * Riepilogo del read oracle (AC_READ_ORACLE): quante letture sono state
 * servite dalla cattura e quali code non sono state consumate del tutto.
 */
void b43_test_oracle_report(void);

/* Riporta il contatore MAC allo stato d'ingresso dei flow: MAC sospeso. */
void b43_test_mac_reset(void);

#endif
