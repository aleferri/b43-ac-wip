/*
 * d11_core.h - host-visible model of a corerev-42 802.11 MAC core.
 *
 * Models the state a driver can observe and change over the bus: the direct
 * MMIO register file, the indirect object-memory spaces reached through the
 * OBJADDR/OBJDATA window (ucode program memory, shared memory, scratch,
 * internal-hw, RCMTA), the MACCONTROL latch and the PSM run/suspend handshake.
 *
 * It does NOT model the PHY or radio: those are a separate fake (fakephy.*),
 * reached through the PHY/radio ports, nexmon-style. It does NOT execute the
 * microcode instruction set; see psm42.* for what the PSM layer does and does
 * not do.
 */
#ifndef D11_CORE_H
#define D11_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Object-memory select field, (OBJADDR >> 16) & 0xff. Values are the b43
 * SHM-routing enum, which is the hardware encoding. */
enum d11_objsel {
	D11_OBJ_UCODE   = 0,   /* microcode program memory, 32-bit words      */
	D11_OBJ_SHARED  = 1,   /* shared memory, 16-bit words, byte-addressed */
	D11_OBJ_SCRATCH = 2,   /* scratch / PSM registers, 32-bit words       */
	D11_OBJ_HW      = 3,   /* internal hardware registers                 */
	D11_OBJ_RCMTA   = 4,   /* receive-match transmitter address table     */
	D11_OBJ__COUNT  = 5,
};

/* MMIO register file is byte-addressable up to here (DMA/PIO blocks sit well
 * past 0x400 on this corerev). */
#define D11_REG_BYTES    0x1000

#define D11_UCODE_WORDS  0x4000
#define D11_SHM_BYTES    0x2000
#define D11_SCR_WORDS    0x0100
#define D11_HW_WORDS     0x0100
#define D11_RCMTA_WORDS  0x0200

/* Direct MMIO offsets this model acts on; everything else is a plain cell. */
#define D11_MMIO_MACCTL          0x120
#define D11_MMIO_MACCMD          0x124
#define D11_MMIO_GEN_IRQ_REASON  0x128
#define D11_MMIO_OBJADDR         0x160
#define D11_MMIO_OBJDATA         0x164
#define D11_MMIO_OBJDATA_HI      0x166

#define D11_MACCTL_PSM_RUN   0x00000002
#define D11_MACCTL_PSM_JMP0  0x00000004

#define D11_IRQ_MAC_SUSPENDED 0x00000001

/* Shared-memory offsets the ucode populates and the host reads back. The
 * values at these cells are written by the real microcode, not here: run the
 * extracted blob through the arch15 interpreter (ucode_init.py) to produce
 * them. This model only raises the MAC_SUSPENDED handshake the host polls. */
#define D11_SHM_UCODEREV   0x0000
#define D11_SHM_UCODEPATCH 0x0002
#define D11_SHM_UCODEDATE  0x0004
#define D11_SHM_UCODETIME  0x0006

struct d11_core {
	uint8_t  reg[D11_REG_BYTES];
	uint32_t ucode[D11_UCODE_WORDS];
	uint8_t  shm[D11_SHM_BYTES];
	uint32_t scr[D11_SCR_WORDS];
	uint32_t hw[D11_HW_WORDS];
	uint32_t rcmta[D11_RCMTA_WORDS];

	size_t   ucode_words;       /* highest word index written + 1        */
	uint32_t objaddr;           /* last value staged into OBJADDR         */
	uint32_t macctl;            /* MACCONTROL latch                       */
	int      psm_booted;        /* MAC_SUSPENDED handshake already raised */

	/* bookkeeping for reports */
	unsigned long writes[D11_OBJ__COUNT];
	unsigned long reg_writes, reg_reads;
};

void core_init(struct d11_core *c);

/* High-level object-memory access used by the op replayer. addr is a byte
 * offset for D11_OBJ_SHARED and a word index for every other space, matching
 * the decoded-op vocabulary. */
void core_obj_write(struct d11_core *c, enum d11_objsel sel,
		    uint32_t addr, uint32_t val, int width);
uint32_t core_obj_read(struct d11_core *c, enum d11_objsel sel,
		       uint32_t addr, int width);

/* Direct MMIO register file. */
void core_reg_write(struct d11_core *c, uint32_t off, uint32_t val, int width);
uint32_t core_reg_read(struct d11_core *c, uint32_t off, int width);

/* MACCONTROL write; drives the JMP0 -> RUN transition that boots the ucode. */
void core_maccontrol_write(struct d11_core *c, uint32_t val);

/* The host-observable handshake: raises MAC_SUSPENDED in GEN_IRQ_REASON, which
 * is what b43_upload_microcode polls for. Idempotent; a no-op unless PSM_RUN is
 * set, ucode is loaded and it has not fired yet. Returns 1 the turn it fires.
 * The ucode's effect on shared memory is NOT modelled here; it comes from
 * executing the real blob (ucode_init.py). */
int core_ucode_boot(struct d11_core *c);

/* 16-bit shared-memory accessors (byte offset), used by core and callers. */
uint16_t core_shm16_read(const struct d11_core *c, uint32_t byte_off);
void core_shm16_write(struct d11_core *c, uint32_t byte_off, uint16_t val);

#endif /* D11_CORE_H */
