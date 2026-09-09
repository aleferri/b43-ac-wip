/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Profili di board condivisi fra le due suite.
 *
 * Stavano in unit/main.c e l'integrazione non li aveva affatto: girava con la
 * SROM azzerata, quindi con femctrl=0 -- che il driver segnala -- rxchain=0,
 * coremask=0, subband5gver=0 e le tabelle pa5ga/maxp5ga tutte a zero. Ogni
 * misura fatta la' sopra era su una board che non esiste.
 *
 * Sono la stessa board, quindi il profilo sta in un posto solo. Ogni suite lo
 * monta a modo suo: `unit` ha una struct ssb_sprom di stub, `integration` ha
 * quella vera degli header del kernel, e i nomi dei campi coincidono perche' i
 * campi rev-11 li aggiunge patches/0001 con quei nomi.
 */
#ifndef B43_TEST_BOARD_PROFILE_H_
#define B43_TEST_BOARD_PROFILE_H_

/* Board profile. Extend when adding more targets (DSL-3580L is identical
 * to D6220 chip-wise; agcombo needs num_cores=3, coremask=0x7). */
struct board_profile {
	const char *name;
	u16 chip_id;
	u8  radio_rev;
	u16 radio_ver;
	u8  phy_rev;
	u8  num_cores;
	u8  coremask;
	u8  rxchain;
	u8  subband5gver;
	/* Word raw del blocco FEM/PA, come lette dai dump SROM. */
	u16 fem_cfg1;
	u16 fem_cfg2;
	u16 tssifloor5g[4];
	/* pa5ga per-core (3 core), 12 u16 = 4 gruppi (5g band) × 3 (a1,b0,b1).
	 * Dal file NVRAM del router, keys pa5ga0/pa5ga1/pa5ga2. Se tutti 0,
	 * mount_board lascia pa5ga = 0 (il caller cade sui pwrdet_def).
	 */
	u16 pa5ga[3][12];
	/* maxp5ga per-core (3 core), 4 sub-band u8. NVRAM keys maxp5ga{0,1,2}.
	 * Drives the per-core max TX index (maxp5ga[grp] - margin). */
	u8 maxp5ga[3][4];
	/* mcsbw{20,40}5g{l,m,h}po, NVRAM. Index 0 = 5gl, 1 = 5gm, 2 = 5gh. */
	u32 mcsbw5g_po[3][3];	/* [sotto-banda][bw20, bw40, bw80] */
	/* rxgains_5gl per-core (3 core). NVRAM keys rxgains5gelnagaina{0,1,2}
	 * e rxgains5gtrisoa{0,1,2}. Usati per computare hdr = (elnagain+3)<<1
	 * e gainctx = ((triso+4)<<1)+2 nel body Phase 3 di noise-shaping. */
	u8 rxgains_5gl_elnagain[3];
	u8 rxgains_5gl_triso[3];
	/* R2069_RCCAL_E/F (radio 0x0414/0x0415) read by rccal in op_init to
	 * derive lpf_cap = ((F-E)*193)>>8. Per-board because it is an analog
	 * measurement, not a constant. */
	u16 rccal_e, rccal_f;
	/* R2069_RCCAL_G (radio 0x0416), read post-apply: dacbuf_cap =
	 * (rccal_g & 0x03e0) >> 5. Per-board analog measurement. */
	u16 rccal_g;
	/* macaddr dell'NVRAM. Lo consuma emit_core_shm_macaddr(), che sta in
	 * piedi per il core: e' dato di board, non una costante. */
	u8 macaddr[6];
	/* Revisione del core 802.11 e MAC_HW_CAP. Li consuma
	 * emit_core_shm_chipinit(): b43 li scrive in shared memory al core
	 * init, il primo da dev->dev->core_rev e il secondo da una lettura
	 * MMIO di B43_MMIO_MAC_HW_CAP che l'harness non modella. */
	u16 core_rev;
	u32 mac_hw_cap;
};

/*
 * D6220 SPROM values from router-data/d6220/wl1_nvram.txt:
 *   subband5gver=0x4
 *   pa5ga0=0xff33,0x175b,0xfd32,0xff23,0x1672,0xfd36,0xff25,0x161d,0xfd4b,0xff2d,0x16c3,0xfd3b
 *   pa5ga1=0xff2e,0x1702,0xfd39,0xff23,0x16ae,0xfd30,0xff44,0x180e,0xfd36,0xff36,0x16b0,0xfd55
 *   pa5ga2=0xff6e,0x15dc,0xfd61,0xff59,0x15be,0xfd49,0xff5d,0x15f7,0xfd4a,0xff3d,0x1560,0xfd33
 * Per ch36 (5180 MHz) con subband5gver=4 → grp=0, a1/b0/b1 = pa5ga0[0..2].
 */
static const struct board_profile PROFILE_D6220 = {
	.name = "d6220", .chip_id = 0x4352, .radio_rev = 4,
	/* macaddr=00:00:00:00:00:03 (wl1_nvram.txt) */
	.macaddr = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	/* WLCOREREV e MACHW_L/H come li scrive il vendor: cold01 #652-#654 */
	.core_rev = 42, .mac_hw_cap = 0x30518c05,
	.radio_ver = 0x2069, .phy_rev = 1,
	.num_cores = 3, .coremask = 0x3, .rxchain = 3,
	.subband5gver = 0x4,
	.fem_cfg1     = 0x30a1,
	.fem_cfg2     = 0x00a1,
	/* word 96..99 = 0xffff, mascherate 0x03ff: campo non programmato. */
	.tssifloor5g  = { 0x3ff, 0x3ff, 0x3ff, 0x3ff },
	.pa5ga = {
		{ 0xff33, 0x175b, 0xfd32, 0xff23, 0x1672, 0xfd36,
		  0xff25, 0x161d, 0xfd4b, 0xff2d, 0x16c3, 0xfd3b },
		{ 0xff2e, 0x1702, 0xfd39, 0xff23, 0x16ae, 0xfd30,
		  0xff44, 0x180e, 0xfd36, 0xff36, 0x16b0, 0xfd55 },
		{ 0xff6e, 0x15dc, 0xfd61, 0xff59, 0x15be, 0xfd49,
		  0xff5d, 0x15f7, 0xfd4a, 0xff3d, 0x1560, 0xfd33 },
	},
	/* rxgains5gelnagaina{0,1,2}=3, rxgains5gtrisoa{0,1,2}=6 (NVRAM d6220). */
	.rxgains_5gl_elnagain = { 3, 3, 3 },
	.rxgains_5gl_triso    = { 6, 6, 6 },
	/* No RETVAL in the d6220 capture (older tracer): the observed TXLPF
	 * write lo=0x50db fixes cap=0xa8, hence F-E=0xdf. Absolute E is taken
	 * as comparator-A 0x0ac7; only the difference feeds the cap. */
	.rccal_e = 0x0ac7, .rccal_f = 0x0ba6,
	/* No RETVAL on d6220: dacbuf_cap 0xe deduced from the observed
	 * 0x0b2e; rccal_g chosen so (g & 0x3e0)>>5 = 0xe. */
	.rccal_g = 0x01c0,
	/* maxp5ga{0,1,2} (NVRAM d6220). */
	.maxp5ga = {
		{ 72, 70, 86, 0 },
		{ 72, 70, 86, 0 },
		{ 76, 76, 76, 76 },
		},
.mcsbw5g_po = {
		/* mcsbw{20,40,80}5g{l,m,h}po di wl1_nvram.txt */
		{ 0x20000000, 0x21000000, 0x32222222 },
		{ 0x11111111, 0x10000000, 0x22222222 },
		{ 0x98764200, 0x98764200, 0xa8764222 },
	},
};

static const struct board_profile PROFILE_AGCOMBO = {
	.name = "agcombo", .chip_id = 0x4360, .radio_rev = 4,
	/* macaddr=00:c0:02:01:07:24 (agcombo_nvram.txt) */
	.macaddr = { 0x00, 0xc0, 0x02, 0x01, 0x07, 0x24 },
	.core_rev = 42, .mac_hw_cap = 0x30518c05,
	/*
	 * Blocco FEM/PA sintetizzato dalla NVRAM agcombo con le maschere
	 * canoniche (SROM11_FEM_CFG1/2): femctrl=6 pdgain=10 tssiposslope=1,
	 * il resto a zero -- gli stessi valori del d6220, quindi stesse word.
	 * Il dump SROM agcombo in repo e' tutto a zero, quindi la sorgente e'
	 * la NVRAM. Senza questo il guard su femctrl scattava e la tabella di
	 * controllo FEM non veniva scritta su questa board.
	 */
	.fem_cfg1 = 0x30a1, .fem_cfg2 = 0x00a1,
	.tssifloor5g = { 0x3ff, 0x3ff, 0x3ff, 0x3ff },
	.radio_ver = 0x2069, .phy_rev = 1,
	.num_cores = 3, .coremask = 0x7, .rxchain = 7,
	/* Same 5gl values as d6220 (NVRAM agcombo). */
	.rxgains_5gl_elnagain = { 3, 3, 3 },
	.rxgains_5gl_triso    = { 6, 6, 6 },
	/* Real E/F from the agcombo rescan RETVALs -> cap=0xae. */
	.rccal_e = 0x0adc, .rccal_f = 0x0bc4,
	.rccal_g = 0x01a8,  /* -> dacbuf_cap 0xd */
	/* maxp5ga{0,1,2} (NVRAM agcombo). */
	.maxp5ga = {
		{ 74, 74, 82, 82 },
		{ 74, 74, 82, 82 },
		{ 74, 74, 82, 82 },
		},
.mcsbw5g_po = {
		/*
		 * mcsbw{20,40,80}5g{l,m,h}po di agcombo_nvram.txt, che li porta
		 * in esadecimale. Su questa board le tre larghezze hanno la
		 * stessa word: 5gl e 5gm a 0x88644220, 5gh a 0xcca88440.
		 */
		{ 0x88644220, 0x88644220, 0x88644220 },
		{ 0x88644220, 0x88644220, 0x88644220 },
		{ 0xcca88440, 0xcca88440, 0xcca88440 },
	},
};

/*
 * DSL-3580L: BCM4352 radio (like the D6220) on a BCM6362 SoC, wl 6.30.102.7.
 * NVRAM from router-data/dsl3580l/wl1_nvram.txt: aa5g/txchain/rxchain=3
 * (coremask 0x3, two active cores like the D6220), subband5gver=0x4,
 * maxp5ga{0,1,2}=76,76,76,76, rxgains 5gl elnagain=3/triso=6, pa5ga per-core
 * below. Same chip as the D6220 but older wl, so it is the version witness.
 */
static const struct board_profile PROFILE_DSL = {
	.name = "dsl", .chip_id = 0x4352, .radio_rev = 4,
	.radio_ver = 0x2069, .phy_rev = 1,
	.num_cores = 3, .coremask = 0x3, .rxchain = 3,
	.subband5gver = 0x4,
	.fem_cfg1     = 0x30a1,
	.fem_cfg2     = 0x00a1,
	/* word 96..99 = 0xffff, mascherate 0x03ff: campo non programmato. */
	.tssifloor5g  = { 0x3ff, 0x3ff, 0x3ff, 0x3ff },
	.pa5ga = {
		{ 0xff4d, 0x1690, 0xfd24, 0xff59, 0x1710, 0xfd28,
		  0xff52, 0x16fd, 0xfd27, 0xff55, 0x1711, 0xfd20 },
		{ 0xff3f, 0x1607, 0xfd1f, 0xff42, 0x1690, 0xfd21,
		  0xff55, 0x1772, 0xfd1e, 0xff5d, 0x178d, 0xfd1e },
		{ 0xff5a, 0x1729, 0xfd25, 0xff62, 0x175c, 0xfd30,
		  0xff48, 0x1720, 0xfd15, 0xff54, 0x1741, 0xfd21 },
	},
	.rxgains_5gl_elnagain = { 3, 3, 3 },
	.rxgains_5gl_triso    = { 6, 6, 6 },
	/* Real E/F from the DSL down-to-bss RETVALs -> cap=0xb6 (lo=0x6cdb). */
	.rccal_e = 0x0b39, .rccal_f = 0x0c2b,
	.rccal_g = 0x0186,  /* -> dacbuf_cap 0xc */
	.maxp5ga = {
		{ 76, 76, 76, 76 },
		{ 76, 76, 76, 76 },
		{ 76, 76, 76, 76 },
		},
.mcsbw5g_po = {
		/*
		 * mcsbw{20,40,80}5g{l,m,h}po di wl1_nvram.txt, che li porta in
		 * decimale. Come sull'agcombo le tre larghezze hanno la stessa
		 * word; 5gm e 5gh coincidono anche fra loro.
		 */
		{ 0xeca86420, 0xeca86420, 0xeca86420 },
		{ 0xcca86420, 0xcca86420, 0xcca86420 },
		{ 0xcca86420, 0xcca86420, 0xcca86420 },
	},
};

/* One-shot mock storage. Lives for the whole run. */
/* Il profilo per nome, come lo passano le due suite (argv o B43_BOARD). */
static inline const struct board_profile *board_profile_lookup(const char *name)
{
	if (name && !strcmp(name, "agcombo"))
		return &PROFILE_AGCOMBO;
	if (name && !strcmp(name, "dsl"))
		return &PROFILE_DSL;
	return &PROFILE_D6220;
}

/*
 * Profilo -> SROM, con le stesse maschere che bcma_sprom_extract_r11() usa
 * (SROM11_FEM_CFG1/2, offset 0x0AA/0x0AC). Il profilo sta al posto di bcma:
 * sull'hardware i per-rate li riempie bcma_sprom_extract_r11() di
 * patches/0001, dai word 176/178/180 e dai due blocchi successivi a passo 8.
 */
static inline void board_profile_to_sprom(const struct board_profile *p,
					  struct ssb_sprom *s)
{
	u16 c1 = p->fem_cfg1, c2 = p->fem_cfg2;
	unsigned int c, b, w;

	memset(s, 0, sizeof(*s));
	s->rxchain = p->rxchain;
	s->subband5gver = p->subband5gver;

	s->tssiposslope2g = c1 & 0x0001;
	s->epagain2g      = (c1 & 0x000e) >> 1;
	s->pdgain2g       = (c1 & 0x01f0) >> 4;
	s->tworangetssi2g = (c1 & 0x0200) >> 9;
	s->papdcap2g      = (c1 & 0x0400) >> 10;
	s->femctrl        = (c1 & 0xf800) >> 11;
	s->tssiposslope5g = c2 & 0x0001;
	s->epagain5g      = (c2 & 0x000e) >> 1;
	s->pdgain5g       = (c2 & 0x01f0) >> 4;
	s->tworangetssi5g = (c2 & 0x0200) >> 9;
	s->papdcap5g      = (c2 & 0x0400) >> 10;
	s->gainctrlsph    = (c2 & 0xf800) >> 11;

	memcpy(s->tssifloor5g, p->tssifloor5g, sizeof(s->tssifloor5g));
	memcpy(s->rxgains_5gl.elnagain, p->rxgains_5gl_elnagain,
	       sizeof(s->rxgains_5gl.elnagain));
	memcpy(s->rxgains_5gl.triso, p->rxgains_5gl_triso,
	       sizeof(s->rxgains_5gl.triso));

	for (c = 0; c < 3; c++) {
		memcpy(s->core_pwr_info[c].pa5ga, p->pa5ga[c],
		       sizeof(s->core_pwr_info[c].pa5ga));
		memcpy(s->core_pwr_info[c].maxp5ga, p->maxp5ga[c],
		       sizeof(s->core_pwr_info[c].maxp5ga));
	}

	for (b = 0; b < 3; b++) {
		u32 *dst[3][3] = {
			{ &s->mcsbw205glpo, &s->mcsbw405glpo, &s->mcsbw805glpo },
			{ &s->mcsbw205gmpo, &s->mcsbw405gmpo, &s->mcsbw805gmpo },
			{ &s->mcsbw205ghpo, &s->mcsbw405ghpo, &s->mcsbw805ghpo },
		};

		for (w = 0; w < 3; w++)
			*dst[b][w] = p->mcsbw5g_po[b][w];
	}
}

#endif /* B43_TEST_BOARD_PROFILE_H_ */
