// SPDX-License-Identifier: GPL-2.0-only
#include "dsi_display.h"
#include "dsi_drm.h"
#include "dsi_panel.h"
#include "dsi_clk.h"
#include "dsi_ctrl.h"
#include "sde_encoder.h"
#include "dsi_m2468_backlight.h"

/* Dedicated allocation/unwind for the five mandatory M2468 tables. */
int dsi_m2468_backlight_parse(struct dsi_panel_cmd_set *set, const void *data, u32 length, bool lp)
{
	const u8 *cursor = data;
	u32 i, count = lp ? 47 : 3, size;
	int rc = m2468_bl_check_blob(data, length, count);

	if (rc)
		return rc;
	set->cmds = kcalloc(count, sizeof(*set->cmds), GFP_KERNEL);
	if (!set->cmds)
		return -ENOMEM;
	set->count = count;
	set->state = lp ? DSI_CMD_SET_STATE_LP : DSI_CMD_SET_STATE_HS;
	for (i = 0; i < count; i++) {
		struct dsi_cmd_desc *cmd = &set->cmds[i];

		size = ((u32)cursor[5] << 8) | cursor[6];
		if (cursor[0] != 0x39 || cursor[2] ||
		    cursor[3] != (i + 1 == count ? 0 : MIPI_DSI_MSG_BATCH_COMMAND)) {
			rc = -EINVAL;
			goto fail;
		}
		cmd->msg.tx_buf = kmemdup(cursor + 7, size, GFP_KERNEL);
		if (!cmd->msg.tx_buf) {
			rc = -ENOMEM;
			goto fail;
		}
		cmd->msg.type = cursor[0];
		cmd->msg.flags = cursor[3];
		cmd->msg.tx_len = size;
		cmd->last_command = i + 1 == count;
		cmd->post_wait_ms = cursor[4];
		cursor += size + 7;
	}
	return 0;
fail:
	for (i = 0; i < count; i++)
		kfree(set->cmds[i].msg.tx_buf);
	kfree(set->cmds);
	set->cmds = NULL;
	set->count = 0;
	return rc;
}

void dsi_m2468_backlight_init(struct dsi_panel *panel)
{
	struct dsi_parser_utils *u = &panel->utils;
	u32 enabled, minimum;

	/* HBM's exact name/node/board profile is the common M2468 identity. */
	if (!panel->m2468_hbm.supported)
		return;
	panel->m2468_bl.configured =
		!u->read_u32(u->data, "qcom,mdss-pwm-dc-siwtch", &enabled) && enabled == 1 &&
		!u->read_u32(u->data, "qcom,mdss-dc-min-brightness", &minimum) && minimum == 1107;
}

void dsi_m2468_backlight_invalidate(struct dsi_panel *panel)
{
	panel->m2468_bl.state.valid = false;
}

static struct dsi_panel_cmd_set *m2468_bl_set(struct dsi_panel *p,
					     enum m2468_bl_command command)
{
	static const enum dsi_cmd_set_type types[] = {
		DSI_CMD_SET_M2468_PWM_DC, DSI_CMD_SET_M2468_DC_PWM,
		DSI_CMD_SET_M2468_DEMURA_DC, DSI_CMD_SET_M2468_DEMURA_MID,
		DSI_CMD_SET_M2468_DEMURA_LOW,
	};

	return &p->cur_mode->priv_info->cmd_sets[types[command]];
}

static int m2468_bl_packet(struct dsi_panel_cmd_set *set, u32 index,
			  const u8 *data, u32 length)
{
	if (set->count <= index || set->cmds[index].msg.tx_len != length ||
	    memcmp(set->cmds[index].msg.tx_buf, data, length))
		return -EINVAL;
	return 0;
}

static int m2468_bl_validate(struct dsi_panel *p, u32 level)
{
	struct dsi_panel_cmd_set *set;
	const u8 *comp[] = {(const u8 *)"\x88\x00\x01\xf0",
		(const u8 *)"\x88\x20\x40\xf0", (const u8 *)"\x88\x40\x80\xf0"};
	u32 i, j;
	int rc;

	if (!p->m2468_bl.configured)
		return -EINVAL;
	if (!p->panel_initialized || atomic_read(&p->esd_recovery_pending))
		return -EIO;
	if (p->panel_mode != DSI_OP_CMD_MODE ||
	    (level && (p->m2468_hbm.low_power || p->power_mode != SDE_MODE_DPMS_ON)) ||
	    (p->power_mode != SDE_MODE_DPMS_ON && p->power_mode != SDE_MODE_DPMS_LP1 &&
	     p->power_mode != SDE_MODE_DPMS_LP2))
		return -EOPNOTSUPP;
	if (!p->cur_mode || !p->cur_mode->priv_info)
		return -EAGAIN;
	for (i = 0; i < 5; i++) {
		set = m2468_bl_set(p, i);
		if (!set->cmds || set->count != (i < 2 ? 47 : 3) ||
		    set->state != (i < 2 ? DSI_CMD_SET_STATE_LP : DSI_CMD_SET_STATE_HS))
			return -EINVAL;
		for (j = 0; j < set->count; j++)
			if (!set->cmds[j].msg.tx_buf || !set->cmds[j].msg.tx_len ||
			    set->cmds[j].msg.tx_len > 64 ||
			    set->cmds[j].msg.channel || set->cmds[j].ctrl ||
			    set->cmds[j].msg.type != 0x39 ||
			    set->cmds[j].msg.flags !=
			    (j + 1 == set->count ? 0 : MIPI_DSI_MSG_BATCH_COMMAND) ||
			    set->cmds[j].last_command != (j + 1 == set->count))
				return -EINVAL;
		if (i < 2) {
			rc = m2468_bl_packet(set, 4, (const u8 *)"\xff\x08\x38\x00", 4);
			if (!rc)
				rc = m2468_bl_packet(set, 5, i ? (const u8 *)"\x51\x04\x52" :
					(const u8 *)"\x51\x04\x53", 3);
			if (!rc)
				rc = m2468_bl_packet(set, 6, (const u8 *)"\xff\x08\x38\x53", 4);
			if (!rc)
				rc = m2468_bl_packet(set, 7, comp[i], 4);
			if (!rc)
				rc = m2468_bl_packet(set, 46, (const u8 *)"\xff\x08\x38\x00", 4);
		} else {
			rc = m2468_bl_packet(set, 0, (const u8 *)"\xff\x08\x38\x53", 4);
			if (!rc)
				rc = m2468_bl_packet(set, 1, comp[i - 2], 4);
			if (!rc)
				rc = m2468_bl_packet(set, 2, (const u8 *)"\xff\x08\x38\x00", 4);
		}
		if (rc)
			return rc;
	}
	return 0;
}

static int m2468_bl_send(void *ctx, enum m2468_bl_command command, u32 level)
{
	struct dsi_panel *p = ctx;
	struct dsi_panel_cmd_set *set = m2468_bl_set(p, command);
	struct dsi_cmd_desc cmd;
	u8 payload[3] = {0x51, level >> 8, level & 0xff};
	u32 i;
	int rc;

	for (i = 0; i < set->count; i++) {
		cmd = set->cmds[i];
		cmd.ctrl_flags = 0;
		cmd.msg.flags &= ~MIPI_DSI_MSG_USE_LPM;
		if (set->state == DSI_CMD_SET_STATE_LP)
			cmd.msg.flags |= MIPI_DSI_MSG_USE_LPM;
		if (command <= M2468_BL_PWM && i == 5)
			cmd.msg.tx_buf = payload;
		if (atomic_read(&p->esd_recovery_pending))
			return -EIO;
		rc = dsi_host_transfer_sub(p->host, &cmd);
		/* Private host ABI: zero, not byte count, is success. */
		if (rc || atomic_read(&p->esd_recovery_pending))
			return rc < 0 ? rc : -EIO;
		if (cmd.post_wait_ms)
			usleep_range(cmd.post_wait_ms * 1000, cmd.post_wait_ms * 1000 + 100);
	}
	return 0;
}

static int m2468_bl_brightness(void *ctx, u32 level)
{
	struct dsi_panel *p = ctx;
	u8 payload[3] = {0x51, level >> 8, level & 0xff};
	struct dsi_cmd_desc cmd = { .msg = {
		.type = 0x39, .tx_buf = payload, .tx_len = sizeof(payload),
	}, .last_command = true };
	int rc;

	if (p->bl_config.lp_mode)
		cmd.msg.flags |= MIPI_DSI_MSG_USE_LPM;
	/* Match the kernel DCS helper (LE), including the panel inversion. */
	if (!p->bl_config.bl_inverted_dbv) {
		payload[1] = level & 0xff;
		payload[2] = level >> 8;
	}
	if (atomic_read(&p->esd_recovery_pending))
		return -EIO;
	rc = dsi_host_transfer_sub(p->host, &cmd);
	return rc < 0 ? rc : rc || atomic_read(&p->esd_recovery_pending) ? -EIO : 0;
}

static int m2468_bl_sync(void *ctx)
{
	struct dsi_panel *p = ctx;
	struct dsi_display *d = container_of(p->host, struct dsi_display, host);
	struct drm_encoder *encoder;
	int rc;

	/* Stock PWM/DC switches wake SDE synchronously, then wait for its
	 * RD_PTR vblank event. GPIO IRQ allocation remuxes the shared TE pad,
	 * while passive GPIO sampling can miss short pulses during wakeup.
	 */
	if (!d->bridge)
		return -ENODEV;
	encoder = d->bridge->base.encoder;
	if (!encoder)
		return -ENODEV;
	if (atomic_read(&p->esd_recovery_pending))
		return -EIO;
	rc = sde_encoder_m2468_early_wakeup(encoder);
	if (rc)
		return rc;
	rc = sde_encoder_wait_for_event(encoder, MSM_ENC_VBLANK);
	if (atomic_read(&p->esd_recovery_pending))
		return -EIO;
	/* The physical encoder has not been enabled during first handoff.
	 * Stock ignores this wait result and still sends the PWM/DC table.
	 */
	return rc == -EWOULDBLOCK ? 0 : rc;
}

static void m2468_bl_wait(void *ctx, u32 ms)
{
	(void)ctx;
	usleep_range(ms * 1000, ms * 1000 + 100);
}

static const struct m2468_bl_ops m2468_bl_ops = {
	m2468_bl_send, m2468_bl_sync, m2468_bl_brightness, m2468_bl_wait,
};

static void m2468_bl_abort_pending(struct dsi_display *d)
{
	/* Host errors already unwind an acquired transfer vote. Only an ESD
	 * arriving between buffered packets can leave this batch outstanding.
	 */
	if (d->ctrl[0].ctrl && d->ctrl[0].ctrl->cmd_len) {
		if (!(d->ctrl[0].ctrl->pending_cmd_flags & DSI_CTRL_CMD_LAST_COMMAND))
			dsi_ctrl_transfer_cleanup(d->ctrl[0].ctrl);
		d->ctrl[0].ctrl->cmd_len = 0;
	}
}

int dsi_m2468_backlight_set(struct dsi_panel *p, u32 panel_level)
{
	struct dsi_display *d = container_of(p->host, struct dsi_display, host);
	struct m2468_hbm_state *hbm = &p->m2468_hbm.state;
	bool use_adfr, restore_adfr = false;
	u32 saved_adfr = 0, hz;
	int rc, unvote, restore_rc;

	lockdep_assert_held(&p->panel_lock);
	if (!d->hw_ownership || d->trusted_vm_env || d->ctrl_count != 1) {
		rc = -EOPNOTSUPP;
		goto fail;
	}
	if (p->m2468_hbm.low_power || p->power_mode != SDE_MODE_DPMS_ON)
		dsi_m2468_backlight_invalidate(p);
	rc = m2468_bl_validate(p, panel_level);
	if (rc)
		goto fail;
	/* DSI clock callbacks do not take panel_lock. Same ordering as ESD;
	 * hold both core/link resources over TE, every packet and stock delay.
	 */
	rc = dsi_display_clk_ctrl(d->dsi_clk_handle, DSI_ALL_CLKS, DSI_CLK_ON);
	if (rc)
		goto fail;
	hz = p->cur_mode->timing.refresh_rate;
	use_adfr = panel_level && (!p->m2468_bl.state.valid ||
			p->m2468_bl.state.dc != (panel_level >= 1107)) &&
			(hz == 120 || hz == 60 || hz == 30);
	/* Stock tactics2/tactics3 surround a full switch with ADFR OFF/restore.
	 * An unknown value is never guessed: establish OFF without a replay.
	 */
	if (use_adfr && (!hbm->adfr_valid || hbm->adfr)) {
		saved_adfr = hbm->adfr_valid ? hbm->adfr : 0;
		rc = dsi_m2468_hbm_backlight_adfr(p, 0);
		restore_adfr = !rc && saved_adfr;
	}
	if (!rc)
		rc = m2468_bl_run(&p->m2468_bl.state, panel_level, hz, &m2468_bl_ops, p);
	if (rc || atomic_read(&p->esd_recovery_pending))
		m2468_bl_abort_pending(d);
	if (restore_adfr) {
		restore_rc = dsi_m2468_hbm_backlight_adfr(p, saved_adfr);
		if (restore_rc || atomic_read(&p->esd_recovery_pending))
			m2468_bl_abort_pending(d);
		if (!rc)
			rc = restore_rc;
	}
	unvote = dsi_display_clk_ctrl(d->dsi_clk_handle, DSI_ALL_CLKS, DSI_CLK_OFF);
	if (!rc)
		rc = unvote;
	if (rc || atomic_read(&p->esd_recovery_pending))
		goto fail;
	return 0;
fail:
	dsi_m2468_backlight_invalidate(p);
	return rc < 0 ? rc : -EIO;
}
