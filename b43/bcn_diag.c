// SPDX-License-Identifier: GPL-2.0
/*
 * Beacon diagnostic for the AC bring-up.
 *
 * Logs, through b43info(), what the beacon path cannot show otherwise: the
 * frame b43 uploads and the shared-memory cells that go with it, the words
 * the microcode changes in template RAM afterwards, the MACCMD valid bits at
 * each beacon interrupt, and the management frames that go through the data
 * TX path with their TX status.
 */

#include <linux/ieee80211.h>
#include <linux/printk.h>

#include "b43.h"
#include "bcn_diag.h"
#include "xmit.h"

/*
 * Shared-memory word 0x75: the AC microcode adds one to it on every TBTT it
 * serves (784 at 0x0414, 928 at 0x04f7), right before the template goes out.
 */
#define B43_BCN_DIAG_TBTT_COUNT		0x00EA

/*
 * Shared-memory word 0xA6: the AC microcode adds one to it for every probe
 * response it gave up on because the request was older than PRMAXTIME (784 at
 * 0x0277). With HOSTF5 bit 15 set it is also the count of probe requests the
 * host never saw.
 */
#define B43_BCN_DIAG_PRS_TIMEOUTS	0x014C

#define B43_BCN_DIAG_MAX_DIFFS		8

static u32 b43_bcn_diag_ram_read(struct b43_wldev *dev, u16 offset, bool swap)
{
	u32 val;

	b43_write32(dev, B43_MMIO_RAM_CONTROL, offset);
	val = b43_read32(dev, B43_MMIO_RAM_DATA);
	return swap ? swab32(val) : val;
}

/* Words of template RAM that differ from the shadow; the first few logged. */
static unsigned int b43_bcn_diag_compare(struct b43_wldev *dev,
					 unsigned int idx,
					 const struct b43_bcn_shadow *s,
					 const char *when)
{
	bool swap = b43_read32(dev, B43_MMIO_MACCTL) & B43_MACCTL_BE;
	unsigned int i, n = DIV_ROUND_UP(s->total, 4), diffs = 0;

	for (i = 0; i < n; i++) {
		u32 rd = b43_bcn_diag_ram_read(dev, s->base + 4 * i, swap);

		if (rd == s->words[i])
			continue;
		if (diffs < B43_BCN_DIAG_MAX_DIFFS)
			b43info(dev->wl,
				"bcn%u %s: tpl+0x%03x %08x -> %08x\n",
				idx, when, 4 * i, s->words[i], rd);
		diffs++;
	}
	if (diffs > B43_BCN_DIAG_MAX_DIFFS)
		b43info(dev->wl, "bcn%u %s: %u more word(s) differ\n",
			idx, when, diffs - B43_BCN_DIAG_MAX_DIFFS);
	return diffs;
}

void b43_bcn_diag_uploaded(struct b43_wldev *dev, unsigned int idx,
			   const u8 *frame, u16 len, u16 rate,
			   u16 tim_position, u16 dtim_period)
{
	const struct b43_bcn_shadow *s = &dev->wl->diag.tpl[idx];
	char line[16 * 3 + 1];
	unsigned int i;

	b43info(dev->wl,
		"bcn%u up: base 0x%03x total %u len %u rate 0x%02x timbpos 0x%03x dtimper %u phyctl 0x%04x\n",
		idx, s->base, s->total, len, rate, tim_position, dtim_period,
		b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_BEACPHYCTL_AC));

	for (i = 0; i < len; i += 16) {
		hex_dump_to_buffer(frame + i, min_t(unsigned int, 16, len - i),
				   16, 1, line, sizeof(line), false);
		b43info(dev->wl, "bcn%u %03x: %s\n", idx, i, line);
	}

	if (!b43_bcn_diag_compare(dev, idx, s, "readback"))
		b43info(dev->wl, "bcn%u readback: matches\n", idx);
}

void b43_bcn_diag_irq(struct b43_wldev *dev, u32 cmd)
{
	struct b43_wl *wl = dev->wl;

	b43info(wl, "bcn irq: maccmd 0x%08x valid0 %d valid1 %d virgin %d uploaded %d/%d\n",
		cmd, !!(cmd & B43_MACCMD_BEACON0_VALID),
		!!(cmd & B43_MACCMD_BEACON1_VALID),
		wl->beacon_templates_virgin,
		wl->beacon0_uploaded, wl->beacon1_uploaded);
}

void b43_bcn_diag_tick(struct b43_wldev *dev)
{
	unsigned int i;

	b43info(dev->wl,
		"bcn tick: maccmd 0x%08x btl0 %u btl1 %u timbpos 0x%03x dtimper %u phyctl 0x%04x tbtt %u hostf5 0x%04x prs-timeouts %u\n",
		b43_read32(dev, B43_MMIO_MACCMD),
		b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_BTL0),
		b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_BTL1),
		b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_TIMBPOS),
		b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_DTIMPER),
		b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_BEACPHYCTL_AC),
		b43_shm_read16(dev, B43_SHM_SHARED, B43_BCN_DIAG_TBTT_COUNT),
		b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_HOSTF5),
		b43_shm_read16(dev, B43_SHM_SHARED, B43_BCN_DIAG_PRS_TIMEOUTS));

	for (i = 0; i < ARRAY_SIZE(dev->wl->diag.tpl); i++) {
		const struct b43_bcn_shadow *s = &dev->wl->diag.tpl[i];

		if (s->valid)
			b43_bcn_diag_compare(dev, i, s, "tick");
	}
}

void b43_bcn_diag_tx(struct b43_wldev *dev, const struct sk_buff *skb)
{
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)skb->data;

	if (!ieee80211_is_mgmt(hdr->frame_control))
		return;
	b43info(dev->wl, "tx mgmt: stype 0x%x len %u to %pM\n",
		(le16_to_cpu(hdr->frame_control) & IEEE80211_FCTL_STYPE) >> 4,
		skb->len, hdr->addr1);
}

void b43_bcn_diag_txstatus(struct b43_wldev *dev, const struct sk_buff *skb,
			   const struct b43_txstatus *status)
{
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)skb->data;

	if (!ieee80211_is_mgmt(hdr->frame_control))
		return;
	b43info(dev->wl,
		"txstatus mgmt: stype 0x%x len %u acked %u frames %u rts %u supp %u raw %08x %08x\n",
		(le16_to_cpu(hdr->frame_control) & IEEE80211_FCTL_STYPE) >> 4,
		skb->len, status->acked, status->frame_count,
		status->rts_count, status->supp_reason,
		status->raw[0], status->raw[1]);
}
