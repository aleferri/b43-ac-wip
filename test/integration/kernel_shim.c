// SPDX-License-Identifier: GPL-2.0
/*
 * Gli shim del kernel: quello che serve perche' gli oggetti di b43 linkino
 * in userspace.
 *
 * Codice UTENTE, come trace_out.c: non e' codice di driver, e gli servono
 * malloc e printf. Le firme non combaciano con quelle del kernel e non
 * devono: il link in C risolve per nome, e questi simboli b43 li chiama ma
 * non ne usa il valore in nessun modo che conti per la traccia delle op.
 *
 * Quello che NON e' un no-op, ed e' scritto qui perche' altrimenti la suite
 * gira e mente:
 *
 *   - i lock non serializzano niente: la suite e' a un thread, e se un giorno
 *     non lo fosse la traccia non sarebbe piu' ordinata e il confronto
 *     posizionale non avrebbe senso.
 *   - i delay non aspettano. Va bene per la traccia, che e' una sequenza di
 *     op e non un profilo temporale, ma vuol dire che un poll su un bit che
 *     l'hardware alza dopo N microsecondi qui gira a vuoto: e' l'oracolo a
 *     dover rispondere, non il tempo.
 *   - request_firmware RIESCE, con un blob minimo valido: se fallisce,
 *     b43_chip_init ritorna errore e b43_phy_init non viene mai chiamata.
 *     L'ucode vero non serve, e dev->fw.rev nemmeno si inietta: b43 lo legge
 *     dalla shared memory, quindi lo serve l'oracolo. Con B43_FW_DIR i
 *     file si leggono da li'. Vedi il commento sul firmware piu' sotto.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Il debug dinamico del kernel: muto. */
void __dynamic_pr_debug(void *descriptor, const char *fmt, ...) { }
void __dynamic_dev_dbg(void *descriptor, const void *dev, const char *fmt, ...) { }

/* --- diagnostica ------------------------------------------------------- */

/*
 * NON muto: b43err e b43info passano da qui, e sono il modo in cui b43 dice
 * quale gate lo ha respinto. Con un _printk vuoto la probe ritorna -95 e
 * l'unica via per sapere perche' e' il debugger -- che e' come ho perso due
 * giri. Va su stderr, cosi' la traccia su stdout resta confrontabile.
 */
/*
 * b43err e compagnia usano %pV, il formato ricorsivo del kernel: l'argomento
 * e' una struct va_format col vero fmt e i suoi argomenti dentro. Un
 * vfprintf normale stampa il puntatore e il messaggio si perde, che e'
 * esattamente cio' che mi ha fatto perdere due giri.
 */
struct shim_va_format {
	const char *fmt;
	va_list *va;
};

int _printk(const char *fmt, ...)
{
	const char *pv = strstr(fmt, "%pV");
	va_list ap;

	va_start(ap, fmt);
	if (pv) {
		/* la parte prima di %pV, poi il messaggio annidato */
		char head[256];
		size_t n = (size_t)(pv - fmt);
		struct shim_va_format *v;

		if (n >= sizeof(head))
			n = sizeof(head) - 1;
		memcpy(head, fmt, n);
		head[n] = 0;
		vfprintf(stderr, head, ap);
		v = va_arg(ap, struct shim_va_format *);
		if (v && v->fmt)
			vfprintf(stderr, v->fmt, *v->va);
	} else {
		vfprintf(stderr, fmt, ap);
	}
	va_end(ap);
	return 0;
}

void __warn_printk(const char *fmt, ...)
{
}

/*
 * dev_err/dev_warn/dev_info, che bcma usa per i suoi messaggi: stesso
 * canale di _printk. Il device non si stampa, la suite ne ha uno solo.
 */
static void shim_dev_printk(const char *level, const char *fmt, va_list ap)
{
	fprintf(stderr, "%s: ", level);
	vfprintf(stderr, fmt, ap);
}

void _dev_err(const void *dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	shim_dev_printk("err", fmt, ap);
	va_end(ap);
}

void _dev_warn(const void *dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	shim_dev_printk("warn", fmt, ap);
	va_end(ap);
}

void _dev_info(const void *dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	shim_dev_printk("info", fmt, ap);
	va_end(ap);
}

void fortify_panic(const char *name)
{
	fprintf(stderr, "b43-integration: fortify_panic in %s\n", name);
	abort();
}

/*
 * The PHY rate of a data frame's TX descriptor. No timeline sends one, so
 * a run that gets here has outgrown the shim and needs the real function.
 */
unsigned int cfg80211_calculate_bitrate(void *rate)
{
	fprintf(stderr, "b43-integration: cfg80211_calculate_bitrate not shimmed\n");
	abort();
}

int net_ratelimit(void)
{
	return 1;
}

/* --- lock: la suite e' a un thread ------------------------------------ */

void _raw_spin_lock(void *l) { }
void _raw_spin_unlock(void *l) { }
void _raw_spin_lock_irq(void *l) { }
void _raw_spin_unlock_irq(void *l) { }
unsigned long _raw_spin_lock_irqsave(void *l) { return 0; }
void _raw_spin_unlock_irqrestore(void *l, unsigned long f) { }
void __mutex_init(void *l, const char *n, void *k) { }
void mutex_lock(void *l) { }
void mutex_unlock(void *l) { }
int mutex_is_locked(void *l) { return 0; }

/* --- tempo: non si aspetta, vedi il commento in testa ----------------- */

unsigned long jiffies;
void __const_udelay(unsigned long x) { }
void __udelay(unsigned long x) { }
void msleep(unsigned int m) { }
void usleep_range_state(unsigned long a, unsigned long b, unsigned int s) { }
unsigned long round_jiffies(unsigned long j) { return j; }
unsigned long round_jiffies_relative(unsigned long j) { return j; }
void init_timer_key(void *t, void *f, unsigned int fl, const char *n, void *k) { }
void delayed_work_timer_fn(void *t) { }
void __SCT__might_resched(void) { }
void __init_swait_queue_head(void *q, const char *n, void *k) { }

/* --- lavoro differito: eseguito subito o ignorato --------------------- */

void *system_wq;
/* queue_work_on() sta in subsystem_stub.c: ha bisogno di struct
 * work_struct, e questo file e' codice utente senza header kernel. */
int cancel_work_sync(void *w) { return 0; }
/* Il registro dei delayed work sta in subsystem_stub.c, coi tipi del kernel. */
int b43_test_dwork_cancel(void *w);
int cancel_delayed_work_sync(void *w) { return b43_test_dwork_cancel(w); }
void complete(void *c) { }
void wait_for_completion(void *c) { }

/* --- memoria ---------------------------------------------------------- */

/* Nel kernel e' bidimensionale: [NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH+1].
 * Sottodimensionarlo fa indicizzare fuori dall oggetto. */
void *kmalloc_caches[8][64];
unsigned long random_kmalloc_seed;

void *kmalloc_trace(void *s, unsigned int flags, unsigned long size)
{
	return calloc(1, size ? size : 1);
}

/* Le allocazioni grandi non passano dalle cache: kmalloc() dispaccia qui. */
void *__kmalloc(unsigned long size, unsigned int flags)
{
	return calloc(1, size ? size : 1);
}

void kfree(const void *p)
{
	free((void *)p);
}

/*
 * L'ultimo gruppo, che compare solo dopo che si compila il nostro phy_ac.c:
 * `int_sqrt` la usa il solve della RX IQ, e NON e' un no-op -- ritornare zero
 * falserebbe i coefficienti in silenzio. Le altre sono contorno.
 */
unsigned long int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;

	if (x <= 1)
		return x;
	m = 1UL << ((8 * sizeof(y) - 2) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return y;
}

/*
 * Chiamata da un inline asm degli header (arch/x86/include/asm/arch_hweight.h)
 * che dichiara solo %rdi in ingresso e %rax in uscita: e' il chiamante ad
 * assumere che ogni altro registro sopravviva, perche' la versione del kernel
 * (arch/x86/lib/hweight.S) li preserva. Una funzione C qui non puo' onorare
 * quel contratto, e con -O2 il puntatore che il chiamante teneva in %rdx o
 * %rcx viene sovrascritto: la corruzione emerge migliaia di op dopo, come
 * free() su un puntatore invalido o SEGV su dev->phy.ac. Stessa sequenza
 * dell'originale, stesso vincolo sui registri.
 */
asm(
"	.text\n"
"	.globl __sw_hweight32\n"
"	.type __sw_hweight32, @function\n"
"__sw_hweight32:\n"
"	pushq	%rdi\n"
"	pushq	%rdx\n"
"	movl	%edi, %edx\n"
"	shrl	%edx\n"
"	andl	$0x55555555, %edx\n"
"	subl	%edx, %edi\n"
"	movl	%edi, %edx\n"
"	andl	$0x33333333, %edx\n"
"	shrl	$2, %edi\n"
"	andl	$0x33333333, %edi\n"
"	addl	%edx, %edi\n"
"	movl	%edi, %edx\n"
"	shrl	$4, %edx\n"
"	addl	%edx, %edi\n"
"	andl	$0x0f0f0f0f, %edi\n"
"	imull	$0x01010101, %edi, %edi\n"
"	shrl	$24, %edi\n"
"	movl	%edi, %eax\n"
"	popq	%rdx\n"
"	popq	%rdi\n"
"	ret\n"
"	.size __sw_hweight32, .-__sw_hweight32\n"
);

/*
 * Chiamata dal wrapper inline find_next_bit() di include/linux/find.h ogni
 * volta che size non e' una costante di compilazione -- il caso di
 * dev->phy.ac->num_cores nei for_each_set_bit() del port. Non e' un no-op:
 * un valore inventato sposterebbe le operazioni sul core sbagliato, lo stesso
 * modo di sbagliare di uno stub a zero. Stessa semantica della versione del
 * kernel (lib/find_bit.c), un bit alla volta invece che una parola alla
 * volta: qui conta la correttezza, non la velocita'.
 */
unsigned long _find_next_bit(const unsigned long *addr1, unsigned long nbits,
			      unsigned long start)
{
	unsigned long i;

	for (i = start; i < nbits; i++)
		if (addr1[i / (8 * sizeof(unsigned long))] &
		    (1UL << (i % (8 * sizeof(unsigned long)))))
			return i;
	return nbits;
}

void __local_bh_enable_ip(unsigned long ip, unsigned int cnt) { }
char pcpu_hot[256];

unsigned long strnlen(const char *s, unsigned long n)
{
	unsigned long i = 0;

	while (i < n && s[i])
		i++;
	return i;
}

/* Il contratto del kernel: -E2BIG (-7) se tronca, altrimenti la lunghezza. */
long strscpy(char *dst, const char *src, unsigned long n)
{
	unsigned long len = strnlen(src, n);

	if (!n)
		return -7;
	if (len == n) {
		len = n - 1;
		memcpy(dst, src, len);
		dst[len] = 0;
		return -7;
	}
	memcpy(dst, src, len + 1);
	return (long)len;
}

/* --- firmware: RIESCE, con un blob minimo valido ------------------------
 *
 * Deve riuscire: se `request_firmware` fallisce, `b43_upload_microcode` esce,
 * `b43_chip_init` ritorna errore e `b43_wireless_core_init` non arriva mai a
 * `b43_phy_init` -- cioe' salta esattamente cio' che questa suite deve
 * misurare.
 *
 * Non serve un ucode vero. Serve un blob che passi la validazione di
 * `b43_do_request_fw` (main.c:2188): almeno `sizeof(struct b43_fw_header)`
 * byte, `type` fra UCODE/PCM/IV, `ver == 1`, e per UCODE e PCM `be32(size)`
 * uguale ai byte che seguono l'header.
 *
 * E NON serve iniettare `dev->fw.rev`: `b43_upload_microcode` lo legge dalla
 * shared memory -- `B43_SHM_SH_UCODEREV` a 0x0000, `UCODEPATCH` a 0x0002 --
 * quindi lo serve l'ORACOLO. Nel segmento di riferimento del d6220 quelle due
 * celle valgono 0x03a0 e 0x2715, che ricomposte danno l'`ucoderev 0x3a02715`
 * della revinfo. Il gate duro `if (fwrev <= 0x128) goto error` passa con
 * 0x3a0 = 928, e `fw.rev >= 598` porta sul ramo nuovo: sono i gate che
 * decidono meta del bring-up, e vengono dai dati, non da una costante qui.
 */
#define FW_HDR_LEN	8		/* type, ver, 2 pad, be32 size */
#define FW_PAYLOAD	64

struct shim_firmware {			/* prefisso di struct firmware */
	unsigned long size;
	const unsigned char *data;
	void *priv;
};

/*
 * Un blob PER TIPO, non uno solo condiviso.
 *
 * b43 tiene quattro firmware vivi insieme -- dev->fw.ucode, .pcm, .initvals,
 * .initvals_band -- e li usa dopo, non al momento della richiesta: con un
 * unico blob statico l'ultima richiesta sovrascriverebbe le altre e
 * b43_upload_microcode() leggerebbe l'header di una lista di initval. Gli IV
 * ne condividono uno perche' le due richieste sono dello stesso tipo e con
 * zero voci.
 */
enum { SHIM_FW_UCODE, SHIM_FW_PCM, SHIM_FW_IV, SHIM_FW_SLOTS };

static struct shim_firmware shim_fw[SHIM_FW_SLOTS];
static unsigned char shim_fw_blob[SHIM_FW_SLOTS][FW_HDR_LEN + FW_PAYLOAD];

static struct shim_firmware *shim_fw_build(const char *name)
{
	unsigned char type = 'u';	/* B43_FW_TYPE_UCODE */
	int slot = SHIM_FW_UCODE;
	unsigned char *blob;

	if (strstr(name, "pcm")) {
		type = 'p';
		slot = SHIM_FW_PCM;
	} else if (strstr(name, "initval") || strstr(name, "_iv")) {
		type = 'i';
		slot = SHIM_FW_IV;
	}

	blob = shim_fw_blob[slot];
	memset(blob, 0, FW_HDR_LEN + FW_PAYLOAD);
	blob[0] = type;
	blob[1] = 1;			/* ver: l'unica accettata */
	/* be32(size) = i byte che seguono l'header. Per gli IV e' il numero
	 * di IV, e zero e' valido: una lista vuota non emette op. */
	if (type == 'i') {
		shim_fw[slot].size = FW_HDR_LEN;
	} else {
		blob[4] = (FW_PAYLOAD >> 24) & 0xff;
		blob[5] = (FW_PAYLOAD >> 16) & 0xff;
		blob[6] = (FW_PAYLOAD >> 8) & 0xff;
		blob[7] = FW_PAYLOAD & 0xff;
		shim_fw[slot].size = FW_HDR_LEN + FW_PAYLOAD;
	}
	shim_fw[slot].data = blob;
	return &shim_fw[slot];
}

/*
 * Il firmware vero, se B43_FW_DIR lo indica: `name` e' il percorso che b43
 * chiede a /lib/firmware ("b43/ucode42.fw"), qui relativo a quella cartella.
 * reverse-tools/fw_from_capture.py la ricava da una cattura al bus del `up`
 * del driver stock, che scrive ucode e initvals dalle stesse porte di b43:
 * con il blob minimo gli initvals sono vuoti e lo stato finale del core non
 * si puo' confrontare con quello della cattura. Un file che manca nella
 * cartella prende il blob minimo, come senza B43_FW_DIR.
 */
static struct shim_firmware *shim_fw_load(const char *name)
{
	const char *dir = getenv("B43_FW_DIR");
	struct shim_firmware *fw;
	char path[512];
	unsigned char *data;
	long len;
	FILE *f;

	if (!dir || !*dir)
		return NULL;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	fw = calloc(1, sizeof(*fw));
	data = malloc(len > 0 ? len : 1);
	if (!fw || !data || fread(data, 1, len, f) != (size_t)len) {
		fclose(f);
		free(fw);
		free(data);
		return NULL;
	}
	fclose(f);
	fw->size = len;
	fw->data = data;
	return fw;
}

static struct shim_firmware *shim_fw_get(const char *name)
{
	struct shim_firmware *fw = shim_fw_load(name);

	return fw ? fw : shim_fw_build(name);
}

int request_firmware(const void **fw, const char *name, void *dev)
{
	*fw = shim_fw_get(name);
	return 0;
}

/*
 * La via che b43 usa davvero, e per un po' questa ritornava -2.
 *
 * b43_do_request_fw() (main.c:2364) chiama la nowait e, se ritorna negativo,
 * stampa "Unable to load firmware" e **ritorna l'errore**: il commento sul
 * fall-through al percorso sincrono vale solo per un esito >= 0. Quindi con
 * -2 il firmware non si caricava, dev->fw.ucode.data restava nullo e
 * b43_upload_microcode() moriva dereferenziandolo a main.c:2761.
 *
 * Qui si chiama la continuazione in linea, che e' b43_fw_cb(): mette il blob
 * in ctx->blob e completa. Poi wait_for_completion() ritorna subito -- nello
 * shim non aspetta -- e b43 trova ctx->blob e prosegue. E' lo stesso
 * ragionamento dei work item in subsystem_stub.c: la suite e' a un thread, e
 * un caricamento davvero asincrono renderebbe la traccia non ordinata.
 */
int request_firmware_nowait(void *mod, int uevent, const char *name,
			    void *dev, unsigned int gfp, void *ctx, void *cont)
{
	void (*cb)(const void *fw, void *context) = cont;

	if (!cb)
		return -22;
	cb(shim_fw_get(name), ctx);
	return 0;
}

void release_firmware(const void *fw) { }

/* --- interruzioni, skb, rng, rfkill: niente di tutto questo emette op -- */

/*
 * I due handler di b43, per consegnare le interruzioni che la timeline porta:
 * il campione di rumore pronto. L'hard handler legge la causa dal registro,
 * e la causa la serve bcma_stub.c; se chiede il thread, il thread segue
 * subito, come fa il kernel su una sola CPU.
 */
static int (*irq_hard)(int, void *);
static int (*irq_thread)(int, void *);
static void *irq_dev;

int request_threaded_irq(unsigned int irq, void *h, void *th,
			 unsigned long flags, const char *name, void *dev)
{
	irq_hard = (int (*)(int, void *))h;
	irq_thread = (int (*)(int, void *))th;
	irq_dev = dev;
	return 0;
}

#define SHIM_IRQ_WAKE_THREAD	2

void b43_test_irq(void)
{
	if (irq_hard && irq_hard(0, irq_dev) == SHIM_IRQ_WAKE_THREAD &&
	    irq_thread)
		irq_thread(0, irq_dev);
}

void free_irq(unsigned int irq, void *dev)
{
	irq_hard = NULL;
	irq_thread = NULL;
}
void dev_kfree_skb_any_reason(void *skb, int reason) { }
/* L'unico skb che b43 clona e' il beacon, che subsystem_stub.c tiene fermo. */
void *skb_clone(void *skb, unsigned int gfp) { return skb; }
void *skb_dequeue(void *list) { return NULL; }
void skb_queue_head(void *list, void *skb) { }
void skb_queue_tail(void *list, void *skb) { }
int hwrng_register(void *rng) { return 0; }
void hwrng_unregister(void *rng) { }
void wiphy_rfkill_start_polling(void *wiphy) { }

/* --- registrazione dei driver di bus ---------------------------------- */

/*
 * Non registriamo niente: catturiamo la probe. `b43_bcma_probe` e' statica,
 * quindi l'unico modo di chiamarla senza toccare b43 e' prenderla dalla
 * struttura che b43 stesso passa qui. Gli offset sono quelli di
 * `struct bcma_driver`: name, id_table, probe, remove.
 */
int (*b43_test_probe)(void *core);
void (*b43_test_remove)(void *core);

struct shim_bcma_driver {
	const char *name;
	const void *id_table;
	int (*probe)(void *core);
	void (*remove)(void *core);
};

int __bcma_driver_register(void *drv, void *owner)
{
	struct shim_bcma_driver *d = drv;

	b43_test_probe = d->probe;
	b43_test_remove = d->remove;
	return 0;
}
void bcma_driver_unregister(void *drv) { }
int __ssb_driver_register(void *drv, void *owner) { return 0; }

/* --- parametri del modulo --------------------------------------------- */

char __this_module[512];
char param_ops_int[64];
char param_ops_string[64];

#ifdef __SANITIZE_ADDRESS__
/*
 * Gli header del kernel, quando vedono __SANITIZE_ADDRESS__, rimappano
 * memcpy/memset/memmove sulle varianti KASAN, che qui non esistono. Sono tre
 * alias sui builtin: bastano a far linkare la build ASAN=1, che e' il solo
 * modo di far dire al sanitizer *chi* corrompe la memoria invece di guardare
 * il sintomo -- una free() invalida in un build e un SIGSEGV in un altro.
 */
void *__hwasan_memcpy(void *d, const void *s, size_t n);
void *__hwasan_memset(void *d, int c, size_t n);
void *__hwasan_memmove(void *d, const void *s, size_t n);

void *__hwasan_memcpy(void *d, const void *s, size_t n)
{
	return __builtin_memcpy(d, s, n);
}

void *__hwasan_memset(void *d, int c, size_t n)
{
	return __builtin_memset(d, c, n);
}

void *__hwasan_memmove(void *d, const void *s, size_t n)
{
	return __builtin_memmove(d, s, n);
}
#endif
