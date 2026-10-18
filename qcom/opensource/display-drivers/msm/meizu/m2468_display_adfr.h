/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _M2468_DISPLAY_ADFR_H_
#define _M2468_DISPLAY_ADFR_H_

#include <linux/kconfig.h>
#include <linux/types.h>

struct dsi_display;
struct dsi_panel;

#if IS_ENABLED(CONFIG_DRM_MSM_DSI)
int m2468_display_adfr_init(struct dsi_display *display);
void m2468_display_adfr_active(void);
void m2468_display_adfr_before_backlight(struct dsi_panel *panel);
void m2468_display_adfr_after_backlight(struct dsi_panel *panel);
void m2468_display_adfr_mode_switch(struct dsi_panel *panel);
void m2468_display_adfr_aod_enter(struct dsi_panel *panel);
void m2468_display_adfr_panel_disable(void);
void m2468_display_adfr_handle_idle(bool enter_idle);
#else
static inline int m2468_display_adfr_init(struct dsi_display *display)
{
	(void)display;
	return 0;
}

static inline void m2468_display_adfr_active(void) {}
static inline void m2468_display_adfr_before_backlight(struct dsi_panel *panel)
{
	(void)panel;
}
static inline void m2468_display_adfr_after_backlight(struct dsi_panel *panel)
{
	(void)panel;
}
static inline void m2468_display_adfr_mode_switch(struct dsi_panel *panel)
{
	(void)panel;
}
static inline void m2468_display_adfr_aod_enter(struct dsi_panel *panel)
{
	(void)panel;
}
static inline void m2468_display_adfr_panel_disable(void) {}
static inline void m2468_display_adfr_handle_idle(bool enter_idle)
{
	(void)enter_idle;
}
#endif

#endif
