// SPDX-License-Identifier: GPL-2.0
/*
 * Il bus BCMA, finto, che emette le op invece di toccare il silicio.
 *
 * L'aggancio non e' sui dieci simboli `bcma_*` che il link chiede -- quelli
 * sono controllo di clock, enable, GPIO e PLL. Le read e le write dei registri
 * passano per gli inline di <linux/bcma/bcma.h>, che dispacciano su
 * `core->bus->ops`, cioe' una vtable: fornire la nostra basta, e b43 non va
 * toccato.
 *
 * Il formato delle righe e' quello di ../unit/wrap.c, cosi' l'output e'
 * confrontabile con le catture del vendor dagli stessi strumenti
 * (reverse-tools/tracelib.py, unit/compare.py). Le classi sono quelle del
 * decoder: PHY.RD/WR/MOD, RAD.*, OBJ.*, MAC.*, TBL.*.
 *
 * Attenzione a cosa questo file NON fa: non modella l'hardware. Una lettura
 * ritorna quello che l'oracolo dice, e senza oracolo ritorna zero -- che per
 * un bit di stato auto-azzerante e' il valore che fa girare un poll fino al
 * timeout. E' lo stesso problema che in ../unit risolvono AC_READ_ORACLE e i
 * read plan, e qui si risolve nello stesso modo, non reinventandolo.
 */
#include <linux/bcma/bcma.h>
#include <linux/kernel.h>

#include "trace_out.h"

/*
 * I registri di b43 che ci interessano stanno nella finestra del core 802.11.
 * Le classi si ricavano dall'offset, come fa il decoder del vendor: il tracer
 * di wl aggancia le funzioni (wlc_bmac_write_objmem16 e compagnia) e ha i nomi
 * gia' fatti, noi abbiamo solo l'offset, quindi la traduzione sta qui.
 */
/*
 * Gli offset NON sono ridefiniti qui: vengono da b43.h, che il tree fetchato
 * contiene. Ridefinirli e' come li avevo scritti la prima volta, e
 * B43_MMIO_PHY_VER era 0x030 invece di 0x3E0 -- risultato, b43 leggeva zero
 * e diceva "FOUND UNSUPPORTED PHY (Analog 0, Type 0, Revision 0)".
 */
#include "b43.h"


/*
 * Lo stato che serve a ricostruire un'op da piu' accessi: b43 scrive prima
 * l'indirizzo in un registro di controllo e poi il dato in quello di dato,
 * quindi la riga si emette sul secondo accesso, con l'indirizzo del primo.
 * E' la stessa ricostruzione che fa il decoder sulle tracce del vendor.
 */
static u16 phy_addr;
static u16 radio_addr;
static u32 shm_routing_off;

/*
 * MACCONTROL non lo puo' servire l'oracolo: il tracer del vendor registra
 * l'argomento della maskset e non la rilettura, quindi le 137 MAC.MCTRL del
 * segmento di riferimento non hanno una sola RETVAL. Il valore lo determinano
 * per intero le scritture del driver -- b43 lo usa in read-modify-write, e
 * b43_validate_chipaccess() (main.c:3641) pretende di rileggere l'IHR_ENABLED
 * che b43_wireless_core_reset() ha appena messo. Quindi qui e' un latch, non
 * una lettura dalla cattura.
 */
static u32 macctl;

/*
 * Le cause d'interruzione che il microcodice alzerebbe: le mette main.c
 * quando la timeline consegna un evento, b43 le legge da
 * B43_MMIO_GEN_IRQ_REASON e le azzera con l'ACK, che su quel registro e' una
 * scrittura dei bit da togliere.
 */
static u32 irq_pending;

void b43_test_raise_irq(u32 reason)
{
	irq_pending |= reason;
}

/*
 * Le quattro celle di b43_validate_chipaccess(), e solo quelle.
 *
 * Sono UCODEREV, UCODEPATCH, UCODEDATE e UCODETIME (SHM_SHARED 0x0000-0x0006).
 * Finche' il PSM e' fermo sono RAM del driver: il self-test ci scrive i suoi
 * pattern e li rilegge, b43_upload_microcode() le azzera con il resto della
 * shared memory. Quando il PSM parte le riscrive il microcodice, e da li' in
 * poi il valore e' suo: lo serve l'oracolo, perche' qui il microcodice non
 * c'e'.
 *
 * Il modello non puo' essere l'oracolo nemmeno a PSM fermo. La meta' non
 * allineata del self-test usa 0x1122/0x3344/0x5566/0x7788, che `wl` non
 * esegue, e la versione 7.14.43 del driver stock non esegue affatto il test:
 * le code per indirizzo non hanno quelle letture, e servirle dalla cattura
 * darebbe al self-test i valori dell'ucode, cioe' un probe che fallisce.
 *
 * Una lettura servita dal modello consuma dalla coda dell'oracolo la voce in
 * testa solo se ha lo stesso valore: e' la stessa lettura fatta dal vendor
 * (il D6220 esegue la meta' allineata, #511-#522 del segmento di
 * riferimento). Una voce diversa e' di una lettura successiva e resta al suo
 * posto.
 *
 * Il write-through NON e' corretto in generale: le celle che aggiorna il ucode
 * -- la finestra statistiche 0x0300-0x0314, i contatori 0x0768-0x0788, il TSSI
 * -- il port le azzera e le rilegge, e servirle dal modello restituirebbe
 * zero al posto dei valori del vendor, cioe' farebbe passare il gate del
 * watchdog misurando niente. Per questo la lista e' corta.
 */
#define SHM_MODEL_CELLS	4		/* byte 0x0000, 0x0002, 0x0004, 0x0006 */

static u16 shm_model[SHM_MODEL_CELLS];
static bool shm_model_written[SHM_MODEL_CELLS];

static int shm_model_slot(u32 routing, u16 byte_off)
{
	if (routing != B43_SHM_SHARED || (byte_off & 1) ||
	    byte_off >= SHM_MODEL_CELLS * 2)
		return -1;
	return byte_off / 2;
}

static bool shm_model_active(void)
{
	return !(macctl & B43_MACCTL_PSM_RUN);
}

static void shm_model_store(u32 routing, u16 byte_off, u16 val)
{
	int slot = shm_model_slot(routing, byte_off);

	if (slot < 0 || !shm_model_active())
		return;
	shm_model[slot] = val;
	shm_model_written[slot] = true;
}

/* Ritorna true e riempie *val se la cella e' modellata e gia' scritta. */
static bool shm_model_load(u32 routing, u16 byte_off, u16 *val)
{
	int slot = shm_model_slot(routing, byte_off);

	if (slot < 0 || !shm_model_active() || !shm_model_written[slot])
		return false;
	*val = shm_model[slot];
	b43_trace_consume_shm_if(routing << 16, byte_off, *val);
	return true;
}

/*
 * L'indirizzo SHM che la traccia deve riportare e' l'offset in BYTE, che e'
 * cio' che il tracer del vendor prende dagli argomenti di wlc_bmac_read_shm.
 * Sul bus non passa quello: per B43_SHM_SHARED b43 mette nel registro di
 * controllo l'indice a 32 bit (offset >> 2) e distingue le due meta' con la
 * porta dati che usa -- B43_MMIO_SHM_DATA per la parola a byte 4w,
 * B43_MMIO_SHM_DATA_UNALIGNED per quella a 4w+2. Le altre routing (SCRATCH,
 * REGS, ...) b43 non le shifta, e la' l'indice e' gia' l'indirizzo.
 *
 * Senza questa ricostruzione ogni op OBJ della suite esce a offset/4 e non
 * combacia con nessuna op del vendor: il write-through degli host flag
 * usciva OBJ.WR addr=0x0017 dove la cattura ha addr=0x005e.
 */
static u16 shm_byte_off(u16 port)
{
	u16 routing = (u16)(shm_routing_off >> 16);
	u16 idx = (u16)(shm_routing_off & 0xffff);

	if (routing != B43_SHM_SHARED)
		return idx;
	return (u16)((idx << 2) + (port == B43_MMIO_SHM_DATA_UNALIGNED ? 2 : 0));
}

/*
 * Un accesso a 32 bit allineato e' UN accesso sul bus e DUE op per il vendor,
 * che legge e scrive la shared memory solo a 16 bit. La parola bassa
 * dell'offset porta la meta' ALTA del valore.
 *
 * Quell'ordine non e' una convenzione scelta qui, lo decide la cattura: sul
 * segmento di riferimento b43_validate_chipaccess() scrive 0x55aaaa55 e
 * rilegge, e il vendor fa lo stesso test alle op #511-#522 con 0x55aa a byte 0
 * e 0xaa55 a byte 2. Ricomporre nell'altro verso fa fallire il test contro i
 * valori che l'hardware ha davvero restituito. Lo confermano le celle
 * dell'ucoderev, 0x03a0 a byte 0 e 0x2715 a byte 2, che la revinfo da come
 * 0x03a02715.
 *
 * La seconda meta' del self-test, quella non allineata, il vendor non la
 * esegue: la servono le quattro celle modellate qui sopra. Vedi il commento
 * di shm_model_slot() per il perche' il write-through su quelle e' misurato e
 * perche' la lista e' corta.
 */
static void shm_word_write(u32 routing, u16 off, u16 val)
{
	shm_model_store(routing >> 16, off, val);
	b43_trace_shm("OBJ.WR", routing | off, val, 16);
}

static u16 shm_word_read(u32 routing, u16 off)
{
	u16 val;

	if (!shm_model_load(routing >> 16, off, &val))
		val = (u16)b43_trace_read_shm(routing, off, 16);
	b43_trace_shm("OBJ.RD", routing | off, val, 16);
	return val;
}

/*
 * Le altre routing -- la tabella di match degli indirizzi (RCMTA), gli
 * scratch -- non hanno la meta' a 16 bit: l'indice e' di una parola a 32 bit,
 * e il vendor al bus le legge e scrive cosi', una op per parola
 * (OBJ.WR sel=0x40000 addr=0x007e val=0x0102c000 sulla riga della stazione).
 */
static void shm_note_write(u16 port, u32 val, int width)
{
	u32 routing = shm_routing_off & 0xffff0000u;
	u16 off = shm_byte_off(port);

	if (width != 32) {
		shm_word_write(routing, off, (u16)val);
		return;
	}
	if (routing >> 16 != B43_SHM_SHARED) {
		b43_trace_shm("OBJ.WR", routing | off, val, 32);
		return;
	}
	shm_word_write(routing, off, (u16)(val >> 16));
	shm_word_write(routing, (u16)(off + 2), (u16)val);
}

static u32 shm_note_read(u16 port, int width)
{
	u32 routing = shm_routing_off & 0xffff0000u;
	u16 off = shm_byte_off(port);
	u32 hi;

	if (width != 32)
		return shm_word_read(routing, off);
	if (routing >> 16 != B43_SHM_SHARED) {
		u32 v = b43_trace_read_shm(routing, off, 32);

		b43_trace_shm("OBJ.RD", routing | off, v, 32);
		return v;
	}
	hi = shm_word_read(routing, off);
	return (hi << 16) | shm_word_read(routing, (u16)(off + 2));
}

/*
 * clk_ctl_st del core 802.11 (BCMA_CLKCTLST), che bcma_core_set_clockmode()
 * e bcma_core_pll_ctl() di drivers/bcma/core.c scrivono e poi interrogano
 * finche' lo stato non segue la richiesta: fino a 1500 e 10000 giri, cioe'
 * tutta la traccia se la lettura torna zero.
 *
 * La meta' bassa sono le richieste del driver e si rilegge come scritta. La
 * meta' alta e' lo stato, e il comportamento e' quello della cattura al bus
 * dell'agcombo (#119-#121, #19216-#19217): ALP e HT sono sempre disponibili,
 * il backplane gira su HT mentre FORCEHT e' alto e su ALP altrimenti, e le tre
 * risorse esterne sono su (EXTRESST = 7) anche senza richiesta, perche' le
 * tiene il PMU. Il valore iniziale delle richieste e' la prima lettura della
 * cattura quando c'e' (0x0040, HQCLKREQ, sull'agcombo), zero se no.
 */
static u32 clkctlst_req;
static bool clkctlst_init;

static u32 clkctlst_read(void)
{
	u32 v;

	if (!clkctlst_init) {
		clkctlst_init = true;
		if (b43_trace_has("REG.RD", BCMA_CLKCTLST))
			clkctlst_req = b43_trace_read_raw(BCMA_CLKCTLST, 32) &
				       0xffff;
	}
	v = clkctlst_req | BCMA_CLKCTLST_HAVEALP | BCMA_CLKCTLST_HAVEHT |
	    BCMA_CLKCTLST_EXTRESST;
	v |= (clkctlst_req & BCMA_CLKCTLST_FORCEHT) ? BCMA_CLKCTLST_BP_ON_HT
						    : BCMA_CLKCTLST_BP_ON_ALP;
	return v;
}

static void note_write(struct bcma_device *core, u16 off, u32 val, int width)
{
	switch (off) {
	case B43_MMIO_PHY_CONTROL:
		phy_addr = (u16)val;
		return;			/* meta' op: la riga esce col dato */
	case B43_MMIO_PHY_DATA:
		b43_trace_op("PHY.WR", phy_addr, val, 0, -1);
		return;
	case B43_MMIO_RADIO_CONTROL:
	case B43_MMIO_RADIO24_CONTROL:
		radio_addr = (u16)val;
		return;
	case B43_MMIO_RADIO_DATA_LOW:
	case B43_MMIO_RADIO24_DATA:
		b43_trace_op("RAD.WR", radio_addr, val, 0, -1);
		return;
	case B43_MMIO_SHM_CONTROL:
		shm_routing_off = val;
		return;
	case B43_MMIO_SHM_DATA:
	case B43_MMIO_SHM_DATA_UNALIGNED:
		shm_note_write(off, val, width);
		return;
	case B43_MMIO_MACCTL:
		macctl = val;
		b43_trace_macctl(val);
		return;
	case B43_MMIO_MACCMD:
		b43_trace_op("MAC.MCMD", 0, val, 0, -1);
		return;
	case B43_MMIO_GEN_IRQ_REASON:
		irq_pending &= ~val;
		b43_trace_raw("REG.WR", off, val, width);
		return;
	case BCMA_CLKCTLST:
		clkctlst_init = true;
		clkctlst_req = val & 0xffff;
		b43_trace_raw("REG.WR", off, val, width);
		return;
	default:
		b43_trace_raw("REG.WR", off, val, width);
		return;
	}
}

/*
 * La versione del PHY, che b43 legge a `b43_phy_versioning` (main.c:4484) e
 * decodifica in analog_type, phy_type e phy_rev secondo i campi di
 * phy_common.h:30. Da phy_type dipende TUTTO il resto: la scelta della
 * vtable, e la selezione del firmware, che per corerev 42 prende `ucode42`
 * solo `if (phy->type == B43_PHYTYPE_AC)`. Senza questo valore phy_type e'
 * zero, b43 non trova un ucode e la probe ritorna -EOPNOTSUPP prima di
 * emettere una sola op.
 *
 * Non e' una costante inventata: `wl1_revinfo.txt` del d6220 da phytype 0xb
 * e phyrev 0x1, cioe' AC rev1.
 */
#define D6220_PHY_ANALOG	0x0		/* campo analog, dai dump */
#define D6220_PHY_TYPE		0x0b		/* B43_PHYTYPE_AC */
#define D6220_PHY_REV		0x01

/* Da wl1_revinfo.txt: radiorev 0x42069. */
#define D6220_RADIO_ID		0x2069
#define D6220_RADIO_REV		0x4

static u32 phy_version_word(void)
{
	return ((D6220_PHY_ANALOG << 12) & 0xf000) |
	       ((D6220_PHY_TYPE << 8) & 0x0f00) |
	       (D6220_PHY_REV & 0x00ff);
}

static u32 note_read(struct bcma_device *core, u16 off, int width)
{
	u32 v;

	switch (off) {
	case B43_MMIO_PHY_VER:
		v = phy_version_word();
		break;
	/*
	 * L'identita' della radio, che b43 legge a `b43_radio_versioning`
	 * (main.c:4679) per corerev 40/42: scrive 0 in RADIO24_CONTROL e la
	 * lettura successiva da' radio_rev, scrive 1 e da' radio_id. Poi
	 * pretende radio_manuf 0x17F -- che per quel ramo b43 mette da se' --
	 * e per l'AC un radio_id 0x2069, o `unsupported`.
	 *
	 * I valori vengono da `wl1_revinfo.txt` del d6220: `radiorev 0x42069`,
	 * cioe' id 0x2069 e revisione 4 -- il 2069 rev4, lo stesso di
	 * reverse-tools/extract_chan_tuning_2069rev4.py.
	 */
	case B43_MMIO_RADIO24_DATA:
		if (radio_addr == 1)
			return D6220_RADIO_ID;
		if (radio_addr == 0)
			return D6220_RADIO_REV;
		fallthrough;
	case B43_MMIO_RADIO_DATA_LOW:
		v = b43_trace_read("RAD.RD", radio_addr, width);
		b43_trace_op("RAD.RD", radio_addr, v, 0, -1);
		return v;
	case B43_MMIO_PHY_DATA:
		v = b43_trace_read("PHY.RD", phy_addr, width);
		b43_trace_op("PHY.RD", phy_addr, v, 0, -1);
		return v;
	case B43_MMIO_SHM_DATA:
	case B43_MMIO_SHM_DATA_UNALIGNED:
		return shm_note_read(off, width);
	case B43_MMIO_MACCTL:
		v = macctl;
		break;
	/*
	 * An address register reads back what was written to it. The read
	 * after every address write -- b43_flush_writes() on these hosts, and
	 * the stock driver on the AC cores -- is the port's own address, not a
	 * value to take from the capture: served from the oracle's queue, it
	 * is the stock driver's address at that point in its sequence, and
	 * wrong as soon as the two sequences part.
	 */
	case B43_MMIO_PHY_CONTROL:
		v = phy_addr;
		break;
	case B43_MMIO_RADIO_CONTROL:
	case B43_MMIO_RADIO24_CONTROL:
		v = radio_addr;
		break;
	case B43_MMIO_SHM_CONTROL:
		v = shm_routing_off;
		break;
	case BCMA_CLKCTLST:
		v = clkctlst_read();
		break;
	/*
	 * La risposta del microcodice. b43_upload_microcode() (main.c:2790)
	 * avvia il PSM e poi aspetta B43_IRQ_MAC_SUSPENDED qui, venti giri e
	 * poi "Microcode not responding"; b43_mac_suspend() aspetta lo stesso
	 * bit.
	 *
	 * L'oracolo non lo puo' servire. Le catture wl-diag non hanno le
	 * letture MMIO grezze; quelle prese al bus le hanno, ma sono le
	 * risposte a un'altra sequenza di eventi, e servite in coda darebbero
	 * a b43 le cause di interruzione di wl fuori dal loro contesto. Il
	 * valore lo produce il microcodice, e il microcodice qui e' lo shim --
	 * il blob di kernel_shim.c e' un header valido senza codice dentro,
	 * quindi il PSM non partirebbe mai. Rispondere e' il modello, non
	 * un'invenzione: senza, ogni fase a MAC sospeso e' irraggiungibile.
	 *
	 * Non e' un latch come MACCONTROL: b43 scrive qui per fare l'ACK
	 * degli interrupt, e un latch restituirebbe l'ACK invece dello stato.
	 */
	case B43_MMIO_GEN_IRQ_REASON:
		v = B43_IRQ_MAC_SUSPENDED | irq_pending;
		break;
	default:
		v = b43_trace_read_raw(off, width);
		break;
	}
	/*
	 * Un registro del MAC, non una porta: sul bus e' una lettura a se', e
	 * una cattura presa li' la porta come REG.RD.
	 */
	b43_trace_raw("REG.RD", off, v, width);
	return v;
}

static u8 stub_read8(struct bcma_device *core, u16 off)
{
	return (u8)note_read(core, off, 8);
}

static u16 stub_read16(struct bcma_device *core, u16 off)
{
	return (u16)note_read(core, off, 16);
}

static u32 stub_read32(struct bcma_device *core, u16 off)
{
	return note_read(core, off, 32);
}

static void stub_write8(struct bcma_device *core, u16 off, u8 val)
{
	note_write(core, off, val, 8);
}

static void stub_write16(struct bcma_device *core, u16 off, u16 val)
{
	note_write(core, off, val, 16);
}

static void stub_write32(struct bcma_device *core, u16 off, u32 val)
{
	note_write(core, off, val, 32);
}

/*
 * I blocchi: b43 li usa per le tabelle e per la template RAM. Il vendor li
 * traccia come un'intestazione (TBL.WR, OBJ.BULKW) seguita dalle op singole,
 * e qui si fa lo stesso, cosi' le due tracce si allineano.
 */
static void stub_block_read(struct bcma_device *core, void *buf, size_t count,
			    u16 off, u8 reg_width)
{
	b43_trace_block("OBJ.BULKR", off, count, reg_width);
	b43_trace_fill(buf, count, off, reg_width);
}

static void stub_block_write(struct bcma_device *core, const void *buf,
			     size_t count, u16 off, u8 reg_width)
{
	b43_trace_block("OBJ.BULKW", off, count, reg_width);
}

/*
 * Lo spazio "agent" del core, il wrapper, che e' una vtable a parte:
 * `bcma_aread32` non passa dalle read32 qui sopra. Ci passano il reset e i
 * clock del core -- bcma_core_enable/disable, compilati dalla suite com'e'
 * in drivers/bcma/core.c -- e i bit PHY di IOCTL che b43 tocca da se'. Sul
 * bus e' la finestra 0x1000-0x1fff, che reverse-tools/mmio2ops.py riporta
 * come WRAP.RD/WR con l'offset nel wrapper: qui si traccia con lo stesso
 * nome.
 *
 * IOCTL e RESET_CTL sono registri di controllo e si rileggono come scritti:
 * un latch. RESET_ST e' lo stato di una transazione di reset in corso, e fuori
 * da una transazione vale zero, che e' cio' che bcma_core_disable() aspetta.
 *
 * IOST lo serve l'oracolo quando la cattura e' presa al bus (0x100c
 * sull'agcombo: DMA64, FASTCLKA, DUALB). Le catture wl-diag non hanno lo
 * spazio agent, e allora vale la costante del D6220, dove wl1 e' il core
 * 5 GHz: da quella risposta b43_wireless_core_attach() (main.c:5393) ricava
 * have_2ghz_phy e have_5ghz_phy.
 */
/* I valori sono quelli di b43.h:509, non inventati qui. */
#define B43_BCMA_IOST_2G_PHY	0x00000001	/* 2.4G capable phy */
#define B43_BCMA_IOST_5G_PHY	0x00000002	/* 5G capable phy */

static u32 agent_ioctl, agent_resetctl;

static u32 agent_read(u16 off)
{
	switch (off) {
	case BCMA_IOCTL:
		return agent_ioctl;
	case BCMA_RESET_CTL:
		return agent_resetctl;
	case BCMA_IOST:
		if (b43_trace_has("WRAP.RD", off))
			return b43_trace_read("WRAP.RD", off, 32);
		return B43_BCMA_IOST_5G_PHY;
	case BCMA_RESET_ST:
		return 0;
	default:
		return b43_trace_read("WRAP.RD", off, 32);
	}
}

static u32 stub_aread32(struct bcma_device *core, u16 off)
{
	u32 v = agent_read(off);

	b43_trace_raw("WRAP.RD", off, v, 32);
	return v;
}

static void stub_awrite32(struct bcma_device *core, u16 off, u32 val)
{
	if (off == BCMA_IOCTL)
		agent_ioctl = val;
	else if (off == BCMA_RESET_CTL)
		agent_resetctl = val;
	b43_trace_raw("WRAP.WR", off, val, 32);
}

const struct bcma_host_ops b43_test_bcma_ops = {
	.aread32	= stub_aread32,
	.awrite32	= stub_awrite32,
	.read8		= stub_read8,
	.read16		= stub_read16,
	.read32		= stub_read32,
	.write8		= stub_write8,
	.write16	= stub_write16,
	.write32	= stub_write32,
	.block_read	= stub_block_read,
	.block_write	= stub_block_write,
};
