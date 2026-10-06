/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DSI_NOTE_HBM_H
#define DSI_NOTE_HBM_H
#include "dsi_note_hbm_core.h"
struct dsi_panel;
struct dsi_display;
struct note_hbm_panel {
	struct note_hbm_state state;
	bool supported, low_power;
	u32 vblanks;
};
void dsi_note_hbm_init(struct dsi_panel *panel);
int dsi_note_hbm_bind(struct dsi_display *display);
void dsi_note_hbm_unbind(struct dsi_display *display);
void dsi_note_hbm_te(void *display);
/* panel_lock held. false invalidates; true follows successful full ON. */
void dsi_note_hbm_invalidate(struct dsi_panel *panel, bool initialized);
void dsi_note_hbm_low_power(struct dsi_panel *panel, bool low_power);
/* display_lock held; quiesce before mode replacement/LP/zero brightness. */
int dsi_note_hbm_quiesce(struct dsi_display *display);
/* Same conversion for normal requests and HBM restoration. panel_lock held. */
u32 dsi_display_bl_to_panel(struct dsi_panel *panel, u32 level);
int dsi_note_hbm_set_lut(struct dsi_panel *panel, const void *data, size_t size);
#endif
