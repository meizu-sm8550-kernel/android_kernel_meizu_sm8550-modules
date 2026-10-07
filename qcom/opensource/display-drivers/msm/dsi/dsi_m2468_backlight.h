/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_M2468_BACKLIGHT_H
#define DSI_M2468_BACKLIGHT_H
#include "dsi_m2468_backlight_core.h"
struct dsi_panel;
struct dsi_panel_cmd_set;
int dsi_m2468_backlight_parse(struct dsi_panel_cmd_set *set, const void *data, u32 length, bool lp);
struct m2468_bl_panel {
	struct m2468_bl_state state;
	bool configured;
};

void dsi_m2468_backlight_init(struct dsi_panel *panel);
/* All calls serialized by panel_lock, including HBM restoration. */
void dsi_m2468_backlight_invalidate(struct dsi_panel *panel);
int dsi_m2468_backlight_set(struct dsi_panel *panel, u32 panel_level);
#endif
