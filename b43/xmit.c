// SPDX-License-Identifier: GPL-2.0-or-later
/*

  Broadcom B43 wireless driver

  Transmission (TX/RX) related functions.

  Copyright (C) 2005 Martin Langer <martin-langer@gmx.de>
  Copyright (C) 2005 Stefano Brivio <stefano.brivio@polimi.it>
  Copyright (C) 2005, 2006 Michael Buesch <m@bues.ch>
  Copyright (C) 2005 Danny van Dyk <kugelfang@gentoo.org>
  Copyright (C) 2005 Andreas Jaggi <andreas.jaggi@waterwave.ch>


*/

#include "xmit.h"
#include "phy_common.h"
#include "phy_ac.h"
#include "dma.h"
#include "pio.h"

static const struct b43_tx_legacy_rate_phy_ctl_entry b43_tx_legacy_rate_phy_ctl[] = {
	{ B43_CCK_RATE_1MB,	0x0,			0x0 },
	{ B43_CCK_RATE_2MB,	0x0,			0x1 },
	{ B43_CCK_RATE_5MB,	0x0,			0x2 },
	{ B43_CCK_RATE_11MB,	0x0,			0x3 },
	{ B43_OFDM_RATE_6MB,	B43_TXH_PHY1_CRATE_1_2,	B43_TXH_PHY1_MODUL_BPSK },
	{ B43_OFDM_RATE_9MB,	B43_TXH_PHY1_CRATE_3_4,	B43_TXH_PHY1_MODUL_BPSK },
	{ B43_OFDM_RATE_12MB,	B43_TXH_PHY1_CRATE_1_2,	B43_TXH_PHY1_MODUL_QPSK },
	{ B43_OFDM_RATE_18MB,	B43_TXH_PHY1_CRATE_3_4,	B43_TXH_PHY1_MODUL_QPSK },
	{ B43_OFDM_RATE_24MB,	B43_TXH_PHY1_CRATE_1_2,	B43_TXH_PHY1_MODUL_QAM16 },
	{ B43_OFDM_RATE_36MB,	B43_TXH_PHY1_CRATE_3_4,	B43_TXH_PHY1_MODUL_QAM16 },
	{ B43_OFDM_RATE_48MB,	B43_TXH_PHY1_CRATE_2_3,	B43_TXH_PHY1_MODUL_QAM64 },
	{ B43_OFDM_RATE_54MB,	B43_TXH_PHY1_CRATE_3_4,	B43_TXH_PHY1_MODUL_QAM64 },
};

static const struct b43_tx_legacy_rate_phy_ctl_entry *
b43_tx_legacy_rate_phy_ctl_ent(u8 bitrate)
{
	const struct b43_tx_legacy_rate_phy_ctl_entry *e;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_tx_legacy_rate_phy_ctl); i++) {
		e = &(b43_tx_legacy_rate_phy_ctl[i]);
		if (e->bitrate == bitrate)
			return e;
	}

	B43_WARN_ON(1);
	return NULL;
}

/* Extract the bitrate index out of a CCK PLCP header. */
static int b43_plcp_get_bitrate_idx_cck(struct b43_plcp_hdr6 *plcp)
{
	switch (plcp->raw[0]) {
	case 0x0A:
		return 0;
	case 0x14:
		return 1;
	case 0x37:
		return 2;
	case 0x6E:
		return 3;
	}
	return -1;
}

/* Extract the bitrate index out of an OFDM PLCP header. */
static int b43_plcp_get_bitrate_idx_ofdm(struct b43_plcp_hdr6 *plcp, bool ghz5)
{
	/* For 2 GHz band first OFDM rate is at index 4, see main.c */
	int base = ghz5 ? 0 : 4;

	switch (plcp->raw[0] & 0xF) {
	case 0xB:
		return base + 0;
	case 0xF:
		return base + 1;
	case 0xA:
		return base + 2;
	case 0xE:
		return base + 3;
	case 0x9:
		return base + 4;
	case 0xD:
		return base + 5;
	case 0x8:
		return base + 6;
	case 0xC:
		return base + 7;
	}
	return -1;
}

u8 b43_plcp_get_ratecode_cck(const u8 bitrate)
{
	switch (bitrate) {
	case B43_CCK_RATE_1MB:
		return 0x0A;
	case B43_CCK_RATE_2MB:
		return 0x14;
	case B43_CCK_RATE_5MB:
		return 0x37;
	case B43_CCK_RATE_11MB:
		return 0x6E;
	}
	B43_WARN_ON(1);
	return 0;
}

u8 b43_plcp_get_ratecode_ofdm(const u8 bitrate)
{
	switch (bitrate) {
	case B43_OFDM_RATE_6MB:
		return 0xB;
	case B43_OFDM_RATE_9MB:
		return 0xF;
	case B43_OFDM_RATE_12MB:
		return 0xA;
	case B43_OFDM_RATE_18MB:
		return 0xE;
	case B43_OFDM_RATE_24MB:
		return 0x9;
	case B43_OFDM_RATE_36MB:
		return 0xD;
	case B43_OFDM_RATE_48MB:
		return 0x8;
	case B43_OFDM_RATE_54MB:
		return 0xC;
	}
	B43_WARN_ON(1);
	return 0;
}

void b43_generate_plcp_hdr(struct b43_plcp_hdr4 *plcp,
			   const u16 octets, const u8 bitrate)
{
	__u8 *raw = plcp->raw;

	if (b43_is_ofdm_rate(bitrate)) {
		u32 d;

		d = b43_plcp_get_ratecode_ofdm(bitrate);
		B43_WARN_ON(octets & 0xF000);
		d |= (octets << 5);
		plcp->data = cpu_to_le32(d);
	} else {
		u32 plen;

		plen = octets * 16 / bitrate;
		if ((octets * 16 % bitrate) > 0) {
			plen++;
			if ((bitrate == B43_CCK_RATE_11MB)
			    && ((octets * 8 % 11) < 4)) {
				raw[1] = 0x84;
			} else
				raw[1] = 0x04;
		} else
			raw[1] = 0x04;
		plcp->data |= cpu_to_le32(plen << 16);
		raw[0] = b43_plcp_get_ratecode_cck(bitrate);
	}
}

/* TODO: verify if needed for SSLPN or LCN  */
static u16 b43_generate_tx_phy_ctl1(struct b43_wldev *dev, u8 bitrate)
{
	const struct b43_phy *phy = &dev->phy;
	const struct b43_tx_legacy_rate_phy_ctl_entry *e;
	u16 control = 0;
	u16 bw;

	if (phy->type == B43_PHYTYPE_LP)
		bw = B43_TXH_PHY1_BW_20;
	else /* FIXME */
		bw = B43_TXH_PHY1_BW_20;

	if (0) { /* FIXME: MIMO */
	} else if (b43_is_cck_rate(bitrate) && phy->type != B43_PHYTYPE_LP) {
		control = bw;
	} else {
		control = bw;
		e = b43_tx_legacy_rate_phy_ctl_ent(bitrate);
		if (e) {
			control |= e->coding_rate;
			control |= e->modulation;
		}
		control |= B43_TXH_PHY1_MODE_SISO;
	}

	return control;
}

static u8 b43_calc_fallback_rate(u8 bitrate, int gmode)
{
	switch (bitrate) {
	case B43_CCK_RATE_1MB:
		return B43_CCK_RATE_1MB;
	case B43_CCK_RATE_2MB:
		return B43_CCK_RATE_1MB;
	case B43_CCK_RATE_5MB:
		return B43_CCK_RATE_2MB;
	case B43_CCK_RATE_11MB:
		return B43_CCK_RATE_5MB;
	/*
	 * Don't just fallback to CCK; it may be in 5GHz operation
	 * and falling back to CCK won't work out very well.
	 */
	case B43_OFDM_RATE_6MB:
		if (gmode)
			return B43_CCK_RATE_5MB;
		else
			return B43_OFDM_RATE_6MB;
	case B43_OFDM_RATE_9MB:
		return B43_OFDM_RATE_6MB;
	case B43_OFDM_RATE_12MB:
		return B43_OFDM_RATE_9MB;
	case B43_OFDM_RATE_18MB:
		return B43_OFDM_RATE_12MB;
	case B43_OFDM_RATE_24MB:
		return B43_OFDM_RATE_18MB;
	case B43_OFDM_RATE_36MB:
		return B43_OFDM_RATE_24MB;
	case B43_OFDM_RATE_48MB:
		return B43_OFDM_RATE_36MB;
	case B43_OFDM_RATE_54MB:
		return B43_OFDM_RATE_48MB;
	}
	B43_WARN_ON(1);
	return 0;
}

/* Generate a TX data header. */
/* Index of a legacy rate in PHY TX control word 2 of the AC microcode. */
static u16 b43_txhdr_ac_rate_idx(u8 rate)
{
	switch (rate) {
	case B43_CCK_RATE_1MB:
	case B43_OFDM_RATE_6MB:
		return 0;
	case B43_CCK_RATE_2MB:
	case B43_OFDM_RATE_9MB:
		return 1;
	case B43_CCK_RATE_5MB:
	case B43_OFDM_RATE_12MB:
		return 2;
	case B43_CCK_RATE_11MB:
	case B43_OFDM_RATE_18MB:
		return 3;
	case B43_OFDM_RATE_24MB:
		return 4;
	case B43_OFDM_RATE_36MB:
		return 5;
	case B43_OFDM_RATE_48MB:
		return 6;
	case B43_OFDM_RATE_54MB:
		return 7;
	}
	B43_WARN_ON(1);
	return 0;
}

/*
 * What the TX header formats share, decided once from mac80211's TX info:
 * the rates, the MAC flags and the protection frame. b43_generate_txhdr()
 * writes it in the pre-AC header, b43_generate_txhdr_ac() in the AC one.
 */
struct b43_txhdr_ctl {
	struct ieee80211_rate *fbrate;
	bool vht;		/* rates[0] is a VHT MCS: rate and fbrate unset */
	u8 rate;		/* hw_value of the rate */
	u8 rate_fb;		/* hw_value of the fallback rate */
	u8 rts_rate;		/* hw_value of the RTS or CTS rate */
	bool ack;		/* expect an ACK */
	bool hwseq;		/* hardware sequence number */
	bool first_frag;	/* first fragment of an MSDU */
	bool long_frame;	/* long retry limit */
	bool short_preamble;
	bool rts;		/* RTS before the frame */
	bool cts;		/* CTS-to-self before the frame */
};

/* An index into the band's legacy rate table, which an MCS is not. */
static bool b43_tx_rate_legacy(const struct ieee80211_tx_rate *r)
{
	return r->idx >= 0 &&
	       !(r->flags & (IEEE80211_TX_RC_MCS | IEEE80211_TX_RC_VHT_MCS));
}

static void b43_txhdr_ctl_get(struct b43_wldev *dev,
			      struct ieee80211_tx_info *info,
			      struct b43_txhdr_ctl *c)
{
	struct ieee80211_tx_rate *rates = info->control.rates;
	struct ieee80211_rate *txrate = NULL, *rts_cts_rate;

	c->vht = rates[0].idx >= 0 && (rates[0].flags & IEEE80211_TX_RC_VHT_MCS);
	if (b43_tx_rate_legacy(&rates[0]))
		txrate = ieee80211_get_tx_rate(dev->wl->hw, info);
	c->rate = txrate ? txrate->hw_value : B43_CCK_RATE_1MB;
	c->fbrate = b43_tx_rate_legacy(&rates[1]) ?
		    ieee80211_get_alt_retry_rate(dev->wl->hw, info, 0) : txrate;
	c->rate_fb = c->fbrate ? c->fbrate->hw_value : c->rate;

	c->ack = !(info->flags & IEEE80211_TX_CTL_NO_ACK);
	/* use hardware sequence counter as the non-TID counter */
	c->hwseq = info->flags & IEEE80211_TX_CTL_ASSIGN_SEQ;
	c->first_frag = info->flags & IEEE80211_TX_CTL_FIRST_FRAGMENT;
	c->short_preamble = rates[0].flags & IEEE80211_TX_RC_USE_SHORT_PREAMBLE;

	/* Overwrite rates[0].count to make the retry calculation
	 * in the tx status easier. need the actual retry limit to
	 * detect whether the fallback rate was used.
	 */
	c->long_frame = (rates[0].flags & IEEE80211_TX_RC_USE_RTS_CTS) ||
			(rates[0].count <=
			 dev->wl->hw->conf.long_frame_max_tx_count);
	if (c->long_frame)
		rates[0].count = dev->wl->hw->conf.long_frame_max_tx_count;
	else
		rates[0].count = dev->wl->hw->conf.short_frame_max_tx_count;

	c->cts = rates[0].flags & IEEE80211_TX_RC_USE_CTS_PROTECT;
	c->rts = !c->cts && (rates[0].flags & IEEE80211_TX_RC_USE_RTS_CTS);
	c->rts_rate = B43_CCK_RATE_1MB;
	if (c->rts || c->cts) {
		rts_cts_rate = ieee80211_get_rts_cts_rate(dev->wl->hw, info);
		if (rts_cts_rate)
			c->rts_rate = rts_cts_rate->hw_value;
	}
}

/* One legacy rate, on the primary 20 MHz of the channel. */
static void b43_txhdr_ac_legacy(const struct b43_phy_ac *ac,
				struct b43_txhdr_ac_rate *r,
				const struct b43_txhdr_ctl *c, u16 cores,
				unsigned int len)
{
	u8 rate = c->rate;
	u16 phy0, po;

	phy0 = b43_is_ofdm_rate(rate) ? B43_TXH_PHY_ENC_OFDM :
					B43_TXH_PHY_ENC_CCK;
	phy0 |= B43_TXH_AC_PHY0_NON_SOUNDING;
	phy0 |= cores << B43_TXH_AC_PHY0_CORES_SHIFT;
	if (c->short_preamble)
		phy0 |= B43_TXH_AC_PHY0_SHORT_PREAMBLE;
	r->phy_ctl[0] = cpu_to_le16(phy0);
	po = b43_is_ofdm_rate(rate) ?
	     ac->rate_po_ofdm[b43_txhdr_ac_rate_idx(rate)] : ac->rate_po_cck;
	r->phy_ctl[1] = cpu_to_le16((po & B43_TXH_AC_PHY1_TXPWR_OFFSET) |
				    ((ac->chanspec &
				      B43_PHY_AC_CHANSPEC_SB_MASK) >>
				     B43_PHY_AC_CHANSPEC_SB_SHIFT));
	r->phy_ctl[2] = cpu_to_le16(b43_txhdr_ac_rate_idx(rate));
	b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)&r->plcp, len, rate);
	r->tx_rate = cpu_to_le16(rate);
}

/* Width of a channel or of a VHT rate as word 0 and SIG-A1 take it. */
static unsigned int b43_txhdr_ac_chan_bw(const struct b43_phy_ac *ac)
{
	switch (ac->chanspec & B43_PHY_AC_CHANSPEC_BW_MASK) {
	case B43_PHY_AC_CHANSPEC_BW80:
		return 2;
	case B43_PHY_AC_CHANSPEC_BW40:
		return 1;
	default:
		return 0;
	}
}

static unsigned int b43_txhdr_ac_vht_bw(const struct ieee80211_tx_rate *t)
{
	if (t->flags & IEEE80211_TX_RC_80_MHZ_WIDTH)
		return 2;
	if (t->flags & IEEE80211_TX_RC_40_MHZ_WIDTH)
		return 1;
	return 0;
}

/*
 * One VHT rate on one stream, laid out as the stock driver lays out its own
 * (router-data/vd625/rxtx-ch36.zip, 7.14.43 and its microcode): word 0 with
 * the frame type, the width in 15:14 and 0x0008; word 1 the power offset, and
 * the primary's subband only on a frame narrower than the channel; word 2 the
 * MCS; VHT-SIG-A1/A2 as 802.11 lays them out, group ID 63 and both reserved
 * bits set, the SGI disambiguation and LDPC extra-symbol bits left clear, as
 * the stock driver leaves them on frames of every length; the PHY rate in
 * 500 kb/s; 0x0300 in the fallback bandwidth field, as on every block it
 * sends. The stock frames are all LDPC: a BCC frame differs only in SIG-A2
 * here, and that is not in any capture. Nor is a 40 MHz frame on an 80 MHz
 * channel, whose subband field carries the primary as at 20.
 */
static void b43_txhdr_ac_vht(const struct b43_phy_ac *ac,
			     struct b43_txhdr_ac_rate *r,
			     const struct ieee80211_tx_info *info, u16 cores)
{
	static const u8 rate_bw[] = {
		RATE_INFO_BW_20, RATE_INFO_BW_40, RATE_INFO_BW_80,
	};
	const struct ieee80211_tx_rate *t = &info->control.rates[0];
	unsigned int bw = b43_txhdr_ac_vht_bw(t);
	u8 mcs = ieee80211_rate_get_vht_mcs(t);
	bool sgi = t->flags & IEEE80211_TX_RC_SHORT_GI;
	struct rate_info ri = {
		.flags = RATE_INFO_FLAGS_VHT_MCS |
			 (sgi ? RATE_INFO_FLAGS_SHORT_GI : 0),
		.mcs = mcs,
		.nss = 1,
		.bw = rate_bw[bw],
	};
	u16 phy1 = ac->rate_po_vht[bw][mcs] & B43_TXH_AC_PHY1_TXPWR_OFFSET;
	u32 a1, a2;

	r->phy_ctl[0] = cpu_to_le16(B43_TXH_PHY_ENC_VHT |
				    B43_TXH_AC_PHY0_NON_SOUNDING |
				    B43_TXH_AC_PHY0_VHT_0008 |
				    cores << B43_TXH_AC_PHY0_CORES_SHIFT |
				    bw << B43_TXH_AC_PHY0_BW_SHIFT);
	if (bw < b43_txhdr_ac_chan_bw(ac))
		phy1 |= (ac->chanspec & B43_PHY_AC_CHANSPEC_SB_MASK) >>
			B43_PHY_AC_CHANSPEC_SB_SHIFT;
	r->phy_ctl[1] = cpu_to_le16(phy1);
	r->phy_ctl[2] = cpu_to_le16(mcs & B43_TXH_AC_PHY2_VHT_MCS);

	a1 = bw | BIT(2) | (63 << 4) | BIT(23);
	a2 = (sgi ? BIT(0) : 0) |
	     (info->flags & IEEE80211_TX_CTL_LDPC ? BIT(2) : 0) |
	     (mcs << 4) | BIT(9);
	r->plcp.raw[0] = a1;
	r->plcp.raw[1] = a1 >> 8;
	r->plcp.raw[2] = a1 >> 16;
	r->plcp.raw[3] = a2;
	r->plcp.raw[4] = a2 >> 8;
	r->plcp.raw[5] = a2 >> 16;
	r->fbw_info = cpu_to_le16(0x0300);
	r->tx_rate = cpu_to_le16(cfg80211_calculate_bitrate(&ri) / 5);
}

/*
 * The AC microcode's descriptor with its first rate block only: legacy, or
 * a VHT MCS on one stream. Hardware encryption is off on B43_FW_HDR_AC. The
 * frame goes out on the TX cores the PHY keeps for the beacon.
 */
static int b43_generate_txhdr_ac(struct b43_wldev *dev, u8 *_txhdr,
				 struct sk_buff *skb,
				 struct ieee80211_tx_info *info,
				 const struct b43_txhdr_ctl *c, u16 cookie)
{
	struct b43_txhdr_ac *txhdr = (struct b43_txhdr_ac *)_txhdr;
	struct b43_txhdr_ac_rate *r = &txhdr->rate[0];
	const struct ieee80211_hdr *wlhdr =
	    (const struct ieee80211_hdr *)skb->data;
	const struct b43_phy_ac *ac = dev->phy.ac;
	unsigned int len = skb->len + FCS_LEN;
	u16 cores = ac->tx_cores ? : 0x0001;
	u16 mac_lo = 0, rts = B43_TXH_AC_RTS_LAST_RATE;

	BUILD_BUG_ON(sizeof(struct b43_txhdr_ac) != 128);

	if (WARN_ON_ONCE(info->control.hw_key))
		return -EOPNOTSUPP;

	memset(txhdr, 0, sizeof(*txhdr));
	txhdr->toe[0] = 0x02;
	txhdr->toe[2] = 0x02;

	if (c->ack)
		mac_lo |= B43_TXH_AC_MAC_IACK;
	if (c->hwseq)
		mac_lo |= B43_TXH_AC_MAC_ASEQ;
	if (c->first_frag)
		mac_lo |= B43_TXH_AC_MAC_STMSDU;
	if (c->long_frame)
		mac_lo |= B43_TXH_AC_MAC_LFRM;

	txhdr->mac_ctl_lo = cpu_to_le16(mac_lo);
	txhdr->mac_ctl_hi = cpu_to_le16(B43_TXH_AC_MAC_FIX_RATE);
	txhdr->chanspec = cpu_to_le16(ac->chanspec);
	txhdr->iv_offset = ieee80211_hdrlen(wlhdr->frame_control);
	txhdr->frame_len = cpu_to_le16(len);
	txhdr->cookie = cpu_to_le16(cookie);
	txhdr->seq = wlhdr->seq_ctrl;

	if (c->vht)
		b43_txhdr_ac_vht(ac, r, info, cores);
	else
		b43_txhdr_ac_legacy(ac, r, c, cores, len);

	if (c->rts || c->cts) {
		u8 code;

		rts |= c->cts ? B43_TXH_AC_RTS_USE_CTS : B43_TXH_AC_RTS_USE_RTS;
		if (b43_is_ofdm_rate(c->rts_rate)) {
			rts |= B43_TXH_AC_RTS_FT_OFDM;
			code = b43_plcp_get_ratecode_ofdm(c->rts_rate);
		} else {
			code = b43_plcp_get_ratecode_cck(c->rts_rate);
		}
		rts |= (code & 0x0f) << B43_TXH_AC_RTS_RATE_SHIFT;
	}
	r->rts_cts_ctl = cpu_to_le16(rts);

	return 0;
}

int b43_generate_txhdr(struct b43_wldev *dev,
		       u8 *_txhdr,
		       struct sk_buff *skb_frag,
		       struct ieee80211_tx_info *info,
		       u16 cookie)
{
	const unsigned char *fragment_data = skb_frag->data;
	unsigned int fragment_len = skb_frag->len;
	struct b43_txhdr *txhdr = (struct b43_txhdr *)_txhdr;
	const struct b43_phy *phy = &dev->phy;
	const struct ieee80211_hdr *wlhdr =
	    (const struct ieee80211_hdr *)fragment_data;
	int use_encryption = !!info->control.hw_key;
	__le16 fctl = wlhdr->frame_control;
	struct ieee80211_rate *fbrate;
	u8 rate, rate_fb;
	int rate_ofdm, rate_fb_ofdm;
	unsigned int plcp_fragment_len;
	u32 mac_ctl = 0;
	u16 phy_ctl = 0;
	bool fill_phy_ctl1 = (phy->type == B43_PHYTYPE_LP ||
			      phy->type == B43_PHYTYPE_N ||
			      phy->type == B43_PHYTYPE_HT);
	u8 extra_ft = 0;
	struct b43_txhdr_ctl c;

	b43_txhdr_ctl_get(dev, info, &c);
	if (dev->fw.hdr_format == B43_FW_HDR_AC)
		return b43_generate_txhdr_ac(dev, _txhdr, skb_frag, info,
					     &c, cookie);

	memset(txhdr, 0, sizeof(*txhdr));

	rate = c.rate;
	rate_ofdm = b43_is_ofdm_rate(rate);
	fbrate = c.fbrate;
	rate_fb = c.rate_fb;
	rate_fb_ofdm = b43_is_ofdm_rate(rate_fb);

	if (rate_ofdm)
		txhdr->phy_rate = b43_plcp_get_ratecode_ofdm(rate);
	else
		txhdr->phy_rate = b43_plcp_get_ratecode_cck(rate);
	txhdr->mac_frame_ctl = wlhdr->frame_control;
	memcpy(txhdr->tx_receiver, wlhdr->addr1, ETH_ALEN);

	/* Calculate duration for fallback rate */
	if ((rate_fb == rate) ||
	    (wlhdr->duration_id & cpu_to_le16(0x8000)) ||
	    (wlhdr->duration_id == cpu_to_le16(0))) {
		/* If the fallback rate equals the normal rate or the
		 * dur_id field contains an AID, CFP magic or 0,
		 * use the original dur_id field. */
		txhdr->dur_fb = wlhdr->duration_id;
	} else {
		txhdr->dur_fb = ieee80211_generic_frame_duration(
			dev->wl->hw, info->control.vif, info->band,
			fragment_len, fbrate);
	}

	plcp_fragment_len = fragment_len + FCS_LEN;
	if (use_encryption) {
		u8 key_idx = info->control.hw_key->hw_key_idx;
		struct b43_key *key;
		int wlhdr_len;
		size_t iv_len;

		B43_WARN_ON(key_idx >= ARRAY_SIZE(dev->key));
		key = &(dev->key[key_idx]);

		if (unlikely(!key->keyconf)) {
			/* This key is invalid. This might only happen
			 * in a short timeframe after machine resume before
			 * we were able to reconfigure keys.
			 * Drop this packet completely. Do not transmit it
			 * unencrypted to avoid leaking information. */
			return -ENOKEY;
		}

		/* Hardware appends ICV. */
		plcp_fragment_len += info->control.hw_key->icv_len;

		key_idx = b43_kidx_to_fw(dev, key_idx);
		mac_ctl |= (key_idx << B43_TXH_MAC_KEYIDX_SHIFT) &
			   B43_TXH_MAC_KEYIDX;
		mac_ctl |= (key->algorithm << B43_TXH_MAC_KEYALG_SHIFT) &
			   B43_TXH_MAC_KEYALG;
		wlhdr_len = ieee80211_hdrlen(fctl);
		if (key->algorithm == B43_SEC_ALGO_TKIP) {
			u16 phase1key[5];
			int i;
			/* we give the phase1key and iv16 here, the key is stored in
			 * shm. With that the hardware can do phase 2 and encryption.
			 */
			ieee80211_get_tkip_p1k(info->control.hw_key, skb_frag, phase1key);
			/* phase1key is in host endian. Copy to little-endian txhdr->iv. */
			for (i = 0; i < 5; i++) {
				txhdr->iv[i * 2 + 0] = phase1key[i];
				txhdr->iv[i * 2 + 1] = phase1key[i] >> 8;
			}
			/* iv16 */
			memcpy(txhdr->iv + 10, ((u8 *) wlhdr) + wlhdr_len, 3);
		} else {
			iv_len = min_t(size_t, info->control.hw_key->iv_len,
				     ARRAY_SIZE(txhdr->iv));
			memcpy(txhdr->iv, ((u8 *) wlhdr) + wlhdr_len, iv_len);
		}
	}
	switch (dev->fw.hdr_format) {
	case B43_FW_HDR_AC:
	case B43_FW_HDR_598:
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(&txhdr->format_598.plcp),
				      plcp_fragment_len, rate);
		break;
	case B43_FW_HDR_351:
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(&txhdr->format_351.plcp),
				      plcp_fragment_len, rate);
		break;
	case B43_FW_HDR_410:
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(&txhdr->format_410.plcp),
				      plcp_fragment_len, rate);
		break;
	}
	b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(&txhdr->plcp_fb),
			      plcp_fragment_len, rate_fb);

	/* Extra Frame Types */
	if (rate_fb_ofdm)
		extra_ft |= B43_TXH_EFT_FB_OFDM;
	else
		extra_ft |= B43_TXH_EFT_FB_CCK;

	/* Set channel radio code. Note that the micrcode ORs 0x100 to
	 * this value before comparing it to the value in SHM, if this
	 * is a 5Ghz packet.
	 */
	txhdr->chan_radio_code = phy->channel;

	/* PHY TX Control word */
	if (rate_ofdm)
		phy_ctl |= B43_TXH_PHY_ENC_OFDM;
	else
		phy_ctl |= B43_TXH_PHY_ENC_CCK;
	if (c.short_preamble)
		phy_ctl |= B43_TXH_PHY_SHORTPRMBL;

	switch (b43_ieee80211_antenna_sanitize(dev, 0)) {
	case 0: /* Default */
		phy_ctl |= B43_TXH_PHY_ANT01AUTO;
		break;
	case 1: /* Antenna 0 */
		phy_ctl |= B43_TXH_PHY_ANT0;
		break;
	case 2: /* Antenna 1 */
		phy_ctl |= B43_TXH_PHY_ANT1;
		break;
	case 3: /* Antenna 2 */
		phy_ctl |= B43_TXH_PHY_ANT2;
		break;
	case 4: /* Antenna 3 */
		phy_ctl |= B43_TXH_PHY_ANT3;
		break;
	default:
		B43_WARN_ON(1);
	}

	/* MAC control */
	if (c.ack)
		mac_ctl |= B43_TXH_MAC_ACK;
	if (c.hwseq)
		mac_ctl |= B43_TXH_MAC_HWSEQ;
	if (c.first_frag)
		mac_ctl |= B43_TXH_MAC_STMSDU;
	if (!phy->gmode)
		mac_ctl |= B43_TXH_MAC_5GHZ;
	if (c.long_frame)
		mac_ctl |= B43_TXH_MAC_LONGFRAME;

	/* Generate the RTS or CTS-to-self frame */
	if (c.rts || c.cts) {
		unsigned int len;
		struct ieee80211_hdr *hdr;
		int rts_rate, rts_rate_fb;
		int rts_rate_ofdm, rts_rate_fb_ofdm;
		struct b43_plcp_hdr6 *plcp;

		rts_rate = c.rts_rate;
		rts_rate_ofdm = b43_is_ofdm_rate(rts_rate);
		rts_rate_fb = b43_calc_fallback_rate(rts_rate, phy->gmode);
		rts_rate_fb_ofdm = b43_is_ofdm_rate(rts_rate_fb);

		if (c.cts) {
			struct ieee80211_cts *cts;

			switch (dev->fw.hdr_format) {
			case B43_FW_HDR_AC:
			case B43_FW_HDR_598:
				cts = (struct ieee80211_cts *)
					(txhdr->format_598.rts_frame);
				break;
			case B43_FW_HDR_351:
				cts = (struct ieee80211_cts *)
					(txhdr->format_351.rts_frame);
				break;
			case B43_FW_HDR_410:
				cts = (struct ieee80211_cts *)
					(txhdr->format_410.rts_frame);
				break;
			}
			ieee80211_ctstoself_get(dev->wl->hw, info->control.vif,
						fragment_data, fragment_len,
						info, cts);
			mac_ctl |= B43_TXH_MAC_SENDCTS;
			len = sizeof(struct ieee80211_cts);
		} else {
			struct ieee80211_rts *rts;

			switch (dev->fw.hdr_format) {
			case B43_FW_HDR_AC:
			case B43_FW_HDR_598:
				rts = (struct ieee80211_rts *)
					(txhdr->format_598.rts_frame);
				break;
			case B43_FW_HDR_351:
				rts = (struct ieee80211_rts *)
					(txhdr->format_351.rts_frame);
				break;
			case B43_FW_HDR_410:
				rts = (struct ieee80211_rts *)
					(txhdr->format_410.rts_frame);
				break;
			}
			ieee80211_rts_get(dev->wl->hw, info->control.vif,
					  fragment_data, fragment_len,
					  info, rts);
			mac_ctl |= B43_TXH_MAC_SENDRTS;
			len = sizeof(struct ieee80211_rts);
		}
		len += FCS_LEN;

		/* Generate the PLCP headers for the RTS/CTS frame */
		switch (dev->fw.hdr_format) {
		case B43_FW_HDR_AC:
		case B43_FW_HDR_598:
			plcp = &txhdr->format_598.rts_plcp;
			break;
		case B43_FW_HDR_351:
			plcp = &txhdr->format_351.rts_plcp;
			break;
		case B43_FW_HDR_410:
			plcp = &txhdr->format_410.rts_plcp;
			break;
		}
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)plcp,
				      len, rts_rate);
		plcp = &txhdr->rts_plcp_fb;
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)plcp,
				      len, rts_rate_fb);

		switch (dev->fw.hdr_format) {
		case B43_FW_HDR_AC:
		case B43_FW_HDR_598:
			hdr = (struct ieee80211_hdr *)
				(&txhdr->format_598.rts_frame);
			break;
		case B43_FW_HDR_351:
			hdr = (struct ieee80211_hdr *)
				(&txhdr->format_351.rts_frame);
			break;
		case B43_FW_HDR_410:
			hdr = (struct ieee80211_hdr *)
				(&txhdr->format_410.rts_frame);
			break;
		}
		txhdr->rts_dur_fb = hdr->duration_id;

		if (rts_rate_ofdm) {
			extra_ft |= B43_TXH_EFT_RTS_OFDM;
			txhdr->phy_rate_rts =
			    b43_plcp_get_ratecode_ofdm(rts_rate);
		} else {
			extra_ft |= B43_TXH_EFT_RTS_CCK;
			txhdr->phy_rate_rts =
			    b43_plcp_get_ratecode_cck(rts_rate);
		}
		if (rts_rate_fb_ofdm)
			extra_ft |= B43_TXH_EFT_RTSFB_OFDM;
		else
			extra_ft |= B43_TXH_EFT_RTSFB_CCK;

		if ((info->control.rates[0].flags &
		     IEEE80211_TX_RC_USE_RTS_CTS) && fill_phy_ctl1) {
			txhdr->phy_ctl1_rts = cpu_to_le16(
				b43_generate_tx_phy_ctl1(dev, rts_rate));
			txhdr->phy_ctl1_rts_fb = cpu_to_le16(
				b43_generate_tx_phy_ctl1(dev, rts_rate_fb));
		}
	}

	/* Magic cookie */
	switch (dev->fw.hdr_format) {
	case B43_FW_HDR_AC:
	case B43_FW_HDR_598:
		txhdr->format_598.cookie = cpu_to_le16(cookie);
		break;
	case B43_FW_HDR_351:
		txhdr->format_351.cookie = cpu_to_le16(cookie);
		break;
	case B43_FW_HDR_410:
		txhdr->format_410.cookie = cpu_to_le16(cookie);
		break;
	}

	if (fill_phy_ctl1) {
		txhdr->phy_ctl1 =
			cpu_to_le16(b43_generate_tx_phy_ctl1(dev, rate));
		txhdr->phy_ctl1_fb =
			cpu_to_le16(b43_generate_tx_phy_ctl1(dev, rate_fb));
	}

	/* Apply the bitfields */
	txhdr->mac_ctl = cpu_to_le32(mac_ctl);
	txhdr->phy_ctl = cpu_to_le16(phy_ctl);
	txhdr->extra_ft = extra_ft;

	return 0;
}

static s8 b43_rssi_postprocess(struct b43_wldev *dev,
			       u8 in_rssi, int ofdm,
			       int adjust_2053, int adjust_2050)
{
	struct b43_phy *phy = &dev->phy;
	struct b43_phy_g *gphy = phy->g;
	s32 tmp;

	switch (phy->radio_ver) {
	case 0x2050:
		if (ofdm) {
			tmp = in_rssi;
			if (tmp > 127)
				tmp -= 256;
			tmp *= 73;
			tmp /= 64;
			if (adjust_2050)
				tmp += 25;
			else
				tmp -= 3;
		} else {
			if (dev->dev->bus_sprom->
			    boardflags_lo & B43_BFL_RSSI) {
				if (in_rssi > 63)
					in_rssi = 63;
				B43_WARN_ON(phy->type != B43_PHYTYPE_G);
				tmp = gphy->nrssi_lt[in_rssi];
				tmp = 31 - tmp;
				tmp *= -131;
				tmp /= 128;
				tmp -= 57;
			} else {
				tmp = in_rssi;
				tmp = 31 - tmp;
				tmp *= -149;
				tmp /= 128;
				tmp -= 68;
			}
			if (phy->type == B43_PHYTYPE_G && adjust_2050)
				tmp += 25;
		}
		break;
	case 0x2060:
		if (in_rssi > 127)
			tmp = in_rssi - 256;
		else
			tmp = in_rssi;
		break;
	default:
		tmp = in_rssi;
		tmp -= 11;
		tmp *= 103;
		tmp /= 64;
		if (adjust_2053)
			tmp -= 109;
		else
			tmp -= 83;
	}

	return (s8) tmp;
}

/*
 * The rate of a frame from the AC microcode, by the frame type of PHY RX
 * status 0, which here counts HT and VHT besides CCK and OFDM (Broadcom's
 * FT_HT and FT_VHT). For HT and VHT the six bytes in front of the frame are
 * the SIG fields, laid out as 802.11 defines them: HT-SIG1/2 (MCS and the
 * 40 MHz bit in byte 0, coding, STBC and short guard interval in byte 3,
 * as brcmsmac's brcms_c_compute_rspec() reads them) and VHT-SIG-A1/A2
 * (bandwidth, STBC and the streams in A1, short guard interval, coding and
 * MCS in A2). Returns the rate index for mac80211, or -1.
 */
static int b43_rx_rate_ac(struct b43_plcp_hdr6 *plcp, u16 phystat0,
			  bool rx_5ghz, struct ieee80211_rx_status *status)
{
	const u8 *p = plcp->raw;
	u32 a1, a2;
	u8 nsts, mcs;

	switch (phystat0 & B43_RX_PHYST0_FTYPE) {
	case B43_RX_PHYST0_HT:
		status->encoding = RX_ENC_HT;
		if (p[0] & 0x80)
			status->bw = RATE_INFO_BW_40;
		if (p[3] & 0x80)
			status->enc_flags |= RX_ENC_FLAG_SHORT_GI;
		if (p[3] & 0x40)
			status->enc_flags |= RX_ENC_FLAG_LDPC;
		status->enc_flags |= ((p[3] >> 4) & 0x3) << RX_ENC_FLAG_STBC_SHIFT;
		return p[0] & 0x7f;
	case B43_RX_PHYST0_VHT:
		a1 = p[0] | (p[1] << 8) | (p[2] << 16);
		a2 = p[3] | (p[4] << 8) | (p[5] << 16);
		mcs = (a2 >> 4) & 0xf;
		if (mcs > 9)
			return -1;
		status->encoding = RX_ENC_VHT;
		switch (a1 & 0x3) {
		case 1:
			status->bw = RATE_INFO_BW_40;
			break;
		case 2:
			status->bw = RATE_INFO_BW_80;
			break;
		case 3:
			status->bw = RATE_INFO_BW_160;
			break;
		}
		nsts = ((a1 >> 10) & 0x7) + 1;
		if (a1 & 0x8) {
			status->enc_flags |= 1 << RX_ENC_FLAG_STBC_SHIFT;
			nsts /= 2;
		}
		status->nss = max_t(u8, nsts, 1);
		if (a2 & 0x1)
			status->enc_flags |= RX_ENC_FLAG_SHORT_GI;
		if (a2 & 0x4)
			status->enc_flags |= RX_ENC_FLAG_LDPC;
		return mcs;
	case B43_RX_PHYST0_OFDM:
		return b43_plcp_get_bitrate_idx_ofdm(plcp, rx_5ghz);
	default:
		return b43_plcp_get_bitrate_idx_cck(plcp);
	}
}

void b43_rx(struct b43_wldev *dev, struct sk_buff *skb, const void *_rxhdr)
{
	struct ieee80211_rx_status status;
	struct b43_plcp_hdr6 *plcp;
	struct ieee80211_hdr *wlhdr;
	const struct b43_rxhdr_fw4 *rxhdr = _rxhdr;
	__le16 fctl;
	u16 phystat0, phystat3;
	u16 chanstat, mactime;
	u32 macstat;
	u16 chanid;
	u8 phytype;
	bool rx_5ghz;
	int padding, rate_idx;

	memset(&status, 0, sizeof(status));

	/* Get metadata about the frame from the header. */
	phystat0 = le16_to_cpu(rxhdr->phy_status0);
	phystat3 = le16_to_cpu(rxhdr->phy_status3);
	switch (dev->fw.hdr_format) {
	case B43_FW_HDR_AC:
	case B43_FW_HDR_598:
		macstat = le32_to_cpu(rxhdr->format_598.mac_status);
		mactime = le16_to_cpu(rxhdr->format_598.mac_time);
		chanstat = le16_to_cpu(rxhdr->format_598.channel);
		break;
	case B43_FW_HDR_410:
	case B43_FW_HDR_351:
		macstat = le32_to_cpu(rxhdr->format_351.mac_status);
		mactime = le16_to_cpu(rxhdr->format_351.mac_time);
		chanstat = le16_to_cpu(rxhdr->format_351.channel);
		break;
	}

	/* The channel field of the older microcode is (cookie << 3) | phytype,
	 * and three bits cannot carry B43_PHYTYPE_AC. What the AC microcode
	 * puts there is not known, so on AC the PHY type comes from the device
	 * and the band from the channel the radio is tuned to. */
	if (dev->phy.type == B43_PHYTYPE_AC) {
		phytype = B43_PHYTYPE_AC;
		rx_5ghz = b43_current_band(dev->wl) == NL80211_BAND_5GHZ;
	} else {
		phytype = chanstat & B43_RX_CHAN_PHYTYPE;
		rx_5ghz = !!(chanstat & B43_RX_CHAN_5GHZ);
	}

	if (unlikely(macstat & B43_RX_MAC_FCSERR)) {
		dev->wl->ieee_stats.dot11FCSErrorCount++;
		status.flag |= RX_FLAG_FAILED_FCS_CRC;
	}
	if (unlikely(phystat0 & (B43_RX_PHYST0_PLCPHCF | B43_RX_PHYST0_PLCPFV)))
		status.flag |= RX_FLAG_FAILED_PLCP_CRC;
	if (phystat0 & B43_RX_PHYST0_SHORTPRMBL)
		status.enc_flags |= RX_ENC_FLAG_SHORTPRE;
	if (macstat & B43_RX_MAC_DECERR) {
		/* Decryption with the given key failed.
		 * Drop the packet. We also won't be able to decrypt it with
		 * the key in software. */
		goto drop;
	}

	/* Skip PLCP and padding */
	padding = (macstat & B43_RX_MAC_PADDING) ? 2 : 0;
	if (unlikely(skb->len < (sizeof(struct b43_plcp_hdr6) + padding))) {
		b43dbg(dev->wl, "RX: Packet size underrun (1)\n");
		goto drop;
	}
	plcp = (struct b43_plcp_hdr6 *)(skb->data + padding);
	skb_pull(skb, sizeof(struct b43_plcp_hdr6) + padding);
	/* The skb contains the Wireless Header + payload data now */
	if (unlikely(skb->len < (2 + 2 + 6 /*minimum hdr */  + FCS_LEN))) {
		b43dbg(dev->wl, "RX: Packet size underrun (2)\n");
		goto drop;
	}
	wlhdr = (struct ieee80211_hdr *)(skb->data);
	fctl = wlhdr->frame_control;

	if (macstat & B43_RX_MAC_DEC) {
		unsigned int keyidx;
		int wlhdr_len;

		keyidx = ((macstat & B43_RX_MAC_KEYIDX)
			  >> B43_RX_MAC_KEYIDX_SHIFT);
		/* We must adjust the key index here. We want the "physical"
		 * key index, but the ucode passed it slightly different.
		 */
		keyidx = b43_kidx_to_raw(dev, keyidx);
		B43_WARN_ON(keyidx >= ARRAY_SIZE(dev->key));

		if (dev->key[keyidx].algorithm != B43_SEC_ALGO_NONE) {
			wlhdr_len = ieee80211_hdrlen(fctl);
			if (unlikely(skb->len < (wlhdr_len + 3))) {
				b43dbg(dev->wl,
				       "RX: Packet size underrun (3)\n");
				goto drop;
			}
			status.flag |= RX_FLAG_DECRYPTED;
		}
	}

	/* Link quality statistics */
	switch (phytype) {
	case B43_PHYTYPE_AC: {
		/*
		 * The power of each core in dBm, bytes 9 and 10 of the header,
		 * -128 on a core that did not receive (gonsolo's port).
		 */
		const s8 *pwr = (const s8 *)_rxhdr + 9;

		if (pwr[0] == -128 && pwr[1] == -128)
			status.flag |= RX_FLAG_NO_SIGNAL_VAL;
		else if (pwr[0] == -128)
			status.signal = pwr[1];
		else if (pwr[1] == -128)
			status.signal = pwr[0];
		else
			status.signal = max(pwr[0], pwr[1]);
		break;
	}
	case B43_PHYTYPE_HT:
		/* TODO: is max the right choice? */
		status.signal = max_t(__s8,
			max(rxhdr->phy_ht_power0, rxhdr->phy_ht_power1),
			rxhdr->phy_ht_power2);
		break;
	case B43_PHYTYPE_N:
		/* Broadcom has code for min and avg, but always uses max */
		if (rxhdr->power0 == 16 || rxhdr->power0 == 32)
			status.signal = max(rxhdr->power1, rxhdr->power2);
		else
			status.signal = max(rxhdr->power0, rxhdr->power1);
		break;
	case B43_PHYTYPE_B:
	case B43_PHYTYPE_G:
	case B43_PHYTYPE_LP:
		status.signal = b43_rssi_postprocess(dev, rxhdr->jssi,
						  (phystat0 & B43_RX_PHYST0_OFDM),
						  (phystat0 & B43_RX_PHYST0_GAINCTL),
						  (phystat3 & B43_RX_PHYST3_TRSTATE));
		break;
	}

	if (dev->fw.hdr_format == B43_FW_HDR_AC)
		rate_idx = b43_rx_rate_ac(plcp, phystat0, rx_5ghz, &status);
	else if (phystat0 & B43_RX_PHYST0_OFDM)
		rate_idx = b43_plcp_get_bitrate_idx_ofdm(plcp, rx_5ghz);
	else
		rate_idx = b43_plcp_get_bitrate_idx_cck(plcp);
	if (unlikely(rate_idx == -1)) {
		/* PLCP seems to be corrupted.
		 * Drop the frame, if we are not interested in corrupted frames. */
		if (!(dev->wl->filter_flags & FIF_PLCPFAIL))
			goto drop;
	}
	status.rate_idx = rate_idx;
	status.antenna = !!(phystat0 & B43_RX_PHYST0_ANT);

	/*
	 * All frames on monitor interfaces and beacons always need a full
	 * 64-bit timestamp. Monitor interfaces need it for diagnostic
	 * purposes and beacons for IBSS merging.
	 * This code assumes we get to process the packet within 16 bits
	 * of timestamp, i.e. about 65 milliseconds after the PHY received
	 * the first symbol.
	 */
	if (ieee80211_is_beacon(fctl) || dev->wl->radiotap_enabled) {
		u16 low_mactime_now;

		b43_tsf_read(dev, &status.mactime);
		low_mactime_now = status.mactime;
		status.mactime = status.mactime & ~0xFFFFULL;
		status.mactime += mactime;
		if (low_mactime_now <= mactime)
			status.mactime -= 0x10000;
		status.flag |= RX_FLAG_MACTIME_START;
	}

	chanid = (chanstat & B43_RX_CHAN_ID) >> B43_RX_CHAN_ID_SHIFT;
	switch (phytype) {
	case B43_PHYTYPE_G:
		status.band = NL80211_BAND_2GHZ;
		/* Somewhere between 478.104 and 508.1084 firmware for G-PHY
		 * has been modified to be compatible with N-PHY and others.
		 */
		if (dev->fw.rev >= 508)
			status.freq = ieee80211_channel_to_frequency(chanid, status.band);
		else
			status.freq = chanid + 2400;
		break;
	case B43_PHYTYPE_N:
	case B43_PHYTYPE_LP:
	case B43_PHYTYPE_HT:
		/* chanid is the SHM channel cookie. Which is the plain
		 * channel number in b43. */
		if (rx_5ghz)
			status.band = NL80211_BAND_5GHZ;
		else
			status.band = NL80211_BAND_2GHZ;
		status.freq =
			ieee80211_channel_to_frequency(chanid, status.band);
		break;
	case B43_PHYTYPE_AC:
		status.band = b43_current_band(dev->wl);
		status.freq = dev->wl->hw->conf.chandef.chan->center_freq;
		break;
	default:
		B43_WARN_ON(1);
		goto drop;
	}

	memcpy(IEEE80211_SKB_RXCB(skb), &status, sizeof(status));
	ieee80211_rx_ni(dev->wl->hw, skb);

#if B43_DEBUG
	dev->rx_count++;
#endif
	return;
drop:
	dev_kfree_skb_any(skb);
}

/*
 * A TX status whose cookie matches no queue: on the AC microcode, the words
 * it came in, for what the status says when it is not about one of b43's
 * frames.
 */
void b43_txstatus_dump(struct b43_wldev *dev,
		       const struct b43_txstatus *status)
{
	const u32 *w = status->raw;

	if (dev->fw.hdr_format != B43_FW_HDR_AC)
		return;
	b43warn(dev->wl, "TX status %08x %08x %08x %08x / "
		"%08x %08x %08x %08x\n",
		w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
}

void b43_handle_txstatus(struct b43_wldev *dev,
			 const struct b43_txstatus *status)
{
	b43_debugfs_log_txstat(dev, status);

	if (status->intermediate)
		return;
	if (status->for_ampdu)
		return;
	if (!status->acked)
		dev->wl->ieee_stats.dot11ACKFailureCount++;
	if (status->rts_count) {
		if (status->rts_count == 0xF)	//FIXME
			dev->wl->ieee_stats.dot11RTSFailureCount++;
		else
			dev->wl->ieee_stats.dot11RTSSuccessCount++;
	}

	if (b43_using_pio_transfers(dev))
		b43_pio_handle_txstatus(dev, status);
	else
		b43_dma_handle_txstatus(dev, status);

	b43_phy_txpower_check(dev, 0);
}

/* Fill out the mac80211 TXstatus report based on the b43-specific
 * txstatus report data. This returns a boolean whether the frame was
 * successfully transmitted. */
bool b43_fill_txstatus_report(struct b43_wldev *dev,
			      struct ieee80211_tx_info *report,
			      const struct b43_txstatus *status)
{
	bool frame_success = true;
	int retry_limit;

	/* preserve the confiured retry limit before clearing the status
	 * The xmit function has overwritten the rc's value with the actual
	 * retry limit done by the hardware */
	retry_limit = report->status.rates[0].count;
	ieee80211_tx_info_clear_status(report);

	if (status->acked) {
		/* The frame was ACKed. */
		report->flags |= IEEE80211_TX_STAT_ACK;
	} else {
		/* The frame was not ACKed... */
		if (!(report->flags & IEEE80211_TX_CTL_NO_ACK)) {
			/* ...but we expected an ACK. */
			frame_success = false;
		}
	}
	if (status->frame_count == 0) {
		/* The frame was not transmitted at all. */
		report->status.rates[0].count = 0;
	} else if (status->rts_count > dev->wl->hw->conf.short_frame_max_tx_count) {
		/*
		 * If the short retries (RTS, not data frame) have exceeded
		 * the limit, the hw will not have tried the selected rate,
		 * but will have used the fallback rate instead.
		 * Don't let the rate control count attempts for the selected
		 * rate in this case, otherwise the statistics will be off.
		 */
		report->status.rates[0].count = 0;
		report->status.rates[1].count = status->frame_count;
	} else {
		if (status->frame_count > retry_limit) {
			report->status.rates[0].count = retry_limit;
			report->status.rates[1].count = status->frame_count -
					retry_limit;

		} else {
			report->status.rates[0].count = status->frame_count;
			report->status.rates[1].idx = -1;
		}
	}

	return frame_success;
}

/* Stop any TX operation on the device (suspend the hardware queues) */
void b43_tx_suspend(struct b43_wldev *dev)
{
	if (b43_using_pio_transfers(dev))
		b43_pio_tx_suspend(dev);
	else
		b43_dma_tx_suspend(dev);
}

/* Resume any TX operation on the device (resume the hardware queues) */
void b43_tx_resume(struct b43_wldev *dev)
{
	if (b43_using_pio_transfers(dev))
		b43_pio_tx_resume(dev);
	else
		b43_dma_tx_resume(dev);
}
