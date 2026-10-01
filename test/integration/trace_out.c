// SPDX-License-Identifier: GPL-2.0
/*
 * L'emissione delle op, nel formato di ../unit/wrap.c.
 *
 * Compilato come codice UTENTE, non kernel: qui servono stdio e getenv, e
 * questo file non e' codice di driver. Il resto della suite si compila con
 * -nostdinc contro gli header kernel, quindi le due meta' non condividono
 * intestazioni di sistema -- solo trace_out.h, che dichiara i tipi che serve.
 *
 * Le letture ritornano quello che l'oracolo dice. Senza oracolo ritornano
 * zero, e zero su un bit di stato auto-azzerante e' il valore che fa girare
 * un poll fino al timeout: e' lo stesso problema che ../unit risolve con
 * AC_READ_ORACLE e i read plan. Qui l'oracolo si aggancia in una sola
 * funzione, oracle_lookup(), perche' quando arrivera' vada in un posto solo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static FILE *out;
static int cpu = 1;

static FILE *stream(void)
{
	if (!out) {
		const char *p = getenv("B43_TRACE_OUT");

		out = p ? fopen(p, "w") : stdout;
		if (!out)
			out = stdout;
		/*
		 * Una riga per op, subito: finche' la probe muore a meta'
		 * attach, cio' che ha emesso prima e' l'unica cosa da
		 * confrontare, e con il buffer pieno di stdio si perde tutto.
		 */
		setvbuf(out, NULL, _IOLBF, 0);
	}
	return out;
}

/*
 * L'oracolo delle letture.
 *
 * Serve per la ragione che ../unit ha imparato: senza, ogni lettura ritorna
 * zero, e zero su un bit di stato auto-azzerante fa terminare subito un poll
 * che dovrebbe girare, mentre su un bit che l'hardware alza da se' lo fa
 * girare fino al timeout. Una traccia con letture inventate sembra buona e
 * non e' una misura.
 *
 * Formato e semantica sono quelli di ../unit: una cattura del vendor coi
 * RETVAL ripiegati (`trace_filter.py --retvals`), servita in ORDINE per
 * (classe, indirizzo). Una coda per chiave, non un valore per chiave: il
 * vendor legge lo stesso registro molte volte e i valori sono diversi, ed e'
 * la successione a contare -- appiattirla su un valore fisso e' esattamente
 * cio' che fa terminare i poll a sproposito.
 *
 * Esaurita la coda si ritorna l'ultimo valore visto, non zero: e' il
 * comportamento meno sbagliato quando il port legge piu' del vendor, e lo
 * dice una volta su stderr.
 *
 * La coda e' quella del bus, non quella del tracer: un PHY.MOD del vendor e'
 * una lettura seguita da una scrittura, e la lettura non ha valore
 * registrato. Senza un posto in coda per quella lettura, il maskset del
 * driver -- che sul bus legge davvero -- consuma il valore della lettura
 * successiva e da li' in avanti l'indirizzo e' sfasato: sul segmento di
 * riferimento il primo PHY.RD 0x19e dello switch prendeva 0x3d2 invece di
 * 0x0. Il posto lo tiene ORACLE_SHADOW, che rende l'ultimo valore noto del
 * registro, letto o scritto: e' cio' che l'hardware avrebbe risposto salvo
 * bit che si muovono da soli. I RAD.MOD non ne hanno bisogno: il tracer del
 * vendor registra anche la RAD.RD e la RAD.WR interne, con i valori.
 */
#define ORACLE_KEYS	4096
#define ORACLE_MAX	65536
#define ORACLE_SHADOW	0xffffffffu

struct oracle_key {
	char cls[32];
	u16 addr;
	u32 *vals;
	unsigned n, cap, pos;
	u32 last;
	int written;
};

/*
 * Registri che rileggono l'ultimo valore scritto: dopo la prima scrittura
 * del driver si rende l'ombra, prima la coda. Dalla coda, una lettura in piu'
 * o in meno del port sfaserebbe tutte le successive.
 *
 *   0x019e  B43_PHY_AC_REG_TBL_WRITE_GATE
 */
static const struct { const char *cls; u16 addr; } oracle_latch[] = {
	{ "PHY.RD", 0x019e },
};

/*
 * Le letture dalle porte dati delle tabelle del PHY (0x000f-0x0011) hanno
 * una coda per indirizzo di tabella, id da 0x000d e offset da 0x000e: le
 * letture di un accesso consumano in ordine la coda del suo indirizzo
 * d'inizio, e una tabella che il port legge e il vendor no non tocca le code
 * delle altre.
 */
static u16 vendor_tbl_id, vendor_tbl_off, port_tbl_id, port_tbl_off;

static int tbl_port(const char *cls, u16 addr)
{
	return !strcmp(cls, "PHY.RD") && addr >= 0x000f && addr <= 0x0011;
}

static void tbl_key(char *buf, size_t len, u16 id, u16 off)
{
	snprintf(buf, len, "TBL.RD@%04x.%04x", id, off);
}

static void tbl_note_addr(u16 *id, u16 *off, u16 addr, u32 val)
{
	if (addr == 0x000d)
		*id = (u16)val;
	else if (addr == 0x000e)
		*off = (u16)val;
}

static int oracle_is_latch(const char *cls, u16 addr)
{
	unsigned i;

	for (i = 0; i < sizeof(oracle_latch) / sizeof(oracle_latch[0]); i++)
		if (oracle_latch[i].addr == addr &&
		    !strcmp(oracle_latch[i].cls, cls))
			return 1;
	return 0;
}

static struct oracle_key okeys[ORACLE_KEYS];
static unsigned n_okeys;
static int oracle_loaded;
static int oracle_absent_warned;

static struct oracle_key *okey(const char *cls, u16 addr, int create)
{
	unsigned i;

	for (i = 0; i < n_okeys; i++)
		if (okeys[i].addr == addr && !strcmp(okeys[i].cls, cls))
			return &okeys[i];
	if (!create || n_okeys == ORACLE_KEYS)
		return NULL;
	snprintf(okeys[n_okeys].cls, sizeof(okeys[n_okeys].cls), "%s", cls);
	okeys[n_okeys].addr = addr;
	return &okeys[n_okeys++];
}

static void oracle_push(const char *cls, u16 addr, u32 val)
{
	struct oracle_key *k = okey(cls, addr, 1);

	if (!k || k->n == ORACLE_MAX)
		return;
	if (k->n == k->cap) {
		k->cap = k->cap ? k->cap * 2 : 8;
		k->vals = realloc(k->vals, k->cap * sizeof(*k->vals));
	}
	k->vals[k->n++] = val;
}

/*
 * Una cattura a freddo dello sweep porta DUE attach in testa: `wl` al
 * caricamento fa l'attach di tutti i core, quindi prima di wl1 -- l'AC -- c'e'
 * quello di wl0, l'N-PHY a 2.4 GHz. Sono 46 op, e reverse-tools/
 * strip_other_core.py le toglie.
 *
 * Qui l'oracolo carica il file intero e non ha una finestra, quindi quelle op
 * finiscono in testa alle code per indirizzo e il driver sotto esame si legge
 * lo stato dell'altro core. Non e' un caso ipotetico: `UCODEREV` (0x0000) e
 * `UCODEPATCH` (0x0002) sono lette da b43_validate_chipaccess() su entrambi i
 * core, e senza il taglio il port riceve i pattern del self-test di wl0,
 * 0x55aa e 0xaa55, al posto dei valori veri.
 *
 * Il testimone e' il confine che definisce il taglio: fra la prima e la
 * seconda coppia OTP.RDR/OTP.INIT. Se prima della prima op PHY dell'attach AC
 * -- la lettura di 0x0739 -- ce ne sono due, la cattura non e' tagliata.
 *
 * Si rifiuta invece di avvisare: quattro valori sbagliati in mezzo a
 * ventimila op non si notano, e il resto della corsa sembra buono.
 */
static int oracle_has_other_core(FILE *f)
{
	char line[512];
	int otp = 0;

	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "PHY.RD") && strstr(line, "addr=0x0739"))
			break;
		if (strstr(line, "OTP.RDR"))
			otp++;
	}
	rewind(f);
	return otp >= 2;
}

/*
 * La shared memory ha piu' spazi -- SHARED, SCRATCH, UCODE, ... -- con gli
 * stessi indirizzi, e la cattura li distingue col campo `sel=` (la routing di
 * B43_MMIO_SHM_CONTROL nei 16 bit alti). La coda e' per spazio: una sola coda
 * per indirizzo darebbe alla lettura di UCODEREV il valore di un'altra
 * routing allo stesso offset.
 */
static void shm_key(char *buf, size_t len, u32 routing)
{
	snprintf(buf, len, "OBJ.RD/%x", routing);
}

/*
 * Le righe della cattura: `<ts> #<ep> cpuN <CLASSE> addr=0x.. val=0x..`, e
 * `off=` al posto di `addr=` per i registri del MAC (REG.RD), che porta solo
 * una cattura presa al bus (reverse-tools/mmio2ops.py).
 * Si prendono le letture, perche' l'oracolo risponde a quelle; delle
 * scritture del vendor solo l'indirizzo di tabella (0x000d, 0x000e), che da'
 * la chiave alle letture delle porte dati. Le altre sono il termine di
 * confronto, non un ingresso.
 */
static void oracle_load(void)
{
	const char *path = getenv("B43_READ_ORACLE");
	char line[512], cls[32];
	unsigned addr, val;
	FILE *f;

	oracle_loaded = 1;
	if (!path)
		return;
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "b43-integration: B43_READ_ORACLE=%s "
			"non si apre\n", path);
		return;
	}
	if (oracle_has_other_core(f)) {
		fprintf(stderr,
			"b43-integration: %s porta ancora l'attach di wl0 in "
			"testa: due coppie OTP.RDR/OTP.INIT prima della prima "
			"PHY.RD 0x0739.\n"
			"b43-integration:   Le code per indirizzo servirebbero "
			"al driver lo stato dell'altro core -- 0x0000 e 0x0002 "
			"col self-test di wl0.\n"
			"b43-integration:   Taglia prima:\n"
			"b43-integration:     python3 reverse-tools/"
			"strip_other_core.py <segmento> /tmp/seg\n"
			"b43-integration:     python3 reverse-tools/"
			"trace_filter.py --retvals /tmp/seg /tmp/oracolo\n",
			path);
		fclose(f);
		abort();
	}
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%*s #%*u cpu%*u %31s addr=0x%x val=0x%x",
			   cls, &addr, &val) != 3 &&
		    sscanf(line, "%*s #%*u cpu%*u %31s off=0x%x val=0x%x",
			   cls, &addr, &val) != 3)
			continue;
		if (!strcmp(cls, "OBJ.RD")) {
			const char *sel = strstr(line, " sel=0x");
			unsigned routing = 0;

			if (sel)
				sscanf(sel, " sel=0x%x", &routing);
			shm_key(cls, sizeof(cls), routing);
			oracle_push(cls, (u16)addr, val);
		} else if (tbl_port(cls, (u16)addr)) {
			char key[32];

			tbl_key(key, sizeof(key), vendor_tbl_id, vendor_tbl_off);
			oracle_push(key, (u16)addr, val);
		} else if (!strcmp(cls, "PHY.WR"))
			tbl_note_addr(&vendor_tbl_id, &vendor_tbl_off,
				      (u16)addr, val);
		else if (strstr(cls, ".RD"))
			oracle_push(cls, (u16)addr, val);
		else if (!strcmp(cls, "PHY.MOD") || !strcmp(cls, "PHY.AND") ||
			 !strcmp(cls, "PHY.OR"))
			oracle_push("PHY.RD", (u16)addr, ORACLE_SHADOW);
	}
	fclose(f);
	fprintf(stderr, "b43-integration: oracolo da %s, %u chiavi\n",
		path, n_okeys);
}

static u32 oracle_lookup(const char *cls, u16 addr, int width)
{
	struct oracle_key *k;

	(void)width;
	if (!oracle_loaded)
		oracle_load();
	if (!n_okeys) {
		if (!oracle_absent_warned) {
			oracle_absent_warned = 1;
			fprintf(stderr,
				"b43-integration: nessun oracolo "
				"(B43_READ_ORACLE non impostato), ogni lettura "
				"ritorna 0. La traccia NON e' una misura del "
				"driver.\n");
		}
		return 0;
	}
	if (tbl_port(cls, addr)) {
		char key[32];

		tbl_key(key, sizeof(key), port_tbl_id, port_tbl_off);
		k = okey(key, addr, 0);
	} else
		k = okey(cls, addr, 0);
	if (!k || !k->n)
		return 0;
	if (k->written && oracle_is_latch(cls, addr))
		return k->last;
	while (k->pos < k->n) {
		u32 v = k->vals[k->pos++];

		if (v == ORACLE_SHADOW)
			return k->last;
		k->last = v;
		return v;
	}
	return k->last;	/* coda esaurita: l'ultimo visto */
}

/* Una scrittura del driver aggiorna l'ombra che i segnaposto rendono. */
static void oracle_note_write(const char *wr_cls, u16 addr, u32 val)
{
	char cls[32];
	struct oracle_key *k;
	size_t n = strlen(wr_cls);

	if (n < 3 || strcmp(wr_cls + n - 3, ".WR"))
		return;
	if (!strcmp(wr_cls, "PHY.WR"))
		tbl_note_addr(&port_tbl_id, &port_tbl_off, addr, val);
	snprintf(cls, sizeof(cls), "%.*s.RD", (int)(n - 3), wr_cls);
	k = okey(cls, addr, 0);
	if (k) {
		k->last = val;
		k->written = 1;
	}
}

/*
 * Marcatori di confine (B43_AC_FN e B43_AC_BLOCK in phy_ac.h), gli stessi che
 * emette ../unit e che fn_map.py legge. Servono a attribuire le op di questa
 * traccia alle funzioni del driver: senza, il confronto fra la sequenza di b43
 * intero e quella dell'harness si puo' allineare ma non spiegare.
 *
 * Silenziosi se B43_FN_MARKERS non e' nell'ambiente, perche' la traccia di
 * default e' un termine di confronto e i marcatori non sono op.
 */
static int fn_markers_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = getenv("B43_FN_MARKERS") ? 1 : 0;
	return enabled;
}

void b43_ac_fn_enter(const char *fn)
{
	if (fn_markers_enabled())
		fprintf(stream(), "----FN:%s----\n", fn);
}

void b43_ac_fn_leave(const char *fn)
{
	if (fn_markers_enabled())
		fprintf(stream(), "----/FN:%s----\n", fn);
}

void b43_ac_block_mark(const char *name)
{
	if (fn_markers_enabled())
		fprintf(stream(), "----BLK:%s----\n", name);
}

static FILE *timeline;

static FILE *timeline_file(void)
{
	const char *path = getenv("B43_TIMELINE");

	if (!timeline && path && *path) {
		timeline = fopen(path, "r");
		if (!timeline)
			fprintf(stderr, "b43-integration: B43_TIMELINE=%s non si "
				"apre\n", path);
	}
	return timeline;
}

int b43_test_timeline_wd(long long *first_us, long long *last_us, int *n)
{
	FILE *f = timeline_file();
	char line[128], kind[16];
	double t;

	*n = 0;
	if (!f)
		return 0;
	rewind(f);
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lf %*d %15s", &t, kind) != 2 ||
		    strcmp(kind, "WD"))
			continue;
		if (!(*n)++)
			*first_us = (long long)(t * 1e6 + 0.5);
		*last_us = (long long)(t * 1e6 + 0.5);
	}
	rewind(f);
	return *n;
}

int b43_test_timeline_next(long long *t_us, char *kind, int len,
			   u32 *arg0, u32 *arg1)
{
	FILE *f = timeline_file();
	char line[128], k[16];
	unsigned int a0 = 0, a1 = 0;
	double t;

	while (f && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lf %*d %15s %x %x", &t, k, &a0, &a1) < 2)
			continue;
		*t_us = (long long)(t * 1e6 + 0.5);
		snprintf(kind, len, "%s", k);
		*arg0 = a0;
		*arg1 = a1;
		return 1;
	}
	return 0;
}

/* Per i messaggi di main.c, che e' compilato senza stdio. */
void b43_trace_note(const char *fmt, int arg)
{
	fprintf(stderr, "b43-integration: ");
	fprintf(stderr, fmt, arg);
}

void b43_trace_op(const char *cls, u16 addr, u32 val, u16 mask, int has_mask)
{
	oracle_note_write(cls, addr, val);
	if (has_mask >= 0)
		fprintf(stream(), "cpu%d %-8s addr=0x%04x val=0x%04x mask=0x%04x\n",
			cpu, cls, addr, val, mask);
	else
		fprintf(stream(), "cpu%d %-8s addr=0x%04x val=0x%04x\n",
			cpu, cls, addr, val);
}

/*
 * MACCONTROL nella forma del vendor, che lo traccia come maskset a 32 bit
 * senza indirizzo. Sul bus e' una scrittura intera: la mask non c'e', e il
 * confronto a maschera di compare.py vincola i bit che il vendor ha toccato.
 */
void b43_trace_macctl(u32 val)
{
	fprintf(stream(), "cpu%d MAC.MCTRL val=0x%08x\n", cpu, val);
}

/* Maskset a 32 bit con indirizzo: i registri del PMU. */
void b43_trace_op32(const char *cls, u16 addr, u32 val, u32 mask)
{
	fprintf(stream(), "cpu%d %s addr=0x%x val=0x%08x mask=0x%08x\n",
		cpu, cls, addr, val, mask);
}

/* Le GPIO del chipcommon: registro implicito nella classe, come nel vendor. */
void b43_trace_gpio(const char *cls, u32 val, u32 mask)
{
	fprintf(stream(), "cpu%d %s val=0x%08x mask=0x%08x\n", cpu, cls, val, mask);
}

/*
 * La shared memory: b43 scrive (routing << 16) | offset nel registro di
 * controllo, quindi l'offset e il routing si separano qui. Il routing e' il
 * campo che le catture del d6220 non hanno -- il decoder di allora non lo
 * emetteva -- e che quelle del DSL chiamano `sel`.
 */
void b43_trace_shm(const char *cls, u32 routing_off, u32 val, int width)
{
	u16 routing = (u16)(routing_off >> 16);
	u16 off = (u16)(routing_off & 0xffff);

	fprintf(stream(), "cpu%d %-8s addr=0x%04x val=0x%0*x sel=0x%x\n",
		cpu, cls, off, width == 32 ? 8 : 4, val, routing << 16);
}

void b43_trace_raw(const char *cls, u16 off, u32 val, int width)
{
	fprintf(stream(), "cpu%d %-8s off=0x%04x val=0x%0*x\n",
		cpu, cls, off, width == 32 ? 8 : 4, val);
}

void b43_trace_block(const char *cls, u16 off, unsigned long count,
		     u8 reg_width)
{
	fprintf(stream(), "cpu%d %-8s addr=0x%04x len=%lu width=%u\n",
		cpu, cls, off, count, reg_width);
}

void b43_trace_fill(void *buf, unsigned long count, u16 off, u8 reg_width)
{
	(void)off;
	(void)reg_width;
	memset(buf, 0, count);
}

u32 b43_trace_read(const char *cls, u16 addr, int width)
{
	return oracle_lookup(cls, addr, width);
}

int b43_trace_has(const char *cls, u16 addr)
{
	if (!oracle_loaded)
		oracle_load();
	return okey(cls, addr, 0) != NULL;
}

u32 b43_trace_read_shm(u32 routing, u16 addr, int width)
{
	char cls[32];

	shm_key(cls, sizeof(cls), routing);
	return oracle_lookup(cls, addr, width);
}

void b43_trace_consume_shm_if(u32 routing, u16 addr, u32 val)
{
	struct oracle_key *k;
	char cls[32];

	if (!oracle_loaded)
		oracle_load();
	shm_key(cls, sizeof(cls), routing);
	k = okey(cls, addr, 0);
	if (k && k->pos < k->n && k->vals[k->pos] == val)
		k->last = k->vals[k->pos++];
}

/*
 * I file compilati con gli header del kernel non hanno stdlib: `-nostdinc` e
 * getenv() finisce dichiarata implicitamente, cioe' troncata a int. Per questo
 * l'ambiente si legge da qui, che e' codice utente.
 */
const char *b43_test_env(const char *name)
{
	const char *v = getenv(name);

	return (v && *v) ? v : NULL;
}

long b43_test_env_long(const char *name, long def)
{
	const char *v = getenv(name);
	char *end;
	long n;

	if (!v || !*v)
		return def;
	n = strtol(v, &end, 0);
	if (*end)
		return def;
	return n;
}

u32 b43_trace_read_raw(u16 off, int width)
{
	return oracle_lookup("REG.RD", off, width);
}
