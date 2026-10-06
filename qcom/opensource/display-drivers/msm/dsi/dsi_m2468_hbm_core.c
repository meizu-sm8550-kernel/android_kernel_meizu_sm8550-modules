// SPDX-License-Identifier: GPL-2.0-only
/* M2468 ELF/DT compatibility rewrite; not original Meizu source. */
#include "dsi_m2468_hbm_core.h"

int m2468_hbm_rgb(u32 brightness, bool on, u8 rgb[3])
{
	static const u8 levels[][3] = {
		{0xd9, 0xd9, 0xd9}, {0xd4, 0xd4, 0xd4}, {0xd0, 0xcf, 0xce},
		{0xcd, 0xcb, 0xca}, {0xc8, 0xc6, 0xc5}, {0xc4, 0xc2, 0xc1},
		{0xbe, 0xbc, 0xbb}, {0xba, 0xb7, 0xb6}, {0xb6, 0xb4, 0xb3},
		{0xb3, 0xb0, 0xae}, {0xaf, 0xac, 0xa9}, {0xac, 0xa8, 0xa6},
		{0xa8, 0xa4, 0xa2}, {0xa3, 0xa0, 0x9d}, {0xa0, 0x9d, 0x9a},
		{0x9c, 0x99, 0x95}, {0x9a, 0x96, 0x93}, {0x97, 0x93, 0x90},
		{0x95, 0x91, 0x8e},
	};
	if (brightness < 3151 || brightness > 4095)
		return -ERANGE;
	if (on)
		memcpy(rgb, levels[(brightness - 3151) / 50], 3);
	else
		memset(rgb, 0xdf, 3);
	return 0;
}

void m2468_hbm_demura(u32 attempted, u32 requested, u32 dc_min, u8 payload[4])
{
	payload[0] = 0x88;
	payload[1] = attempted <= 408 ? 0x40 : requested < dc_min ? 0x20 : 0;
	payload[2] = attempted <= 408 ? 0x80 : requested < dc_min ? 0x40 : 1;
	payload[3] = 0xf0;
}

int m2468_hbm_delays(u32 hz, u32 *before, u32 *after)
{
	switch (hz) {
	case 144: *before = 7; *after = 14; break;
	case 120: *before = 9; *after = 18; break;
	case 90: *before = 13; *after = 26; break;
	case 60: *before = 17; *after = 34; break;
	case 30: *before = 33; *after = 66; break;
	default: return -EINVAL;
	}
	return 0;
}

int m2468_hbm_adfr_byte(u32 code)
{
	switch (code) {
	case 1: return 0x77;
	case 16: return 0x0b;
	case 32: return 0x05;
	case 48: return 0x03;
	case 64: return 0x02;
	case 96: return 0x01;
	default: return -EINVAL;
	}
}

void m2468_hbm_invalidate(struct m2468_hbm_state *s, bool reset_complete)
{
	s->generation++;
	s->phase = reset_complete ? M2468_HBM_OFF : M2468_HBM_FAULT;
	s->attempted_valid = false;
	s->successful_valid = false;
	s->adfr_valid = false;
	if (reset_complete) {
		s->adfr = 0;
		s->saved_adfr = 0;
	}
}

bool m2468_hbm_ready_matches(const struct m2468_hbm_state *s, u64 generation)
{
	return s->phase == M2468_HBM_ON && s->generation == generation;
}

static void m2468_first_error(int *first, int rc)
{
	if (!*first && rc)
		*first = rc;
}

/* Always attempt all recovery stages; partial packets may have taken effect. */
static int m2468_hbm_restore(struct m2468_hbm_state *s, u32 after, bool use_adfr,
		const struct m2468_hbm_ops *ops, void *ctx)
{
	int first = 0, rc;

	s->phase = M2468_HBM_DISABLING;
	m2468_first_error(&first, ops->send(ctx, M2468_LOCAL_OFF, 0));
	if (s->attempted >= 3151)
		m2468_first_error(&first, ops->send(ctx, M2468_LEVEL_OFF, 0));
	ops->wait_ms(ctx, after);
	m2468_first_error(&first, ops->restore(ctx));
	s->adfr_valid = false;
	if (use_adfr) {
		rc = ops->send(ctx, M2468_ADFR, s->saved_adfr);
		m2468_first_error(&first, rc);
		s->adfr_valid = !rc;
		if (!rc)
			s->adfr = s->saved_adfr;
	}
	s->phase = first ? M2468_HBM_FAULT : M2468_HBM_OFF;
	if (!first)
		s->saved_adfr = 0;
	return first;
}

/* Caller owns panel transaction lock, validated mode/tables and clock vote. */
int m2468_hbm_run(struct m2468_hbm_state *s, bool on, u32 hz, bool use_adfr,
		const struct m2468_hbm_ops *ops, void *ctx)
{
	u32 before, after;
	int rc, recovery;

	rc = m2468_hbm_delays(hz, &before, &after);
	if (rc)
		return rc;
	if (s->phase == M2468_HBM_FAULT)
		return -EIO;
	if ((on && s->phase == M2468_HBM_ON) || (!on && s->phase == M2468_HBM_OFF))
		return 0;
	if (!s->attempted_valid || !s->successful_valid)
		return -EAGAIN;
	if (s->attempted > 4095 || s->requested > 4095)
		return -ERANGE;
	s->generation++;
	if (!on)
		return m2468_hbm_restore(s, after, use_adfr, ops, ctx);

	/* Candidate default policy is ADFR off until explicitly programmed. */
	s->saved_adfr = use_adfr && s->adfr_valid ? s->adfr : 0;
	s->phase = M2468_HBM_ENABLING;
	s->adfr_valid = false;
	rc = 0;
	if (use_adfr) {
		rc = ops->send(ctx, M2468_ADFR, 0);
		s->adfr_valid = !rc;
		if (!rc)
			s->adfr = 0;
	}
	if (!rc) {
		ops->wait_ms(ctx, before);
		rc = ops->send(ctx, M2468_LOCAL_ON, 0);
	}
	if (!rc && s->attempted >= 3151)
		rc = ops->send(ctx, M2468_LEVEL_ON, 0);
	if (rc) {
		recovery = m2468_hbm_restore(s, after, use_adfr, ops, ctx);
		if (recovery)
			s->phase = M2468_HBM_FAULT;
		return rc;
	}
	ops->wait_ms(ctx, after);
	s->phase = M2468_HBM_ON;
	return 0;
}
