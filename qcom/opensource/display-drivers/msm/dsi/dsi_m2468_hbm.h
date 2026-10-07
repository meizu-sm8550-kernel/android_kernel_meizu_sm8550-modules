/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_M2468_HBM_H
#define DSI_M2468_HBM_H
#include "dsi_m2468_hbm_core.h"
struct dsi_panel;
struct dsi_display;
struct m2468_hbm_panel {
	struct m2468_hbm_state state;
	bool supported, low_power;
	u32 vblanks;
};
void dsi_m2468_hbm_init(struct dsi_panel *panel);
/* panel_lock and DSI clocks held. Does not change local-HBM ready. */
int dsi_m2468_hbm_backlight_adfr(struct dsi_panel *panel, u32 value);
int dsi_m2468_hbm_bind(struct dsi_display *display);
void dsi_m2468_hbm_unbind(struct dsi_display *display);
void dsi_m2468_hbm_te(void *display);
/* panel_lock held. false invalidates; true follows successful full ON. */
void dsi_m2468_hbm_invalidate(struct dsi_panel *panel, bool initialized);
void dsi_m2468_hbm_low_power(struct dsi_panel *panel, bool low_power);
/* display_lock held; quiesce before mode replacement/LP/zero brightness. */
int dsi_m2468_hbm_quiesce(struct dsi_display *display);
/* Same conversion for normal requests and HBM restoration. panel_lock held. */
u32 dsi_display_bl_to_panel(struct dsi_panel *panel, u32 level);
int dsi_m2468_hbm_set_lut(struct dsi_panel *panel, const void *data, size_t size);
#endif
