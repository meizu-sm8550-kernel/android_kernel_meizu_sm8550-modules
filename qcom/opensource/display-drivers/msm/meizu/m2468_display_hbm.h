/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _M2468_DISPLAY_HBM_H_
#define _M2468_DISPLAY_HBM_H_

#include <linux/kconfig.h>
#include <linux/types.h>

struct dsi_display;
struct dsi_panel;
struct kobject;
struct sde_encoder_phys;

enum m2468_backlight_type {
	M2468_BACKLIGHT_PWM,
	M2468_BACKLIGHT_DC,
	M2468_BACKLIGHT_LOW_PWM,
};

struct m2468_panel_backlight {
	u32 normal_max;
	u32 dc_min;
	u32 requested_bl;
	u32 effective_bl;
	u32 type;
	u32 hbm_mode;
	u32 ofp_type;
	u32 ready_vblanks;
	bool supported;
	bool pwm_dc_enabled;
	bool ofp_enabled;
	bool global_hbm;
	bool local_hbm;
	bool aod_active;
};

#if IS_ENABLED(CONFIG_DRM_MSM_DSI)
bool m2468_display_panel_supported(const struct dsi_panel *panel);
int m2468_display_hbm_init(struct dsi_display *display, struct kobject *kobj);
void m2468_display_hbm_deinit(struct kobject *kobj);
void m2468_display_hbm_panel_disable(struct dsi_panel *panel);
void m2468_display_hbm_te_irq(struct sde_encoder_phys *phys_enc);
int m2468_display_wait_for_vsync(struct dsi_panel *panel);
void m2468_display_frame_delay(struct dsi_panel *panel, unsigned int frames);
int m2468_display_backlight_prepare(struct dsi_panel *panel, u32 level);
void m2468_display_backlight_done(struct dsi_panel *panel, u32 level);
int m2468_display_hbm_aod_enter(struct dsi_panel *panel);
int m2468_display_hbm_aod_exit(struct dsi_panel *panel);
int display_set_fingerprint_scene(unsigned int scene);
unsigned int display_get_fingerprint_scene(void);
#else
static inline bool m2468_display_panel_supported(const struct dsi_panel *panel)
{
	return false;
}
static inline void m2468_display_hbm_te_irq(struct sde_encoder_phys *phys_enc) {}
#endif

#endif
