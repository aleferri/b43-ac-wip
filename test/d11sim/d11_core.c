#include "d11_core.h"
#include <string.h>

void core_init(struct d11_core *c)
{
	memset(c, 0, sizeof(*c));
}

uint16_t core_shm16_read(const struct d11_core *c, uint32_t off)
{
	if (off + 1 >= D11_SHM_BYTES)
		return 0;
	/* Shared memory is little-endian to the host. */
	return (uint16_t)c->shm[off] | ((uint16_t)c->shm[off + 1] << 8);
}

void core_shm16_write(struct d11_core *c, uint32_t off, uint16_t val)
{
	if (off + 1 >= D11_SHM_BYTES)
		return;
	c->shm[off]     = (uint8_t)val;
	c->shm[off + 1] = (uint8_t)(val >> 8);
}

void core_obj_write(struct d11_core *c, enum d11_objsel sel,
		    uint32_t addr, uint32_t val, int width)
{
	switch (sel) {
	case D11_OBJ_UCODE:
		if (addr < D11_UCODE_WORDS) {
			c->ucode[addr] = val;
			if (addr + 1 > c->ucode_words)
				c->ucode_words = addr + 1;
		}
		break;
	case D11_OBJ_SHARED:
		/* addr is already a byte offset; a 32-bit access is two words. */
		if (width == 4) {
			core_shm16_write(c, addr, (uint16_t)val);
			core_shm16_write(c, addr + 2, (uint16_t)(val >> 16));
		} else {
			core_shm16_write(c, addr, (uint16_t)val);
		}
		break;
	case D11_OBJ_SCRATCH:
		if (addr < D11_SCR_WORDS)
			c->scr[addr] = val;
		break;
	case D11_OBJ_HW:
		if (addr < D11_HW_WORDS)
			c->hw[addr] = val;
		break;
	case D11_OBJ_RCMTA:
		if (addr < D11_RCMTA_WORDS)
			c->rcmta[addr] = val;
		break;
	default:
		return;
	}
	c->writes[sel]++;
}

uint32_t core_obj_read(struct d11_core *c, enum d11_objsel sel,
		       uint32_t addr, int width)
{
	switch (sel) {
	case D11_OBJ_UCODE:
		return addr < D11_UCODE_WORDS ? c->ucode[addr] : 0;
	case D11_OBJ_SHARED:
		if (width == 4)
			return core_shm16_read(c, addr) |
			       ((uint32_t)core_shm16_read(c, addr + 2) << 16);
		return core_shm16_read(c, addr);
	case D11_OBJ_SCRATCH:
		return addr < D11_SCR_WORDS ? c->scr[addr] : 0;
	case D11_OBJ_HW:
		return addr < D11_HW_WORDS ? c->hw[addr] : 0;
	case D11_OBJ_RCMTA:
		return addr < D11_RCMTA_WORDS ? c->rcmta[addr] : 0;
	default:
		return 0;
	}
}

void core_reg_write(struct d11_core *c, uint32_t off, uint32_t val, int width)
{
	if (off == D11_MMIO_MACCTL && width == 4) {
		core_maccontrol_write(c, val);
		return;
	}
	if (off + (uint32_t)width > D11_REG_BYTES)
		return;
	for (int i = 0; i < width; i++)
		c->reg[off + i] = (uint8_t)(val >> (8 * i));   /* little-endian */
	c->reg_writes++;
}

uint32_t core_reg_read(struct d11_core *c, uint32_t off, int width)
{
	c->reg_reads++;
	if (off == D11_MMIO_MACCTL && width == 4)
		return c->macctl;
	if (off + (uint32_t)width > D11_REG_BYTES)
		return 0;
	uint32_t v = 0;
	for (int i = 0; i < width; i++)
		v |= (uint32_t)c->reg[off + i] << (8 * i);
	return v;
}

void core_maccontrol_write(struct d11_core *c, uint32_t val)
{
	c->macctl = val;
	for (int i = 0; i < 4; i++)
		c->reg[D11_MMIO_MACCTL + i] = (uint8_t)(val >> (8 * i));
	if (val & D11_MACCTL_PSM_RUN)
		core_ucode_boot(c);
}

int core_ucode_boot(struct d11_core *c)
{
	if (c->psm_booted)
		return 0;
	if (!(c->macctl & D11_MACCTL_PSM_RUN))
		return 0;
	if (c->ucode_words == 0)
		return 0;

	uint32_t irq = core_reg_read(c, D11_MMIO_GEN_IRQ_REASON, 4);
	core_reg_write(c, D11_MMIO_GEN_IRQ_REASON, irq | D11_IRQ_MAC_SUSPENDED, 4);

	c->psm_booted = 1;
	return 1;
}
