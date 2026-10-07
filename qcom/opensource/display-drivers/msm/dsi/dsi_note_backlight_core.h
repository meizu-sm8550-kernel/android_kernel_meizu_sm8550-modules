/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_NOTE_BACKLIGHT_CORE_H
#define DSI_NOTE_BACKLIGHT_CORE_H
#include <linux/types.h>
#include <linux/errno.h>

enum note_bl_command { NOTE_BL_DC, NOTE_BL_PWM, NOTE_BL_COMP_DC,
	NOTE_BL_COMP_MID, NOTE_BL_COMP_LOW };

enum note_bl_band { NOTE_BL_LOW, NOTE_BL_MID, NOTE_BL_HIGH };

struct note_bl_state {
	bool valid, dc;
	enum note_bl_band band, compensation;
	u32 level;
};

struct note_bl_ops {
	int (*send)(void *ctx, enum note_bl_command command, u32 level);
	int (*sync)(void *ctx);
	int (*brightness)(void *ctx, u32 level);
	void (*wait_ms)(void *ctx, u32 ms);
};

int note_bl_check_blob(const void *data, u32 length, u32 expected);
int note_bl_run(struct note_bl_state *state, u32 level, u32 hz,
		const struct note_bl_ops *ops, void *ctx);
#endif
