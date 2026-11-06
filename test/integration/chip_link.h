/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Link to b43-tools' virtual chip (interpreter/vchipd.py). B43_VCHIP holds the
 * command that starts it; when set, the 802.11 core's MMIO and the passing of
 * time go to the chip instead of the oracle. The board's options follow it,
 * then B43_VCHIP_ARGS.
 */
#ifndef B43_TEST_CHIP_LINK_H
#define B43_TEST_CHIP_LINK_H

/* vchipd's options for the selected board; before the first access. */
void b43_chip_board(const char *args);
int b43_chip_active(void);
unsigned int b43_chip_read(unsigned short off, int width);
void b43_chip_write(unsigned short off, unsigned int val, int width);
void b43_chip_time(unsigned long us);
/* GEN_IRQ_REASON & IRQ_MASK as of the chip's last reply. */
unsigned int b43_chip_irq(void);

#endif /* B43_TEST_CHIP_LINK_H */
