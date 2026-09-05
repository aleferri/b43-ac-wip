/* SPDX-License-Identifier: GPL-2.0
 * Trace-emitting shim for the userspace test build of the b43 AC-PHY
 * scratch. Every low-level HW accessor referenced by the scratch code
 * is wrapped via -Wl,--wrap=SYM. The wrapper writes one line in the
 * wl-diag format to stdout, then either simulates the HW effect (for
 * writes -- update the in-memory mirror) or returns the mirror value
 * (for reads). No real MMIO happens.
 *
 * Read strategy: scripted. Before invoking the flow under test, main.c
 * (or a per-flow helper) calls b43_test_plan_{phy,radio,mmio}_reads()
 * to register a sequence of return values for each hot address. On
 * each __wrap_*_read of that address the i-th value is returned and i
 * is advanced; past cap, the read returns 0. Addresses without a plan
 * fall back to the write-mirror (returns last-written value, or 0).
 *
 * This lets the test reproduce vendor branches driven by HW state
 * (polls, capability flags, board-strap probes) by scripting exactly
 * what the HW would have returned on each iteration, without any HW
 * simulation logic inside the wrapper itself.
 *
 * Trace format matches the annotated dumps under reverse-output/ and
 * router-data/, minus timestamp and episode number:
 *
 *   cpu1 PHY.WR   addr=0xNNNN val=0xNNNN
 *   cpu1 PHY.RD   addr=0xNNNN val=UNDEFINED
 *   cpu1 PHY.MOD  addr=0xNNNN val=0xNNNN mask=0xNNNN
 *   cpu1 RAD.WR   addr=0xNNNN val=0xNNNN
 *   cpu1 RAD.RD   addr=0xNNNN val=UNDEFINED
 *   cpu1 MMIO.WR  off=0xNNNN  val=0xNNNN
 *   cpu1 MMIO.RD  off=0xNNNN  val=UNDEFINED
 *   cpu1 TBL.WR   id=0xNNNN off=0xNNNN len=N
 *   cpu1 TBL.RD   id=0xNNNN off=0xNNNN len=N
 *
 * Episode / timestamp columns are omitted; a normaliser in test/compare.py
 * strips them from the reference trace before diffing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "b43.h"
#include "phy_ac.h"
#include "test_harness.h"

/* ============ write mirror (fallback for unscripted reads) ============ */

#define MIRROR_PHY_SZ    0x2000
#define MIRROR_RADIO_SZ  0x1000
#define MIRROR_MMIO_SZ   0x1000

static u16 mirror_phy[MIRROR_PHY_SZ];
static u16 mirror_radio[MIRROR_RADIO_SZ];
static u16 mirror_mmio[MIRROR_MMIO_SZ];
static u32 pll_vals[8];

/* ============ read plans ============
 *
 * A read plan for one address is a fixed-length array of return values
 * and a cursor. The i-th read of that address returns results[i] and
 * bumps i; when i >= cap the read returns 0. `results` is owned by the
 * caller of b43_test_plan_*_reads() and must outlive the run.
 */
struct read_plan {
	u16 addr;
	const u16 *results;
	int cap;
	int iter;
};

/* Sized generously; register N=64 per space at most. Scanning is
 * linear -- for our workload that beats hashing on cache. */
#define MAX_PLANS  64

static struct read_plan phy_plans[MAX_PLANS];
static struct read_plan rad_plans[MAX_PLANS];
static struct read_plan mmio_plans[MAX_PLANS];
static int phy_plans_n, rad_plans_n, mmio_plans_n;

static struct read_plan *plan_lookup(struct read_plan *tbl, int n, u16 addr)
{
	for (int i = 0; i < n; i++)
		if (tbl[i].addr == addr)
			return &tbl[i];
	return NULL;
}

static void plan_add(struct read_plan *tbl, int *n, u16 addr,
		     const u16 *results, int cap)
{
	struct read_plan *p = plan_lookup(tbl, *n, addr);
	if (p) {
		/* Re-register clears the cursor; useful when a flow
		 * runs the same address through two logical polls. */
		p->results = results;
		p->cap = cap;
		p->iter = 0;
		return;
	}
	if (*n == MAX_PLANS) {
		fprintf(stderr, "wrap: MAX_PLANS exceeded for addr 0x%04x\n",
			addr);
		return;
	}
	tbl[*n].addr = addr;
	tbl[*n].results = results;
	tbl[*n].cap = cap;
	tbl[*n].iter = 0;
	(*n)++;
}

void b43_test_plan_phy_reads(u16 addr, const u16 *results, int cap)
{ plan_add(phy_plans, &phy_plans_n, addr, results, cap); }

void b43_test_plan_radio_reads(u16 addr, const u16 *results, int cap)
{ plan_add(rad_plans, &rad_plans_n, addr, results, cap); }

void b43_test_plan_mmio_reads(u16 addr, const u16 *results, int cap)
{ plan_add(mmio_plans, &mmio_plans_n, addr, results, cap); }

void b43_test_plans_reset(void)
{
	memset(phy_plans, 0, sizeof(phy_plans));
	memset(rad_plans, 0, sizeof(rad_plans));
	memset(mmio_plans, 0, sizeof(mmio_plans));
	phy_plans_n = rad_plans_n = mmio_plans_n = 0;
	memset(mirror_phy, 0, sizeof(mirror_phy));
	memset(mirror_radio, 0, sizeof(mirror_radio));
	memset(mirror_mmio, 0, sizeof(mirror_mmio));
	memset(pll_vals, 0, sizeof(pll_vals));
}

void b43_test_mirror_radio_set(u16 reg, u16 val)
{
	if (reg < MIRROR_RADIO_SZ)
		mirror_radio[reg] = val;
}

void b43_test_mirror_phy_set(u16 reg, u16 val)
{
	if (reg < MIRROR_PHY_SZ)
		mirror_phy[reg] = val;
}

u16 b43_test_mirror_phy_get(u16 reg)
{
	return (reg < MIRROR_PHY_SZ) ? mirror_phy[reg] : 0;
}

/* Diagnostics: main.c may want to know whether every scripted plan was
 * consumed to completion (indicating the flow polled as expected). */
void b43_test_plans_report(FILE *f)
{
	for (int i = 0; i < phy_plans_n; i++)
		fprintf(f, "phy   0x%04x  iter=%d/%d\n",
			phy_plans[i].addr, phy_plans[i].iter, phy_plans[i].cap);
	for (int i = 0; i < rad_plans_n; i++)
		fprintf(f, "radio 0x%04x  iter=%d/%d\n",
			rad_plans[i].addr, rad_plans[i].iter, rad_plans[i].cap);
	for (int i = 0; i < mmio_plans_n; i++)
		fprintf(f, "mmio  0x%04x  iter=%d/%d\n",
			mmio_plans[i].addr, mmio_plans[i].iter, mmio_plans[i].cap);
}

static FILE *trace_stream = NULL;

static FILE *trace(void)
{
	if (!trace_stream)
		trace_stream = stdout;
	return trace_stream;
}

/*
 * Function-boundary markers (see B43_AC_FN in phy_ac.h). Emit into the same
 * trace stream, in order, only when AC_FN_MARKERS is set in the environment;
 * otherwise silent, so the default trace matches the vendor capture for
 * compare.py. Enter/leave nest, so fn_map.py can attribute each op to the
 * innermost active function.
 */
static int b43_ac_fn_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = getenv("AC_FN_MARKERS") ? 1 : 0;
	return enabled;
}

void b43_ac_fn_enter(const char *fn)
{
	if (b43_ac_fn_enabled())
		fprintf(trace(), "----FN:%s----\n", fn);
}

void b43_ac_fn_leave(const char *fn)
{
	if (b43_ac_fn_enabled())
		fprintf(trace(), "----/FN:%s----\n", fn);
}

void b43_test_trace_to(FILE *f) { trace_stream = f; }

/* ============ Read oracle ============
 *
 * Invece di read plan scritti a mano, i valori di ritorno possono venire dalla
 * cattura vendor: la N-esima lettura di un indirizzo restituisce la N-esima
 * lettura di quello stesso indirizzo nella cattura, in ordine di episodio.
 * Si attiva con AC_READ_ORACLE=<cattura passata per `trace_filter.py
 * --retvals`>, e
 * AC_READ_ORACLE_FROM=<episodio> limita il caricamento alla fase che il flow
 * copre. Serve: le code sono per indirizzo e in ordine, quindi caricare tutta
 * la cattura per un flow che ne esegue una fetta fa consumare valori di letture
 * precedenti alla fetta.
 *
 * Attenzione a cosa questo dimostra: le read combaciano per costruzione, non
 * perche' il driver le indovini. Il guadagno e' sulle *write derivate*, che
 * dipendono dai valori letti e diventano confrontabili. E l'esaurimento di una
 * coda (il driver legge un indirizzo piu' volte del vendor) e' un segnale, non
 * un dettaglio: viene contato e riportato da b43_test_oracle_report().
 */
#define ORACLE_ADDRS	0x2000

struct oracle_q {
	u16 *v;
	int n, cap, iter;
};

static struct oracle_q oracle_phy[ORACLE_ADDRS];
static struct oracle_q oracle_rad[ORACLE_ADDRS];
static struct oracle_q oracle_obj[ORACLE_ADDRS];
static int oracle_on;
static unsigned long oracle_from;
static long oracle_hits, oracle_miss_addr, oracle_miss_exhausted;

static int oracle_has_obj, oracle_has_tpl, oracle_has_cal;
static const char *oracle_path;

static void oracle_push(struct oracle_q *tbl, unsigned addr, unsigned val)
{
	struct oracle_q *q;

	if (addr >= ORACLE_ADDRS)
		return;
	q = &tbl[addr];
	if (q->n == q->cap) {
		int nc = q->cap ? q->cap * 2 : 8;
		u16 *nv = realloc(q->v, (size_t)nc * sizeof(*nv));
		if (!nv)
			return;
		q->v = nv;
		q->cap = nc;
	}
	q->v[q->n++] = (u16)val;
}

static void oracle_init(void)
{
	static int tried;
	const char *path;
	char line[512];
	FILE *f;

	if (tried)
		return;
	tried = 1;

	path = getenv("AC_READ_ORACLE");
	if (!path || !*path)
		return;
	oracle_path = path;
	{
		const char *e = getenv("AC_READ_ORACLE_FROM");

		oracle_from = e ? strtoul(e, NULL, 0) : 0;
	}
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "wrap: oracle: cannot open %s\n", path);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		unsigned addr, val;
		char *p;

		/*
		 * Track the classes the capture traces at all, before the
		 * episode filter: the tracer gained OBJ, TPL and CAL hooks over
		 * time, and a capture taken earlier has none of those ops. An
		 * op missing from such a capture says nothing about the driver,
		 * so a flow that needs them must not be gated on it. See
		 * router-data/CLASS-COVERAGE.md.
		 */
		if (strstr(line, " OBJ."))
			oracle_has_obj = 1;
		if (strstr(line, " TPL."))
			oracle_has_tpl = 1;
		if (strstr(line, " CAL."))
			oracle_has_cal = 1;

		if (oracle_from) {
			char *h = strchr(line, '#');
			unsigned long ep = h ? strtoul(h + 1, NULL, 10) : 0;

			if (ep < oracle_from)
				continue;
		}

		if ((p = strstr(line, "PHY.RD")) != NULL) {
			if (sscanf(p, "PHY.RD %*[^=]=%x %*[^=]=%x",
				   &addr, &val) == 2)
				oracle_push(oracle_phy, addr, val);
		} else if ((p = strstr(line, "RAD.RD")) != NULL) {
			if (sscanf(p, "RAD.RD %*[^=]=%x %*[^=]=%x",
				   &addr, &val) == 2)
				oracle_push(oracle_rad, addr, val);
		} else if ((p = strstr(line, "OBJ.RD")) != NULL) {
			if (sscanf(p, "OBJ.RD %*[^=]=%x %*[^=]=%x",
				   &addr, &val) == 2)
				oracle_push(oracle_obj, addr, val);
		}
	}
	fclose(f);
	oracle_on = 1;
	fprintf(stderr, "wrap: oracle attivo da %s\n", path);
}

/* Ritorna 1 e scrive *out se l'oracolo ha un valore per questo indirizzo. */
/*
 * Complain once if the capture behind the oracle does not trace a class. The
 * flow may still be comparable on the classes that are there, but any argument
 * from the absence of an op in a missing class is void.
 */
void b43_test_oracle_coverage_report(void)
{
	if (!oracle_path)
		return;
	if (oracle_has_obj && oracle_has_tpl && oracle_has_cal)
		return;

	fprintf(stderr, "wrap: oracle %s does not trace:%s%s%s\n", oracle_path,
		oracle_has_obj ? "" : " OBJ",
		oracle_has_tpl ? "" : " TPL",
		oracle_has_cal ? "" : " CAL");
	fprintf(stderr, "wrap:   absence of those ops in it is not evidence; "
		"see router-data/CLASS-COVERAGE.md\n");
}


static int perturb_addr_valid;
static unsigned perturb_addr;
static u16 perturb_mask = 1;
static struct oracle_q *perturb_tbl;

static void perturb_init(void)
{
	static int done;
	const char *a, *m, *k;

	if (done)
		return;
	done = 1;
	a = getenv("AC_READ_PERTURB");
	if (!a)
		return;
	perturb_addr = (unsigned)strtoul(a, NULL, 0);
	m = getenv("AC_READ_PERTURB_MASK");
	if (m)
		perturb_mask = (u16)strtoul(m, NULL, 0);
	k = getenv("AC_READ_PERTURB_KIND");
	perturb_tbl = (k && !strcmp(k, "radio")) ? oracle_rad
		    : (k && !strcmp(k, "obj")) ? oracle_obj : oracle_phy;
	perturb_addr_valid = 1;
}

static int oracle_take(struct oracle_q *tbl, u16 addr, u16 *out)
{
	struct oracle_q *q;

	oracle_init();
	perturb_init();
	if (!oracle_on || addr >= ORACLE_ADDRS)
		return 0;
	q = &tbl[addr];
	if (!q->n) {
		oracle_miss_addr++;
		return 0;
	}
	if (q->iter >= q->n) {
		oracle_miss_exhausted++;
		return 0;
	}
	*out = q->v[q->iter++];

	/*
	 * Perturbation hook. AC_READ_PERTURB names one address, and every
	 * value the oracle hands out for it is XORed with AC_READ_PERTURB_MASK
	 * (0x0001 by default).
	 *
	 * The point is to find reads the port does not actually consume. A read
	 * whose value the driver uses changes the emitted trace when perturbed;
	 * one it discards does not. That is decidable at runtime and covers the
	 * reads whose address is computed, which a source scan cannot classify.
	 */
	if (perturb_addr_valid && addr == perturb_addr && tbl == perturb_tbl)
		*out ^= perturb_mask;

	oracle_hits++;
	return 1;
}

void b43_test_oracle_report(void)
{
	int a, addrs = 0, partial = 0;

	if (!oracle_on)
		return;
	for (a = 0; a < ORACLE_ADDRS; a++) {
		struct oracle_q *q[3] = {
			&oracle_phy[a], &oracle_rad[a], &oracle_obj[a]
		};
		static const char *const nm[3] = { "phy", "radio", "obj" };
		int i;

		for (i = 0; i < 3; i++) {
			if (!q[i]->n)
				continue;
			addrs++;
			if (q[i]->iter != q[i]->n) {
				partial++;
				fprintf(stderr,
					"oracle %s 0x%04x  consumate %d/%d\n",
					nm[i], a,
					q[i]->iter, q[i]->n);
			}
		}
	}
	fprintf(stderr,
		"oracle: %ld hit, %ld indirizzi senza voce, %ld code esaurite; "
		"%d indirizzi noti, %d non consumati del tutto\n",
		oracle_hits, oracle_miss_addr, oracle_miss_exhausted,
		addrs, partial);
}

/* ============ PHY register accessors ============ */

u16 __wrap_b43_phy_read(struct b43_wldev *dev, u16 reg)
{
	struct read_plan *p;
	u16 v;

	(void)dev;

	if (oracle_take(oracle_phy, reg, &v))
		goto out;

	p = plan_lookup(phy_plans, phy_plans_n, reg);
	if (p && p->iter < p->cap) {
		v = p->results[p->iter];
		p->iter++;
		goto out;
	}
	/* oracolo e plan assenti o esauriti: cadi sul mirror delle write. */
	v = (reg < MIRROR_PHY_SZ) ? mirror_phy[reg] : 0;
	/*
	 * Backstop, non un modello: le letture di 0x0270 arrivano dall'oracolo o
	 * dal plan (readplan_0270.h). Se entrambi si esauriscono, il mirror
	 * restituirebbe il bit di start ancora alto e il poll girerebbe fino al
	 * timeout; qui l'attesa termina invece subito, cosi' un plan troppo
	 * corto si vede come conteggio di op sbagliato e non come stallo.
	 */
	if (reg == 0x0270)
		v &= (u16)~0x0001;
out:
	fprintf(trace(), "cpu1 PHY.RD   addr=0x%04x val=0x%04x\n", reg, v);
	return v;
}

/*
 * status_mask derivato dallo stato dei registri, non dalle op emesse. La tabella
 * e' la stessa di reverse-tools/annotate_enables.py, cosi' lo stato che le
 * REQUIRE dello scratch vedono e' lo stesso che l'annotatore ricava dalla
 */
static void phy_state_track(struct b43_wldev *dev, u16 reg, u16 val)
{
	struct b43_phy_ac *ac = dev->phy.ac;

	if (!ac)
		return;

	switch (reg) {
	case 0x0140:				/* CLASSCTL */
		ac->status_mask &= ~B43_PHY_AC_STATE_RX_ANY;
		if (val & 0x1)
			ac->status_mask |= B43_PHY_AC_STATE_RX_CCK;
		if (val & 0x2)
			ac->status_mask |= B43_PHY_AC_STATE_RX_OFDM;
		if (val & 0x4)
			ac->status_mask |= B43_PHY_AC_STATE_RX_WAITED;
		break;
	case 0x0001:				/* BBCFG: RSTCCA */
		if (val & 0x4000)
			ac->status_mask |= B43_PHY_AC_STATE_CCA_RESET;
		else
			ac->status_mask &= ~B43_PHY_AC_STATE_CCA_RESET;
		break;
	case 0x06d4:
	case 0x08d4:
	case 0x0ad4: {
		u16 bit = (reg == 0x06d4) ? B43_PHY_AC_STATE_CLIP_C0_DIS :
			  (reg == 0x08d4) ? B43_PHY_AC_STATE_CLIP_C1_DIS :
					    B43_PHY_AC_STATE_CLIP_C2_DIS;
		if (val & 0x4000)
			ac->status_mask |= bit;
		else
			ac->status_mask &= ~bit;
		break;
	}
	case 0x1720:				/* front-end armed vs parked */
		if (val & 0x0200)
			ac->status_mask |= B43_PHY_AC_STATE_AFE_ON;
		else
			ac->status_mask &= ~B43_PHY_AC_STATE_AFE_ON;
		break;
	default:
		break;
	}
}

void __wrap_b43_phy_write(struct b43_wldev *dev, u16 reg, u16 val)
{
	fprintf(trace(), "cpu1 PHY.WR   addr=0x%04x val=0x%04x\n", reg, val);
	if (reg < MIRROR_PHY_SZ) mirror_phy[reg] = val;
	phy_state_track(dev, reg, val);
}

void __wrap_b43_phy_mask(struct b43_wldev *dev, u16 reg, u16 mask)
{
	(void)dev;
	/*
	 * kernel: reg = read(reg) & mask
	 * vendor: single PHY.MOD line with val=<kmask>, mask=0 (no RD).
	 *   Verified: b43_phy_mask(0x0471, ~0x0001) at D6220 #82499 emits
	 *   PHY.MOD addr=0x0471 val=0xfffe mask=0x0000.
	 */
	fprintf(trace(), "cpu1 PHY.MOD  addr=0x%04x val=0x%04x mask=0x0000\n",
		reg, mask);
	if (reg < MIRROR_PHY_SZ) mirror_phy[reg] &= mask;
}

void __wrap_b43_phy_set(struct b43_wldev *dev, u16 reg, u16 val)
{
	(void)dev;
	/*
	 * kernel: reg = read(reg) | val
	 * vendor: single PHY.MOD line with val=<kset>, mask=0 (no RD).
	 *   Verified: b43_phy_set(0x0400, 0x0001) at D6220 #82504 emits
	 *   PHY.MOD addr=0x0400 val=0x0001 mask=0x0000.
	 */
	fprintf(trace(), "cpu1 PHY.MOD  addr=0x%04x val=0x%04x mask=0x0000\n",
		reg, val);
	if (reg < MIRROR_PHY_SZ) mirror_phy[reg] |= val;
}

/*
 * PHY.MOD format for phy_maskset -- distinct from phy_mask/phy_set:
 *   val  = kernel_set  (OR-in bits, arg3)
 *   mask = ~kernel_mask (bits being modified, i.e. NOT preserved)
 * Verified: b43_phy_maskset(0x0271, ~0x00ff, 0x0020) at D6220 #82534
 * emits PHY.MOD addr=0x0271 val=0x0020 mask=0x00ff.
 *
 * The three convention variants (mask / set / maskset) reflect the way
 * the vendor wl-diag tracer intercepts each entry point independently;
 * although mask() and set() are semantically special cases of maskset(),
 * they get their own trace format with mask=0 as the sentinel.
 */
void __wrap_b43_phy_maskset(struct b43_wldev *dev, u16 reg, u16 mask, u16 set)
{
	fprintf(trace(), "cpu1 PHY.MOD  addr=0x%04x val=0x%04x mask=0x%04x\n",
		reg, set, (u16)~mask);
	if (reg < MIRROR_PHY_SZ) {
		mirror_phy[reg] = (mirror_phy[reg] & mask) | set;
		phy_state_track(dev, reg, mirror_phy[reg]);
	}
}

void __wrap_b43_phy_force_clock(struct b43_wldev *dev, bool force)
{
	(void)dev;
	fprintf(trace(), "; phy_force_clock %d\n", force);
}

/* ============ RADIO register accessors ============ */

u16 __wrap_b43_radio_read(struct b43_wldev *dev, u16 reg)
{
	struct read_plan *p;
	u16 v;

	(void)dev;

	if (oracle_take(oracle_rad, reg, &v))
		goto out;

	p = plan_lookup(rad_plans, rad_plans_n, reg);
	if (p && p->iter < p->cap) {
		v = p->results[p->iter];
		p->iter++;
		goto out;
	}
	v = (reg < MIRROR_RADIO_SZ) ? mirror_radio[reg] : 0;
out:
	fprintf(trace(), "cpu1 RAD.RD   addr=0x%04x val=0x%04x\n", reg, v);
	return v;
}

void __wrap_b43_radio_write(struct b43_wldev *dev, u16 reg, u16 val)
{
	(void)dev;
	fprintf(trace(), "cpu1 RAD.WR   addr=0x%04x val=0x%04x\n", reg, val);
	if (reg < MIRROR_RADIO_SZ) mirror_radio[reg] = val;
}

void __wrap_b43_radio_mask(struct b43_wldev *dev, u16 reg, u16 mask)
{
	u16 cur, nxt;
	fprintf(trace(), "cpu1 RAD.MOD  addr=0x%04x val=0x0000 mask=0x%04x\n",
		reg, (u16)~mask);
	cur = __wrap_b43_radio_read(dev, reg);	/* emette RAD.RD + read plan */
	nxt = cur & mask;
	fprintf(trace(), "cpu1 RAD.WR   addr=0x%04x val=0x%04x\n", reg, nxt);
	if (reg < MIRROR_RADIO_SZ) mirror_radio[reg] = nxt;
}

void __wrap_b43_radio_set(struct b43_wldev *dev, u16 reg, u16 val)
{
	u16 cur, nxt;
	fprintf(trace(), "cpu1 RAD.MOD  addr=0x%04x val=0x%04x mask=0x%04x\n",
		reg, val, val);
	cur = __wrap_b43_radio_read(dev, reg);
	nxt = cur | val;
	fprintf(trace(), "cpu1 RAD.WR   addr=0x%04x val=0x%04x\n", reg, nxt);
	if (reg < MIRROR_RADIO_SZ) mirror_radio[reg] = nxt;
}

void __wrap_b43_radio_maskset(struct b43_wldev *dev, u16 reg, u16 mask, u16 set)
{
	/*
	 * Il tracer wl-diag vendor espande OGNI radio maskset in tripletta
	 * MOD + RD + WR (visibile l'RMW hardware reale: il chip non ha un
	 * comando "maskset" atomico sul bus radio, il blob fa read+write e
	 * wl-diag li mostra entrambi). Il PHY invece è tracciato atomico
	 * (solo MOD). Riprodotto qui per coerenza col trace vendor. Il peek
	 * RAD.RD è delegato a __wrap_b43_radio_read così onora eventuali
	 * read plans registrati per riflettere lo stato pre-esistente del
	 * silicio (bit non azzerati dallo scratch).
	 */
	u16 cur, nxt;
	fprintf(trace(), "cpu1 RAD.MOD  addr=0x%04x val=0x%04x mask=0x%04x\n",
		reg, set, (u16)~mask);
	cur = __wrap_b43_radio_read(dev, reg);
	nxt = (cur & mask) | set;
	fprintf(trace(), "cpu1 RAD.WR   addr=0x%04x val=0x%04x\n", reg, nxt);
	if (reg < MIRROR_RADIO_SZ) mirror_radio[reg] = nxt;
}

/* ============ Raw MMIO ============ */

u16 __wrap_b43_read16(struct b43_wldev *dev, u16 off)
{
	(void)dev;
	struct read_plan *p = plan_lookup(mmio_plans, mmio_plans_n, off);
	u16 v;

	if (p && p->iter < p->cap) {
		v = p->results[p->iter];
		p->iter++;
	} else {
		v = (off < MIRROR_MMIO_SZ) ? mirror_mmio[off] : 0;
	}
	fprintf(trace(), "cpu1 MMIO.RD  off=0x%04x  val=0x%04x\n", off, v);
	return v;
}

void __wrap_b43_write16(struct b43_wldev *dev, u16 off, u16 val)
{
	(void)dev;
	fprintf(trace(), "cpu1 MMIO.WR  off=0x%04x  val=0x%04x\n", off, val);
	if (off < MIRROR_MMIO_SZ) mirror_mmio[off] = val;
}

void __wrap_b43_write16f(struct b43_wldev *dev, u16 off, u16 val)
{
	__wrap_b43_write16(dev, off, val);
	/* _f variant on real HW does a read-back flush; emit it so op counts
	 * line up with the vendor trace. */
	(void)__wrap_b43_read16(dev, off);
}

/* ============ Table accessors (b43_actab_*) ============
 * These are defined in tables_phy_ac.c and expand to a series of
 * PHY.WR (id/offset) + PHY.WR/RD (data). We emit a TBL.* header line to
 * mirror the wl-diag layout, then delegate to __real_* so the underlying
 * PHY.* lines are produced by the phy_write/phy_read wrappers above.
 */
void __real_b43_actab_write_bulk(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data);
void __real_b43_actab_read_bulk(struct b43_wldev *dev,
				u16 id, u16 offset, u8 width,
				size_t len, void *data);
void __real_b43_actab_zerofill(struct b43_wldev *dev,
			       u16 id, u16 offset, u8 width, size_t len);
void __real_b43_actab_write_bulk_locked(struct b43_wldev *dev,
					u16 id, u16 offset, u8 width,
					size_t len, const void *data);
void __real_b43_actab_zerofill_locked(struct b43_wldev *dev,
				      u16 id, u16 offset, u8 width, size_t len);

void __wrap_b43_actab_write_bulk(struct b43_wldev *dev,
				 u16 id, u16 offset, u8 width,
				 size_t len, const void *data)
{
	fprintf(trace(), "cpu1 TBL.WR   id=0x%04x off=0x%04x len=%zu\n",
		id, offset, len);
	__real_b43_actab_write_bulk(dev, id, offset, width, len, data);
}

void __wrap_b43_actab_zerofill(struct b43_wldev *dev,
			       u16 id, u16 offset, u8 width, size_t len)
{
	fprintf(trace(), "cpu1 TBL.WR   id=0x%04x off=0x%04x len=%zu\n",
		id, offset, len);
	__real_b43_actab_zerofill(dev, id, offset, width, len);
}

void __wrap_b43_actab_write_bulk_locked(struct b43_wldev *dev,
					u16 id, u16 offset, u8 width,
					size_t len, const void *data)
{
	fprintf(trace(), "cpu1 TBL.WR   id=0x%04x off=0x%04x len=%zu\n",
		id, offset, len);
	__real_b43_actab_write_bulk_locked(dev, id, offset, width, len, data);
}

void __wrap_b43_actab_zerofill_locked(struct b43_wldev *dev,
				      u16 id, u16 offset, u8 width, size_t len)
{
	fprintf(trace(), "cpu1 TBL.WR   id=0x%04x off=0x%04x len=%zu\n",
		id, offset, len);
	__real_b43_actab_zerofill_locked(dev, id, offset, width, len);
}

/*
 * write_bulk_reopen: variante emessa quando il gate era unlockato
 * all'entrata (aggiunge relock intermedio). Il tracer emette la stessa
 * label TBL.WR — nel vendor trace il pattern label + peek + relock + WR ID
 * è indistinguibile dal caso normale (che ha solo label + peek + WR ID).
 */
void __real_b43_actab_write_bulk_reopen(struct b43_wldev *dev,
					u16 id, u16 offset, u8 width,
					size_t len, const void *data);

void __wrap_b43_actab_write_bulk_reopen(struct b43_wldev *dev,
					u16 id, u16 offset, u8 width,
					size_t len, const void *data)
{
	fprintf(trace(), "cpu1 TBL.WR   id=0x%04x off=0x%04x len=%zu\n",
		id, offset, len);
	__real_b43_actab_write_bulk_reopen(dev, id, offset, width, len, data);
}

/*
 * write_bulk_scoped: variante auto-contained (fase B4, vendor #41503+).
 * Emette lo stesso label TBL.WR; le op interne (peek + lock + WR ID + WR
 * data + unlock) sono generate dai wrap phy_read/phy_write/phy_maskset.
 */
void __real_b43_actab_write_bulk_scoped(struct b43_wldev *dev,
					u16 id, u16 offset, u8 width,
					size_t len, const void *data);

void __wrap_b43_actab_write_bulk_scoped(struct b43_wldev *dev,
					u16 id, u16 offset, u8 width,
					size_t len, const void *data)
{
	fprintf(trace(), "cpu1 TBL.WR   id=0x%04x off=0x%04x len=%zu\n",
		id, offset, len);
	__real_b43_actab_write_bulk_scoped(dev, id, offset, width, len, data);
}

/*
 * TX/RX-LPF table-7 pre-state. On silicon the {lo,hi} cells that hold the
 * 25-bit analog LPF word are pre-loaded (by the table init that runs before
 * switch_channel, not re-executed in this harness) with a per-stage base that the
 * RMW preserves, rewriting only the cap fields. The base is the same across
 * units -- only the cap (from rccal E/F) varies -- so it is a constant here.
 * TX: lo {0,1,2,8}=0x00db {3,4,5}=0x0123 {6,7}=0x016b, hi=0x0001, 9 stages at
 * lo {0x142,0x152,0x162}+stage / hi {0x362,0x372,0x382}+stage.
 * RX: lo base bit0-5 = 0x00/0x09/0x12 per stage (0x2000/0x2009/0x2012), hi 0,
 * 3 stages at the sparse offsets below. Returns -1 if not an LPF cell.
 */
static int txlpf_prestate(u16 id, u16 offset)
{
	static const u16 lo_base[3] = { 0x142, 0x152, 0x162 };
	static const u16 hi_base[3] = { 0x362, 0x372, 0x382 };
	static const u16 lo_pre[9] = {
		0x00db, 0x00db, 0x00db,
		0x0123, 0x0123, 0x0123,
		0x016b, 0x016b,
		0x00db,
	};
	static const u16 rx_lo[3][3] = {
		{ 0x140, 0x150, 0x160 },
		{ 0x141, 0x151, 0x161 },
		{ 0x441, 0x443, 0x445 },
	};
	static const u16 rx_hi[3][3] = {
		{ 0x360, 0x370, 0x380 },
		{ 0x361, 0x371, 0x381 },
		{ 0x440, 0x442, 0x444 },
	};
	static const u16 rx_lo_pre[3] = { 0x2000, 0x2009, 0x2012 };
	/* DACBUF cells: base[core] + add[stage], add = {b,b,c,c,e,e,f,f,a}.
	 * Base cell 0x0b20 for stages 0-7 (add b/c/e/f), 0x0020 for stage 8
	 * (add a); the RMW writes the cap into it. */
	static const u16 dac_base[3] = { 0x3f0, 0x60, 0xd0 };
	int core, stage;

	if (id != 7)
		return -1;
	for (core = 0; core < 3; core++) {
		u16 b = dac_base[core];
		if (offset == b + 0xb || offset == b + 0xc ||
		    offset == b + 0xe || offset == b + 0xf)
			return 0x0b20;
		if (offset == b + 0xa)
			return 0x0020;
	}
	for (core = 0; core < 3; core++) {
		if (offset >= lo_base[core] && offset < lo_base[core] + 9)
			return lo_pre[offset - lo_base[core]];
		if (offset >= hi_base[core] && offset < hi_base[core] + 9)
			return 0x0001;
	}
	for (stage = 0; stage < 3; stage++) {
		for (core = 0; core < 3; core++) {
			if (offset == rx_lo[stage][core])
				return rx_lo_pre[stage];
			if (offset == rx_hi[stage][core])
				return 0x0000;
		}
	}
	return -1;
}

/*
 * Plan per cella di tabella, chiavato su (id, offset). Le letture di tabella
 * passano tutte dalla porta 0x000f, quindi un plan su quell'indirizzo dipende
 * dalla posizione nella coda globale; questo no. I valori vengono spinti nella
 * coda della porta appena prima della lettura, cosi' l'ordine si arrangia.
 */
#define MAX_TBL_PLANS 32
struct tbl_plan {
	u16 id, off;
	const u16 *vals;
	int n, cur;
};
static struct tbl_plan tbl_plans[MAX_TBL_PLANS];
static int tbl_plans_n;

void b43_test_plan_table_cell(u16 id, u16 off, const u16 *vals, int n)
{
	if (tbl_plans_n == MAX_TBL_PLANS) {
		fprintf(stderr, "wrap: MAX_TBL_PLANS superato (id 0x%04x off 0x%04x)\n",
			id, off);
		return;
	}
	tbl_plans[tbl_plans_n].id = id;
	tbl_plans[tbl_plans_n].off = off;
	tbl_plans[tbl_plans_n].vals = vals;
	tbl_plans[tbl_plans_n].n = n;
	tbl_plans[tbl_plans_n].cur = 0;
	tbl_plans_n++;
}

/* Consuma `len` valori dal plan della cella, avanzando il cursore. */
static const u16 *tbl_plan_take(u16 id, u16 off, size_t len)
{
	int i;

	for (i = 0; i < tbl_plans_n; i++) {
		struct tbl_plan *p = &tbl_plans[i];

		if (p->id != id || p->off != off)
			continue;
		if (p->cur + (int)len > p->n)
			return NULL;
		p->cur += (int)len;
		return p->vals + p->cur - (int)len;
	}
	return NULL;
}

void __wrap_b43_actab_read_bulk(struct b43_wldev *dev,
				u16 id, u16 offset, u8 width,
				size_t len, void *data)
{
	int pre = txlpf_prestate(id, offset);
	const u16 *tv = tbl_plan_take(id, offset, len);

	if (tv)
		plan_add(phy_plans, &phy_plans_n, 0x000f, tv, (int)len);

	fprintf(trace(), "cpu1 TBL.RD   id=0x%04x off=0x%04x len=%zu\n",
		id, offset, len);
	/*
	 * Seed the data-port with the cell's real pre-state so the driver's
	 * read-modify-write of the TX-LPF word starts from silicon state, not
	 * from an empty mirror. Only the txlpf cells are affected; every other
	 * table read falls through to the real path unchanged.
	 */
	if (pre >= 0)
		mirror_phy[0x000f] = (u16)pre;
	__real_b43_actab_read_bulk(dev, id, offset, width, len, data);
}

/*
 * actab_write_r11 wrap: il vendor emette TBL.WR label per-cella (len=1)
 * seguito dal pattern peek + WR ID + WR OFFSET + WR DATA_2. Emettiamo la
 * label riga per riga chiamando __real con len=1 per volta, così ogni
 * label è affiancata dalle sue 4 op PHY.
 */
void __real_b43_actab_write_r11(struct b43_wldev *dev,
				u16 id, u16 offset, size_t len,
				const u16 *data);

void __wrap_b43_actab_write_r11(struct b43_wldev *dev,
				u16 id, u16 offset, size_t len,
				const u16 *data)
{
	size_t i;

	for (i = 0; i < len; i++) {
		fprintf(trace(),
			"cpu1 TBL.WR   id=0x%04x off=0x%04x len=1\n",
			id, (u16)(offset + i));
		__real_b43_actab_write_r11(dev, id, (u16)(offset + i), 1,
					   &data[i]);
	}
}

/* Same, for the constant-fill companion. */
void __real_b43_actab_fill_r11(struct b43_wldev *dev,
			       u16 id, u16 offset, size_t len, u16 val);

void __wrap_b43_actab_fill_r11(struct b43_wldev *dev,
			       u16 id, u16 offset, size_t len, u16 val)
{
	size_t i;

	for (i = 0; i < len; i++) {
		fprintf(trace(),
			"cpu1 TBL.WR   id=0x%04x off=0x%04x len=1\n",
			id, (u16)(offset + i));
		__real_b43_actab_fill_r11(dev, id, (u16)(offset + i), 1, val);
	}
}

/* ============ MAC / misc helpers ============ */

/*
 * b43_maccontrol_set: r/m/w di MMIO_MACCTL con `new = (old & mask) | set`.
 * Emette MAC.MCTRL con `mask` nel tracer = ~b43_mask (bit toccati).
 *
 * b43_mac_suspend/enable in mainline chiamano internamente questo helper
 * per clear/set del bit 0 (B43_MACCTL_ENABLED). Nel test wrap replichiamo
 * quella semantica — se il porting driver chiama mac_suspend in punti dove
 * il vendor non emette la stessa op, è un bug DEL PORTING, non del wrap.
 *
 * Nel test env aggiorniamo anche `dev->phy.ac->status_mask` con il bit
 * MAC_EN: quello che il driver fa in production è emettere la MAC.MCTRL,
 * il modello scratch traccia lo stato per i REQUIRE checks. Se l'op tocca
 * il bit 0 (B43_MACCTL_ENABLED), sincronizziamo il modello.
 */
#define B43_MACCTL_ENABLED  0x00000001u

void __wrap_b43_maccontrol_set(struct b43_wldev *dev, u32 mask, u32 set)
{
	fprintf(trace(),
		"cpu1 MAC.MCTRL val=0x%08x mask=0x%08x\n",
		set, (u32)~mask);

	if (((u32)~mask) & B43_MACCTL_ENABLED) {
		if (set & B43_MACCTL_ENABLED)
			dev->phy.ac->status_mask |= B43_PHY_AC_STATE_MAC_EN;
		else
			dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_MAC_EN;
	}
}

/*
 * Accessor del core che il corpo di b43_maccontrol_set() usa. Nell'harness
 * quel corpo non gira mai -- il --wrap sopra intercetta la chiamata -- ma
 * helpers_phy_ac.c ora e' nel link e il simbolo va risolto.
 */
void b43_maskset32(struct b43_wldev *dev, u16 offset, u32 mask, u32 set)
{
	(void)dev; (void)offset; (void)mask; (void)set;
}

/*
 * b43_mac_suspend/enable in-tree sono annidabili: tengono dev->mac_suspended e
 * toccano MACCTL solo sulle transizioni 0->1 e 1->0. E' il modello fedele, ed e'
 * il DEFAULT: AC_MAC_REFCOUNT=0 torna a una MAC.MCTRL per chiamata.
 *
 * Il default era il contrario, con la nota che i call site erano stati
 * trascritti nell'assunzione senza contatore e che col contatore il MATCH di
 * switch_channel sarebbe caduto. La misura dice altro:
 *
 *            MAC.MCTRL emesse      similarita' (compare_lcs.py)
 *   flow1    32 -> 113 (vendor 119)     49.50% -> 99.30%
 *   flow2    124 -> 124 (vendor 119)    99.19% -> 99.19%
 *
 * Su switch_channel il flag non ha effetto: quel percorso non annida. Le 6
 * MAC.MCTRL che ancora mancano su flow1 sono la lunghezza variabile del ciclo
 * probe, non op assenti -- vedi il commento in phy_ac.c sulle quattro inserite
 * prima della prima GPIO.
 *
 * I flow entrano con il MAC sospeso, come ops->init e switch_channel nel driver
 * vero; b43_test_mac_reset() riporta il contatore a quello stato.
 */
static int mac_refcount = -1;
/* AC_MAC_TRACE=1 per il log delle transizioni. */
static int mac_trace = -1;

/*
 * Lo stato d'ingresso dei flow e' MAC *acceso*: switch_channel viene chiamata da
 * b43_phy_init dopo ops->init, e le sue coppie suspend/enable producono le write
 * che il driver stock emette solo partendo da contatore 0. Verificato: con
 * AC_MAC_REFCOUNT=1 e questo stato, switch_channel combacia con 192 MAC.MCTRL,
 * zero annidamenti e zero enable fuori posto.
 *
 * Lo stato logico del MAC segue il contatore, non le write emesse. Con il
 * refcount attivo le transizioni che non scrivono MACCTL lasciavano status_mask
 * indietro, e le REQUIRE dello scratch fallivano saltando blocchi interi: il
 * flow si troncava invece di divergere.
 */
static void mac_state_sync(struct b43_wldev *dev)
{
	if (!dev->phy.ac)
		return;
	if (dev->mac_suspended > 0)
		dev->phy.ac->status_mask &= ~B43_PHY_AC_STATE_MAC_EN;
	else
		dev->phy.ac->status_mask |= B43_PHY_AC_STATE_MAC_EN;
}

static int mac_rc(void)
{
	if (mac_refcount < 0) {
		/*
		 * Default acceso: e' il modello fedele e la misura lo conferma. Sul
		 * flow full il port emette 113 MAC.MCTRL col contatore contro le 119
		 * del vendor, e 32 senza; in similarita' 99.30% contro 49.50%. Su
		 * switch_channel il flag e' irrilevante, 124 in entrambi i casi.
		 * AC_MAC_REFCOUNT=0 lo disattiva.
		 */
		const char *e = getenv("AC_MAC_REFCOUNT");
		mac_refcount = !(e && *e == '0');
		e = getenv("AC_MAC_TRACE");
		mac_trace = (e && *e == '1');
	}
	return mac_refcount;
}

void b43_test_mac_reset(void) { }

void __wrap_b43_mac_enable(struct b43_wldev *dev)
{
	if (!mac_rc()) {
		b43_maccontrol_set(dev, ~B43_MACCTL_ENABLED,
				   B43_MACCTL_ENABLED);
		return;
	}
	if (mac_trace)
		fprintf(stderr, "mac: ENA %d -> %d\n",
			dev->mac_suspended, dev->mac_suspended - 1);
	if (--dev->mac_suspended == 0)
		b43_maccontrol_set(dev, ~B43_MACCTL_ENABLED,
				   B43_MACCTL_ENABLED);
	if (dev->mac_suspended < 0)
		fprintf(stderr, "wrap: mac_suspended = %d (enable senza suspend)\n",
			dev->mac_suspended);
	mac_state_sync(dev);
}

void __wrap_b43_mac_suspend(struct b43_wldev *dev)
{
	if (!mac_rc()) {
		b43_maccontrol_set(dev, ~B43_MACCTL_ENABLED, 0);
		return;
	}
	if (mac_trace)
		fprintf(stderr, "mac: SUS %d -> %d\n",
			dev->mac_suspended, dev->mac_suspended + 1);
	if (dev->mac_suspended++ == 0)
		b43_maccontrol_set(dev, ~B43_MACCTL_ENABLED, 0);
	mac_state_sync(dev);
}

void __wrap_b43_mac_suspend_enable(struct b43_wldev *dev) { (void)dev; }
void __wrap_b43_mac_phy_clock_set(struct b43_wldev *dev, bool on)
{ (void)dev; (void)on; }

/*
 * MHF (Master Host Flag) maskset helper AC-PHY-specifico.
 * Emette label:
 *   MAC.MHF addr=<slot> val=<val> mask=<mask>
 * dove val e mask sono i 16 bit del maskset atomico applicato dal firmware
 * MAC sulla word MHF indicata da slot (0..4).
 *
 * Differisce da b43_hf_write mainline (che ha signature `u64 value` e
 * supporta solo 3 slot). Vedi test/stubs/b43.h per la motivazione.
 */
void __real_b43_phy_ac_mhf_maskset(struct b43_wldev *dev,
				   u16 slot, u16 mask, u16 val);

void __wrap_b43_phy_ac_mhf_maskset(struct b43_wldev *dev,
				   u16 slot, u16 mask, u16 val)
{
	fprintf(trace(), "cpu1 MAC.MHF  addr=0x%04x val=0x%04x mask=0x%04x\n",
		slot, val, (u16)~mask);

	/*
	 * La decisione di scrivere la cella sta nella funzione vera, che tiene
	 * lo shadow delle cinque word: qui si emette solo il record MAC.MHF,
	 * che e' il livello a cui l'hook del vendor traccia la chiamata. La
	 * OBJ.WR, quando c'e', esce da b43_shm_write16 come nel driver.
	 */
	__real_b43_phy_ac_mhf_maskset(dev, slot, mask, val);
}

void __wrap_b43_phyop_switch_analog_generic(struct b43_wldev *dev, bool on)
{ (void)dev; (void)on; }

/* ============ SHM (OBJ) accessors ============
 *
 * Definizioni piene, non wrap: nel link di test helpers_phy_ac.c non c'e',
 * quindi b43_shm_read16/write16 non hanno un'implementazione reale da
 * intercettare. Il tracer le emette come OBJ.RD/OBJ.WR, gli stessi
 * mnemonici della cattura vendor. Le letture pescano dall'oracolo per
 * indirizzo (i valori SHM sono stato ucode, non derivabile dal modello) e
 * cadono sul mirror delle write in sua assenza.
 */
#define MIRROR_SHM_WORDS 0x1000	/* offset byte < 0x2000 */
static u16 mirror_shm[MIRROR_SHM_WORDS];

u16 b43_shm_read16(struct b43_wldev *dev, u16 routing, u16 offset)
{
	u16 v;

	(void)dev; (void)routing;

	if (!oracle_take(oracle_obj, offset, &v))
		v = (offset / 2 < MIRROR_SHM_WORDS) ? mirror_shm[offset / 2]
						    : 0;
	fprintf(trace(), "cpu1 OBJ.RD   addr=0x%04x val=0x%04x\n", offset, v);
	return v;
}

void b43_shm_write16(struct b43_wldev *dev, u16 routing, u16 offset, u16 val)
{
	(void)dev; (void)routing;

	if (offset / 2 < MIRROR_SHM_WORDS)
		mirror_shm[offset / 2] = val;
	fprintf(trace(), "cpu1 OBJ.WR   addr=0x%04x val=0x%04x\n", offset, val);
}

/* ============ address match table ============
 *
 * Un record per riga, che e' la granularita' del tracer: il suo hook su
 * wlc_bmac_write_amt da' `AMT.WR idx=`, non le due word sottostanti. Quelle il
 * vendor le scrive sulla coppia objaddr/objdata direttamente, per una via che
 * nessun accessor agganciato copre, quindi non sono confrontabili.
 *
 * Non passa da b43_shm_write16: quella ignora il routing e indicizza il mirror
 * su offset/2, quindi una riga AMT -- word 0..127 -- calpesterebbe le celle
 * basse della shared memory, UCODEREV e le HOSTF comprese.
 */
void b43_test_emit_amt(u16 idx, u16 flags)
{
	if (flags)
		fprintf(trace(), "cpu1 AMT.WR    idx=0x%04x a3=0x%08x\n",
			idx, flags);
	else
		fprintf(trace(), "cpu1 AMT.WR    idx=0x%04x\n", idx);
}

/*
 * b43_current_band: the test main.c sets `b43_test_band` before the
 * flow under test is run. Default is 5 GHz (matches every capture we
 * compare against).
 */
enum nl80211_band b43_test_band = NL80211_BAND_5GHZ;
enum nl80211_band __wrap_b43_current_band(struct b43_wl *wl)
{ (void)wl; return b43_test_band; }

/* Trivial b43_phy_init fallback so the linker resolves it. */
int __wrap_b43_phy_init(struct b43_wldev *dev) { (void)dev; return 0; }

/* ============ Kernel utility stubs ============
 * These are not wrapped -- they are new symbols, since <linux/delay.h>
 * only declares them. Provide no-op bodies (or count-only if useful).
 */
/*
 * Scripted per-channel regulatory ceiling. AC_MAX_POWER sets the value for
 * every channel; AC_MAX_POWER_MAP overrides individual ones as a
 * comma-separated list of chan:dBm pairs, which is what exercises a
 * regulatory domain whose limit is not uniform across a bonded block.
 */
static struct ieee80211_channel g_reg_chans[64];
static unsigned int g_reg_n;

struct ieee80211_channel *ieee80211_get_channel(struct wiphy *wiphy, int freq)
{
	unsigned int i;

	(void)wiphy;
	for (i = 0; i < g_reg_n; i++)
		if (g_reg_chans[i].center_freq == freq)
			return &g_reg_chans[i];
	return NULL;
}

void b43_test_reg_init(int dflt, const char *map)
{
	static const u16 chans[] = {
		36, 40, 44, 48, 52, 56, 60, 64,
		100, 104, 108, 112, 116, 120, 124, 128,
		132, 136, 140, 144, 149, 153, 157, 161, 165,
	};
	unsigned int i;

	g_reg_n = 0;
	for (i = 0; i < sizeof(chans) / sizeof(chans[0]); i++) {
		g_reg_chans[g_reg_n].hw_value = chans[i];
		g_reg_chans[g_reg_n].center_freq = 5000 + 5 * chans[i];
		g_reg_chans[g_reg_n].max_power = dflt;
		g_reg_n++;
	}
	while (map && *map) {
		unsigned long ch = strtoul(map, (char **)&map, 10);
		long pw;

		if (*map != ':')
			break;
		pw = strtol(map + 1, (char **)&map, 10);
		for (i = 0; i < g_reg_n; i++)
			if (g_reg_chans[i].hw_value == ch)
				g_reg_chans[i].max_power = (int)pw;
		if (*map == ',')
			map++;
	}
}

void b43_mac_bw_set(struct b43_wldev *dev, u32 bw)
{
	(void)dev;
	fprintf(trace(), "cpu1 MAC.BW   addr=0x0000 val=%#06x\n", bw);
}

void udelay(unsigned long us)         { (void)us; }
void mdelay(unsigned long ms)         { (void)ms; }
void msleep(unsigned int ms)          { (void)ms; }
void usleep_range(unsigned long a, unsigned long b) { (void)a; (void)b; }

/* ============ bcma chipcommon (no wrap: definitions, not wraps) ============
 * These accessors are called directly by phy_ac.c (op_init, rfkill,
 * switch_channel). Upstream they touch real MMIO under bcma_drv_cc; here
 * they emit a line matching the vendor wl-diag format and return 0.
 * The `cc` pointer is opaque to the caller (we don't dereference it),
 * so the mock g_bcma_bus.drv_cc suffices.
 *
 * Vendor format (verified against d6220 down-to-bss-up capture #51707/8):
 *   PMU.RC  addr=OFF  val=<kernel_set>  mask=<bits_touched=~kernel_mask>
 *     (same convention as PHY.MOD: val is the set bits, mask is which
 *     bits the op modifies)
 *   GPIO.CTL   val=<value>  mask=<mask>
 *   GPIO.OUT   val=<value>  mask=<mask>
 *   GPIO.OUTEN val=<value>  mask=<mask>
 *     (no addr; kernel's mask is the mask directly)
 */
u32 bcma_chipco_gpio_out(struct bcma_drv_cc *cc, u32 mask, u32 value)
{
	(void)cc;
	fprintf(trace(), "cpu1 GPIO.OUT val=0x%08x mask=0x%08x\n",
		value, mask);
	return 0;
}

u32 bcma_chipco_gpio_outen(struct bcma_drv_cc *cc, u32 mask, u32 value)
{
	(void)cc;
	fprintf(trace(), "cpu1 GPIO.OUTEN val=0x%08x mask=0x%08x\n",
		value, mask);
	return 0;
}

u32 bcma_chipco_gpio_control(struct bcma_drv_cc *cc, u32 mask, u32 value)
{
	(void)cc;
	fprintf(trace(), "cpu1 GPIO.CTL val=0x%08x mask=0x%08x\n",
		value, mask);
	return 0;
}

void bcma_chipco_regctl_maskset(struct bcma_drv_cc *cc, u32 offset,
				u32 mask, u32 set)
{
	(void)cc;
	fprintf(trace(),
		"cpu1 PMU.RC   addr=0x%04x val=0x%08x mask=0x%08x\n",
		offset, set, ~mask);
}

/*
 * PLL readback for the driver's own PLLCTL verification. Not traced: the
 * reference captures were taken before the tracer logged PLL reads, so
 * emitting a line here would desync compare.py; the value is what the
 * check needs, seeded per-profile via b43_test_pll_set().
 */
void b43_test_pll_set(u32 offset, u32 val)
{
	if (offset < 8)
		pll_vals[offset] = val;
}

u32 bcma_chipco_pll_read(struct bcma_drv_cc *cc, u32 offset)
{
	(void)cc;
	return offset < 8 ? pll_vals[offset] : 0;
}
