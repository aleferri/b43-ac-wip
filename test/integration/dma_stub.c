// SPDX-License-Identifier: GPL-2.0
/*
 * Quel che serve a dma.c per girare qui: memoria per gli anelli, skb per i
 * buffer di ricezione, e un mapping DMA finto. Compilato coi flag del kernel
 * perche' gli skb sono `struct sk_buff` veri: dma.c li riempie con skb_put e
 * legge l'intestazione RX da skb->data.
 *
 * Nessuna periferica scrive in questa memoria. I descrittori che dma.c
 * compila restano come li lascia, e l'intestazione dei buffer RX resta il
 * poison che ci mette lui: frame_len = 0. Cosi' quando la timeline consegna
 * un RX_DONE, b43_dma_rx() legge dal registro di stato lo slot corrente --
 * quello lo serve l'oracolo, dalla cattura -- percorre gli slot fino a li',
 * trova la lunghezza a zero, scarta e ricicla il buffer, e riscrive
 * l'indice: le stesse op di registro che il vendor emette per un frame, con
 * la geometria dell'anello di b43.
 *
 * Gli indirizzi DMA sono pseudo-fisici, distribuiti da un contatore che parte
 * dove la cattura dell'agcombo ha il primo anello (0x00c18000) e allineati
 * alla dimensione chiesta, come fa dma_alloc_coherent() con le potenze di
 * due: il valore non lo legge nessuno tranne i registri di base degli anelli,
 * che cosi' portano numeri dell'ordine di quelli del vendor e l'allineamento
 * che dma.c pretende dall'allocatore.
 */
#include <linux/bcma/bcma.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/mac80211.h>

#include "trace_out.h"

/*
 * Le costanti che virt_to_page() e __pa() usano in linea negli header di
 * x86. Con tutte e tre a zero __pa(x) == x e la pagina e' x / 64: qui si
 * vuole solo che il calcolo non tocchi memoria, non che dia una pagina vera.
 */
unsigned long page_offset_base;
unsigned long phys_base;
unsigned long vmemmap_base;

bool is_vmalloc_addr(const void *x) { return false; }

/* --- memoria coerente degli anelli --------------------------------------- */

#define SHIM_DMA_BASE	0x00c18000ull

static dma_addr_t shim_dma_next = SHIM_DMA_BASE;

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle,
		      gfp_t gfp, unsigned long attrs)
{
	size_t align = roundup_pow_of_two(size);
	void *p = kzalloc(size + align, gfp);
	uintptr_t a;

	if (!p)
		return NULL;
	a = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
	*handle = ALIGN(shim_dma_next, align);
	shim_dma_next = *handle + align;
	return (void *)a;
}

void dma_free_attrs(struct device *dev, size_t size, void *vaddr,
		    dma_addr_t handle, unsigned long attrs)
{
	/* La base non e' recuperabile dall'indirizzo allineato: resta. */
}

int dma_set_mask(struct device *dev, u64 mask) { return 0; }
int dma_set_coherent_mask(struct device *dev, u64 mask) { return 0; }

/* --- mapping dei buffer: un handle unico, mai dereferenziato ------------- */

dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page,
			      size_t offset, size_t size,
			      enum dma_data_direction dir, unsigned long attrs)
{
	dma_addr_t h = shim_dma_next;

	shim_dma_next += ALIGN(size, 0x1000);
	return h;
}

void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
			  enum dma_data_direction dir, unsigned long attrs) { }
void dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
			     enum dma_data_direction dir) { }
void dma_sync_single_for_device(struct device *dev, dma_addr_t addr,
				size_t size, enum dma_data_direction dir) { }

/* --- skb: solo cio' che dma.c tocca --------------------------------------- */

struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int len,
				   gfp_t gfp)
{
	struct sk_buff *skb = kzalloc(sizeof(*skb), gfp);
	u8 *data;

	if (!skb)
		return NULL;
	data = kzalloc(len, gfp);
	if (!data) {
		kfree(skb);
		return NULL;
	}
	skb->head = data;
	skb->data = data;
	skb->tail = 0;
	skb->end = len;
	skb->len = 0;
	return skb;
}

void *skb_put(struct sk_buff *skb, unsigned int len)
{
	void *p = skb->head + skb->tail;

	skb->tail += len;
	skb->len += len;
	return p;
}

void *skb_pull(struct sk_buff *skb, unsigned int len)
{
	skb->len -= len;
	skb->data += len;
	return skb->data;
}

void consume_skb(struct sk_buff *skb)
{
	if (!skb)
		return;
	kfree(skb->head);
	kfree(skb);
}

void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
	void *p = kmalloc(len, gfp);

	if (p)
		memcpy(p, src, len);
	return p;
}

const char *dev_driver_string(const struct device *dev) { return "b43"; }

/* Le code TX di mac80211 e lo stato di trasmissione: niente traffico qui. */
void ieee80211_wake_queue(struct ieee80211_hw *hw, int queue) { }
void ieee80211_tx_status_skb(struct ieee80211_hw *hw, struct sk_buff *skb) { }
