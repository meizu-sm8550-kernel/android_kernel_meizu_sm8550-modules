// SPDX-License-Identifier: GPL-2.0-only

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/ktime.h>
#include <linux/math64.h>

#include "../dsi/dsi_display.h"
#include "../dsi/dsi_panel.h"
#include "../sde/sde_connector.h"
#include "../sde/sde_encoder.h"
#include "../sde/sde_encoder_phys.h"
#include "m2468_display_brightness.h"
#include "m2468_display_hbm.h"

bool synclights_enable = true;
EXPORT_SYMBOL(synclights_enable);

int m2468_display_parse_vsync(struct dsi_display_mode *mode)
{
	if (!mode || !mode->priv_info || !mode->timing.refresh_rate)
		return -EINVAL;

	mode->priv_info->vsync_period = USEC_PER_SEC / mode->timing.refresh_rate;
	mode->priv_info->vsync_width = mode->priv_info->vsync_period / 2;
	return 0;
}

int m2468_display_set_brightness(struct backlight_device *bd, u32 brightness)
{
	int rc = -ENXIO;

	if (!bd)
		return -ENODEV;
	mutex_lock(&bd->ops_lock);
	if (bd->ops) {
		if (brightness > bd->props.max_brightness) {
			rc = -EINVAL;
		} else {
			bd->props.brightness = brightness;
			rc = backlight_update_status(bd);
		}
	}
	mutex_unlock(&bd->ops_lock);
	return rc;
}

int m2468_display_sync_brightness(struct drm_encoder *encoder)
{
	struct sde_encoder_virt *sde_enc;
	struct sde_encoder_phys *phys;
	struct sde_encoder_phys_cmd *cmd_enc;
	struct sde_encoder_phys_cmd_te_timestamp *te;
	struct sde_connector *conn;
	struct dsi_display *display;
	struct dsi_display_mode_priv_info *priv;
	ktime_t timestamp = 0;
	unsigned long flags;
	s64 elapsed, delay;
	u32 brightness, period, width;
	int rc;

	if (!encoder)
		return -EINVAL;
	sde_enc = to_sde_encoder_virt(encoder);
	phys = sde_enc->cur_master;
	if (!phys || !phys->connector ||
	    phys->connector->connector_type != DRM_MODE_CONNECTOR_DSI)
		return 0;
	conn = to_sde_connector(phys->connector);
	display = conn->display;
	if (!display || !m2468_display_panel_supported(display->panel) ||
	    !READ_ONCE(conn->bl_need_sync))
		return 0;
	if (!phys->connector->state || !conn->bl_device ||
	    !display->panel->cur_mode || !display->panel->cur_mode->priv_info)
		return -ENODEV;

	brightness = sde_connector_get_property(phys->connector->state,
						CONNECTOR_PROP_SYNC_BL);
	priv = display->panel->cur_mode->priv_info;
	period = priv->vsync_period;
	width = priv->vsync_width;
	if (READ_ONCE(synclights_enable) && period && width < period &&
	    sde_encoder_get_intf_mode(encoder) == INTF_MODE_CMD) {
		cmd_enc = container_of(phys, struct sde_encoder_phys_cmd, base);
		spin_lock_irqsave(phys->enc_spinlock, flags);
		if (!list_empty(&cmd_enc->te_timestamp_list)) {
			te = list_last_entry(&cmd_enc->te_timestamp_list,
				struct sde_encoder_phys_cmd_te_timestamp, list);
			timestamp = te->timestamp;
		}
		spin_unlock_irqrestore(phys->enc_spinlock, flags);

		if (timestamp) {
			elapsed = ktime_us_delta(ktime_get(), timestamp);
			if (elapsed >= 0) {
				delay = width - (elapsed % period);
				if (delay > 0)
					usleep_range(delay, delay + 100);
			}
		}
	}
	rc = m2468_display_set_brightness(conn->bl_device, brightness);
	if (!rc)
		WRITE_ONCE(conn->bl_need_sync, false);
	return rc;
}

int m2468_display_update_pwm_dc(struct dsi_panel *panel, u32 level)
{
	struct m2468_panel_backlight *bl = &panel->m2468_bl;
	struct dsi_panel_cmd_set *set;
	enum dsi_cmd_set_type cmd;
	u8 *payload;
	u32 type;
	int rc;

	if (!bl->pwm_dc_enabled || !level)
		return 0;

	/* The stock driver changes demura before switching the gamma bank. */
	if (level <= 408 && bl->type != M2468_BACKLIGHT_LOW_PWM) {
		rc = dsi_panel_m2468_tx_cmd_set(panel,
					     DSI_CMD_SET_M2468_DEMURA_PWM_2);
		if (rc)
			return rc;
	} else if (level > 408 && bl->type == M2468_BACKLIGHT_LOW_PWM) {
		rc = dsi_panel_m2468_tx_cmd_set(panel,
					     DSI_CMD_SET_M2468_DEMURA_PWM_1);
		if (rc)
			return rc;
	}

	type = level >= bl->dc_min ? M2468_BACKLIGHT_DC :
		(level <= 408 ? M2468_BACKLIGHT_LOW_PWM : M2468_BACKLIGHT_PWM);
	if ((type == M2468_BACKLIGHT_DC) == (bl->type == M2468_BACKLIGHT_DC)) {
		bl->type = type;
		return 0;
	}

	cmd = type == M2468_BACKLIGHT_DC ? DSI_CMD_SET_M2468_PWM_DC :
		DSI_CMD_SET_M2468_DC_PWM;
	set = &panel->cur_mode->priv_info->cmd_sets[cmd];
	if (!set->cmds || set->count <= 5 ||
	    set->cmds[5].msg.tx_len != 3 || !set->cmds[5].msg.tx_buf)
		return -EINVAL;
	payload = (u8 *)set->cmds[5].msg.tx_buf;
	if (payload[0] != 0x51)
		return -EINVAL;
	payload[1] = level >> 8;
	payload[2] = level;

	rc = m2468_display_wait_for_vsync(panel);
	if (rc)
		return rc;
	rc = dsi_panel_m2468_tx_cmd_set(panel, cmd);
	if (rc)
		return rc;
	m2468_display_frame_delay(panel, 2);
	bl->type = type;
	return 0;
}
