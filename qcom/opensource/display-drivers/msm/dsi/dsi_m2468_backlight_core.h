/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_M2468_BACKLIGHT_CORE_H
#define DSI_M2468_BACKLIGHT_CORE_H
#include <linux/types.h>
#include <linux/errno.h>

enum m2468_bl_command { M2468_BL_DC, M2468_BL_PWM, M2468_BL_COMP_DC,
	M2468_BL_COMP_MID, M2468_BL_COMP_LOW };

enum m2468_bl_band { M2468_BL_LOW, M2468_BL_MID, M2468_BL_HIGH };

struct m2468_bl_state {
	bool valid, dc;
	enum m2468_bl_band band, compensation;
	u32 level;
};

struct m2468_bl_ops {
	int (*send)(void *ctx, enum m2468_bl_command command, u32 level);
	int (*sync)(void *ctx);
	int (*brightness)(void *ctx, u32 level);
	void (*wait_ms)(void *ctx, u32 ms);
};

int m2468_bl_check_blob(const void *data, u32 length, u32 expected);
int m2468_bl_run(struct m2468_bl_state *state, u32 level, u32 hz,
		const struct m2468_bl_ops *ops, void *ctx);
#endif
