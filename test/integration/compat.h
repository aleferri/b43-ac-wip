/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cio' che `src/` usa e gli header kernel installati non hanno.
 *
 * Non e' una comodita': senza, `-w` nasconde la dichiarazione implicita e
 * `kzalloc_obj(*p)` compila passando una struct per valore e trattando un
 * `int` come puntatore. Il link non se ne accorge e il programma muore in
 * `b43_phy_ac_op_allocate`. Per questo il Makefile ora tiene
 * `-Werror=implicit-function-declaration` anche con `-w`.
 *
 * `kzalloc_obj` e' una macro dei kernel piu' recenti di 6.8. Che `src/` la
 * usi vuol dire che punta a un kernel piu' nuovo degli header con cui la
 * suite compila: e' un fatto da sapere, non da nascondere.
 */
#ifndef B43_TEST_COMPAT_H
#define B43_TEST_COMPAT_H

#include <linux/slab.h>

#ifndef kzalloc_obj
#define kzalloc_obj(obj)	kzalloc(sizeof(obj), GFP_KERNEL)
#endif

#endif /* B43_TEST_COMPAT_H */
