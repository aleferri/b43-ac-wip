/* SPDX-License-Identifier: GPL-2.0 */
#ifndef B43_BCN_DIAG_H_
#define B43_BCN_DIAG_H_

#include <linux/types.h>

struct b43_wldev;
struct sk_buff;
struct b43_txstatus;

/* Largest beacon template: 0x280 bytes on the 832/928 layout. */
#define B43_BCN_DIAG_WORDS	(0x280 / 4)

/*
 * One beacon template as b43 uploaded it, to tell what the microcode and the
 * hardware change in template RAM afterwards.
 */
struct b43_bcn_shadow {
	bool valid;
	u16 base;
	u16 total;
	u32 words[B43_BCN_DIAG_WORDS];
};

struct b43_bcn_diag {
	struct b43_bcn_shadow tpl[2];
};

void b43_bcn_diag_uploaded(struct b43_wldev *dev, unsigned int idx,
			   const u8 *frame, u16 len, u16 rate,
			   u16 tim_position, u16 dtim_period);
void b43_bcn_diag_irq(struct b43_wldev *dev, u32 cmd);
void b43_bcn_diag_tick(struct b43_wldev *dev);
void b43_bcn_diag_tx(struct b43_wldev *dev, const struct sk_buff *skb);
void b43_bcn_diag_txstatus(struct b43_wldev *dev, const struct sk_buff *skb,
			   const struct b43_txstatus *status);

#endif /* B43_BCN_DIAG_H_ */
