/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _M2468_DISPLAY_BRIGHTNESS_H_
#define _M2468_DISPLAY_BRIGHTNESS_H_

#include <linux/kconfig.h>
#include <linux/errno.h>
#include <linux/types.h>

struct backlight_device;
struct drm_encoder;
struct dsi_display_mode;
struct dsi_panel;

#if IS_ENABLED(CONFIG_DRM_MSM_DSI)
extern bool synclights_enable;
int m2468_display_parse_vsync(struct dsi_display_mode *mode);
int m2468_display_set_brightness(struct backlight_device *bd, u32 brightness);
int m2468_display_sync_brightness(struct drm_encoder *encoder);
int m2468_display_update_pwm_dc(struct dsi_panel *panel, u32 level);
#else
static inline int m2468_display_set_brightness(struct backlight_device *bd,
					      u32 brightness)
{
	return -EOPNOTSUPP;
}
static inline int m2468_display_sync_brightness(struct drm_encoder *encoder)
{
	return 0;
}
#endif

#endif
