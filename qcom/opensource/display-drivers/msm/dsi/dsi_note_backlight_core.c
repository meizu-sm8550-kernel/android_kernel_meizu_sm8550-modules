// SPDX-License-Identifier: GPL-2.0-only
#include "dsi_note_backlight_core.h"

/* The generic parser accepts a short trailing header. Note requires exact
 * packet boundaries before allocation, including a nonempty payload.
 */
int note_bl_check_blob(const void *data, u32 length, u32 expected)
{
	const u8 *cursor = data;
	u32 count = 0, size;

	if (!cursor)
		return -EINVAL;
	while (length >= 7) {
		size = ((u32)cursor[5] << 8) | cursor[6];
		if (!size || size > 64 || size + 7 > length)
			return -EINVAL;
		length -= size + 7;
		cursor += size + 7;
		count++;
	}
	return !length && count == expected ? 0 : -EINVAL;
}

/* State is committed only after the complete transaction, including 0x51. */
int note_bl_run(struct note_bl_state *state, u32 level, u32 hz,
		const struct note_bl_ops *ops, void *ctx)
{
	struct note_bl_state next = *state;
	enum note_bl_band band;
	bool dc, change;
	u32 delay;
	int rc;

	if (level > 4095) {
		rc = -ERANGE;
		goto fail;
	}
	switch (hz) {
	case 144:
		delay = 14;
		break;
	case 120:
		delay = 18;
		break;
	case 90:
		delay = 26;
		break;
	case 60:
		delay = 34;
		break;
	case 30:
		delay = 66;
		break;
	default:
		rc = -EOPNOTSUPP;
		goto fail;
	}
	if (!level)
		goto brightness;
	band = level <= 408 ? NOTE_BL_LOW :
		level < 1107 ? NOTE_BL_MID : NOTE_BL_HIGH;
	dc = band == NOTE_BL_HIGH;
	change = !next.valid || next.dc != dc;
	/* In the known state preserve stock's demura-before-switch ordering.
	 * DC -> low sends LOW then PWM, whose table leaves actual MID.
	 * Keep actual compensation separate from the requested/cached band.
	 */
	if (next.valid && band == NOTE_BL_LOW && next.band != NOTE_BL_LOW) {
		rc = ops->send(ctx, NOTE_BL_COMP_LOW, level);
		if (rc)
			goto fail;
		next.compensation = NOTE_BL_LOW;
	} else if (next.valid && band != NOTE_BL_LOW && next.band == NOTE_BL_LOW) {
		rc = ops->send(ctx, NOTE_BL_COMP_MID, level);
		if (rc)
			goto fail;
		next.compensation = NOTE_BL_MID;
	}
	if (change) {
		rc = ops->sync(ctx);
		if (rc)
			goto fail;
		rc = ops->send(ctx, dc ? NOTE_BL_DC : NOTE_BL_PWM, level);
		if (rc)
			goto fail;
		ops->wait_ms(ctx, delay);
		next.dc = dc;
		next.compensation = dc ? NOTE_BL_HIGH : NOTE_BL_MID;
	}
	/* Splash, errors, HBM, LP and mode replacement cannot establish a
	 * known hardware state. Rebuild the complete mode, then compensation.
	 */
	if (!next.valid) {
		rc = ops->send(ctx, band == NOTE_BL_LOW ? NOTE_BL_COMP_LOW :
			band == NOTE_BL_MID ? NOTE_BL_COMP_MID : NOTE_BL_COMP_DC, level);
		if (rc)
			goto fail;
		next.compensation = band;
	}
	next.valid = true;
	next.band = band;
brightness:
	rc = ops->brightness(ctx, level);
	if (rc)
		goto fail;
	next.level = level;
	*state = next;
	return 0;
fail:
	state->valid = false;
	return rc < 0 ? rc : -EIO;
}
