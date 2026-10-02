#include "ops.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Pull "key=0x..." (or decimal) out of a line. Returns 1 on hit. */
static int field_u32(const char *line, const char *key, uint32_t *out)
{
	const char *p = strstr(line, key);
	if (!p)
		return 0;
	p += strlen(key);
	*out = (uint32_t)strtoul(p, NULL, 0);
	return 1;
}

/* Number of hex digits in a "key=0x...." field, used to infer access width. */
static int field_hexdigits(const char *line, const char *key)
{
	const char *p = strstr(line, key);
	if (!p)
		return 0;
	p += strlen(key);
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	int n = 0;
	while (isxdigit((unsigned char)p[n]))
		n++;
	return n;
}

static const char *find_class(const char *line)
{
	/* class is the first token of form AAA.BBB or "WIN", after "cpu<N>". */
	const char *p = strstr(line, "cpu");
	if (p) {
		p += 3;
		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	} else {
		p = line;
	}
	return p;
}

static int class_is(const char *cls, const char *name)
{
	size_t n = strlen(name);
	return strncmp(cls, name, n) == 0 &&
	       (cls[n] == ' ' || cls[n] == '\0' || cls[n] == '\n');
}

void replay_stream(FILE *in, struct d11_core *core, struct fakephy *phy,
		   struct d11_chip *chip, struct replay_stats *st)
{
	char line[512];

	memset(st, 0, sizeof(*st));

	while (fgets(line, sizeof(line), in)) {
		st->lines++;
		const char *cls = find_class(line);
		uint32_t addr = 0, off = 0, val = 0, sel = 0;

		if (class_is(cls, "REG.WR")) {
			if (!field_u32(cls, "off=", &off) ||
			    !field_u32(cls, "val=", &val)) { st->parse_errors++; continue; }
			int w = field_hexdigits(cls, "val=") > 4 ? 4 : 2;
			core_reg_write(core, off, val, w);
			st->reg_wr++; st->applied++;
		} else if (class_is(cls, "REG.RD")) {
			st->reg_rd++; st->ignored++;
		} else if (class_is(cls, "OBJ.WR")) {
			if (!field_u32(cls, "addr=", &addr) ||
			    !field_u32(cls, "val=", &val) ||
			    !field_u32(cls, "sel=", &sel)) { st->parse_errors++; continue; }
			enum d11_objsel s = (enum d11_objsel)((sel >> 16) & 0xff);
			int w = (s == D11_OBJ_SHARED) ? 2 : 4;
			core_obj_write(core, s, addr, val, w);
			st->obj_wr++; st->applied++;
		} else if (class_is(cls, "OBJ.RD")) {
			st->obj_rd++; st->ignored++;
		} else if (class_is(cls, "OBJ.BULKW") || class_is(cls, "OBJ.BULKR")) {
			/* folded run with no values: re-decode with --no-bulk */
			st->bulk_skipped++; st->ignored++;
		} else if (class_is(cls, "MAC.MCTRL")) {
			if (!field_u32(cls, "val=", &val)) { st->parse_errors++; continue; }
			core_maccontrol_write(core, val);
			st->mac_mctrl++; st->applied++;
			if (core->psm_booted)
				st->ucode_booted = 1;
		} else if (class_is(cls, "MAC.MCMD")) {
			if (field_u32(cls, "val=", &val))
				core_reg_write(core, D11_MMIO_MACCMD, val, 4);
			st->mac_mcmd++; st->applied++;
		} else if (class_is(cls, "PHY.WR")) {
			if (field_u32(cls, "addr=", &addr) && field_u32(cls, "val=", &val)) {
				fakephy_phy_write(phy, (uint16_t)addr, (uint16_t)val);
				st->phy_wr++; st->applied++;
			} else st->parse_errors++;
		} else if (class_is(cls, "PHY.RD")) {
			if (field_u32(cls, "addr=", &addr) && field_u32(cls, "val=", &val))
				fakephy_observe_phy_read(phy, (uint16_t)addr, (uint16_t)val);
			st->phy_rd++; st->ignored++;
		} else if (class_is(cls, "RAD.WR")) {
			if (field_u32(cls, "addr=", &addr) && field_u32(cls, "val=", &val)) {
				fakephy_rad_write(phy, (uint16_t)addr, (uint16_t)val);
				st->rad_wr++; st->applied++;
			} else st->parse_errors++;
		} else if (class_is(cls, "RAD.RD")) {
			st->rad_rd++; st->ignored++;
		} else if (class_is(cls, "CC.WR") || class_is(cls, "PCIE.WR") ||
			   class_is(cls, "WRAP.WR")) {
			if (!field_u32(cls, "off=", &off) ||
			    !field_u32(cls, "val=", &val)) { st->parse_errors++; continue; }
			if (cls[0] == 'C')
				chip_cc_write(chip, off, val);
			else if (cls[0] == 'P')
				chip_pcie_write(chip, off, val);
			else
				chip_wrap_write(chip, off, val);
			st->chip_wr++; st->applied++;
		} else {
			/* EROM/SROM/CORE/WIN and reads: not state here */
			st->ignored++;
		}
	}
}
