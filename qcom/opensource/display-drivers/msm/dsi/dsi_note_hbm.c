// SPDX-License-Identifier: GPL-2.0-only
/* M2468 active local HBM. See reports/HBM-IMPLEMENTATION-PLAN.zh-CN.md. */
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include "dsi_display.h"
#include "dsi_panel.h"
#include "dsi_clk.h"
#include "dsi_note_hbm.h"

int dsi_note_hbm_set_lut(struct dsi_panel *panel, const void *data, size_t size)
{
	const struct drm_msm_dimming_bl_lut *input = data;
	struct drm_msm_dimming_bl_lut *copy = NULL, *old;
	size_t length;

	/* Atomic state owns the input blob; never retain its unreferenced data. */
	if (data) {
		if (size < sizeof(input->length) || input->length > DIMMING_BL_LUT_LEN)
			return -EINVAL;
		length = sizeof(input->length) + input->length * sizeof(input->mapped_bl[0]);
		if (size < length)
			return -EINVAL;
		copy = kzalloc(sizeof(*copy), GFP_KERNEL);
		if (!copy)
			return -ENOMEM;
		memcpy(copy, data, length);
	} else if (size) {
		return -EINVAL;
	}
	mutex_lock(&panel->panel_lock);
	old = panel->bl_config.dimming_bl_lut;
	panel->bl_config.dimming_bl_lut = copy;
	mutex_unlock(&panel->panel_lock);
	kfree(old);
	return 0;
}

/* One Android primary-display ABI. No unprotected global panel pointer. */
static DEFINE_MUTEX(note_admin_lock);
static DEFINE_MUTEX(note_entry_lock);
static DEFINE_SPINLOCK(note_ready_lock);
static struct dsi_display *note_display;
static struct kobject *note_kobj;
static u64 note_ready_generation;
static u32 note_remaining;
static bool note_armed, note_ready;

static void note_ready_work_fn(struct work_struct *work)
{
	/* Only wake readers; never replay a captured true/false value. */
	mutex_lock(&note_entry_lock);
	if (note_kobj)
		sysfs_notify(note_kobj, NULL, "hbm_ready_status");
	mutex_unlock(&note_entry_lock);
}
static DECLARE_WORK(note_ready_work, note_ready_work_fn);

static void note_clear_ready(struct dsi_panel *panel)
{
	unsigned long flags;

	spin_lock_irqsave(&note_ready_lock, flags);
	if (note_display && note_display->panel == panel) {
		note_ready_generation++;
		note_armed = false;
		note_ready = false;
		schedule_work(&note_ready_work);
	}
	spin_unlock_irqrestore(&note_ready_lock, flags);
}

static void note_arm_ready(struct dsi_panel *panel)
{
	unsigned long flags;

	spin_lock_irqsave(&note_ready_lock, flags);
	if (note_display && note_display->panel == panel) {
		note_ready_generation = panel->note_hbm.state.generation;
		note_remaining = panel->note_hbm.vblanks;
		note_armed = true;
		note_ready = false;
	}
	spin_unlock_irqrestore(&note_ready_lock, flags);
}

void dsi_note_hbm_te(void *display)
{
	unsigned long flags;

	spin_lock_irqsave(&note_ready_lock, flags);
	if (note_display == display && note_armed && note_remaining &&
	    !atomic_read(&note_display->panel->esd_recovery_pending)) {
		if (!--note_remaining) {
			note_ready = true;
			note_armed = false;
			schedule_work(&note_ready_work);
		}
	}
	spin_unlock_irqrestore(&note_ready_lock, flags);
}

void dsi_note_hbm_invalidate(struct dsi_panel *panel, bool initialized)
{
	if (!panel->note_hbm.supported)
		return;
	dsi_note_backlight_invalidate(panel);
	note_clear_ready(panel);
	note_hbm_invalidate(&panel->note_hbm.state, initialized);
	if (initialized)
		panel->note_hbm.low_power = false;
}

void dsi_note_hbm_low_power(struct dsi_panel *panel, bool low_power)
{
	if (!panel->note_hbm.supported)
		return;
	dsi_note_backlight_invalidate(panel);
	note_clear_ready(panel);
	panel->note_hbm.low_power = low_power;
	panel->note_hbm.state.generation++;
	panel->note_hbm.state.adfr_valid = false;
}

void dsi_note_hbm_init(struct dsi_panel *panel)
{
	struct device_node *root;
	struct dsi_parser_utils *u = &panel->utils;
	u32 board[2], ofp, enabled, dc_min, adfr, vblanks;
	int rc;

	if (strcmp(panel->name, "ILI7838E_TIANMA_MEIZU") ||
	    !panel->panel_of_node || strcmp(panel->panel_of_node->name,
					"qcom,mdss_dsi_xjmz_amoled_tianma_cmd"))
		return;
	root = of_find_node_by_path("/");
	if (!root)
		return;
	rc = of_property_count_u32_elems(root, "meizu,board-id") == 2 ?
		of_property_read_u32_array(root, "meizu,board-id", board, 2) : -EINVAL;
	of_node_put(root);
	if (rc || board[0] != 3 || board[1] != 5)
		return;
	if (u->read_u32(u->data, "xjmz,display-ofp-type-config", &ofp) || ofp != 2 ||
	    u->read_u32(u->data, "xjmz,display-ofp-enable-config", &enabled) || enabled != 1 ||
	    u->read_u32(u->data, "qcom,display-adfr-config", &adfr) || adfr != 1 ||
	    u->read_u32(u->data, "qcom,mdss-dc-min-brightness", &dc_min) || dc_min != 1107 ||
	    u->read_u32(u->data, "xjmz,display-ofp-hbm-take-effect-vblank-config", &vblanks) ||
	    vblanks != 1 || panel->bl_config.bl_max_level != 4095 ||
	    panel->bl_config.type != DSI_BACKLIGHT_DCS)
		return;
	panel->note_hbm.supported = true;
	panel->note_hbm.vblanks = vblanks;
	panel->note_hbm.state.dc_min = dc_min;
	/* Splash handoff is not proof of a known local-HBM state. */
	panel->note_hbm.state.phase = NOTE_HBM_FAULT;
}

static struct dsi_panel_cmd_set *note_set(struct dsi_panel *p,
		enum note_hbm_command command, u32 value)
{
	enum dsi_cmd_set_type type;

	switch (command) {
	case NOTE_LOCAL_ON: type = DSI_CMD_SET_NOTE_LOCAL_ON; break;
	case NOTE_LOCAL_OFF: type = DSI_CMD_SET_NOTE_LOCAL_OFF; break;
	case NOTE_LEVEL_ON:
	case NOTE_LEVEL_OFF: type = DSI_CMD_SET_NOTE_LOCAL_LEVEL; break;
	case NOTE_ADFR:
		type = value ? DSI_CMD_SET_NOTE_ADFR_ON : DSI_CMD_SET_NOTE_ADFR_OFF;
		break;
	default: return NULL;
	}
	return &p->cur_mode->priv_info->cmd_sets[type];
}

static int note_check_set(struct dsi_panel_cmd_set *set, u32 count,
		enum dsi_cmd_set_state state)
{
	u32 i;

	if (!set || !set->cmds || !set->count)
		return -EOPNOTSUPP;
	if ((count && set->count != count) || set->state != state)
		return -EINVAL;
	for (i = 0; i < set->count; i++)
		if (!set->cmds[i].msg.tx_buf || !set->cmds[i].msg.tx_len)
			return -EINVAL;
	return 0;
}

static int note_check_packet(struct dsi_panel_cmd_set *set, u32 index,
		const u8 *data, u32 length)
{
	if (set->count <= index || set->cmds[index].msg.tx_len != length ||
	    memcmp(set->cmds[index].msg.tx_buf, data, length))
		return -EINVAL;
	return 0;
}

static int note_validate(struct dsi_panel *p)
{
	struct dsi_panel_cmd_set *set;
	int rc;

	if (!p->panel_initialized || atomic_read(&p->esd_recovery_pending))
		return -EIO;
	if (p->note_hbm.low_power || p->power_mode != SDE_MODE_DPMS_ON ||
	    p->panel_mode != DSI_OP_CMD_MODE)
		return -EOPNOTSUPP;
	if (!p->cur_mode || !p->cur_mode->priv_info)
		return -EAGAIN;
	rc = note_check_set(note_set(p, NOTE_LOCAL_ON, 0), 54, DSI_CMD_SET_STATE_HS);
	if (rc)
		return rc;
	set = note_set(p, NOTE_LOCAL_OFF, 0);
	rc = note_check_set(set, 50, DSI_CMD_SET_STATE_HS);
	if (rc)
		return rc;
	rc = note_check_packet(set, 10, (const u8 *)"\x88\x00\x01\xf0", 4);
	if (rc)
		return rc;
	set = note_set(p, NOTE_LEVEL_ON, 0);
	rc = note_check_set(set, 3, DSI_CMD_SET_STATE_HS);
	if (rc)
		return rc;
	rc = note_check_packet(set, 1, (const u8 *)"\xc2\xdf\xdf\xdf\x6a\x00\x6d", 7);
	if (rc)
		return rc;
	/* Note's 144/90Hz modes have local-HBM tables but no ADFR pair.
	 * Select a transaction without ADFR; do not send an empty command
	 * set or claim that an ADFR setting was programmed.
	 */
	if (p->cur_mode->timing.refresh_rate == 144 ||
	    p->cur_mode->timing.refresh_rate == 90)
		return note_set(p, NOTE_ADFR, 0)->count ||
			note_set(p, NOTE_ADFR, 1)->count ? -EINVAL : 0;
	if (p->cur_mode->timing.refresh_rate != 120 &&
	    p->cur_mode->timing.refresh_rate != 60 &&
	    p->cur_mode->timing.refresh_rate != 30)
		return -EOPNOTSUPP;
	rc = note_check_set(note_set(p, NOTE_ADFR, 0), 0, DSI_CMD_SET_STATE_LP);
	if (rc)
		return rc;
	set = note_set(p, NOTE_ADFR, 1);
	rc = note_check_set(set, 18, DSI_CMD_SET_STATE_LP);
	if (rc)
		return rc;
	return note_check_packet(set, 15, (const u8 *)"\xb1\xff\xff\xff\x77", 5);
}

static int note_send(void *ctx, enum note_hbm_command command, u32 value)
{
	struct dsi_panel *p = ctx;
	struct note_hbm_state *s = &p->note_hbm.state;
	struct dsi_panel_cmd_set *set = note_set(p, command, value);
	struct dsi_cmd_desc cmd;
	u8 payload[7];
	u32 i;
	int rc;

	if (command == NOTE_LOCAL_ON || command == NOTE_LOCAL_OFF)
		dsi_note_backlight_invalidate(p);
	for (i = 0; i < set->count; i++) {
		/* Private descriptor/payload: immutable mode tables survive retries. */
		cmd = set->cmds[i];
		cmd.ctrl_flags = 0;
		cmd.msg.flags &= ~MIPI_DSI_MSG_USE_LPM;
		if (set->state == DSI_CMD_SET_STATE_LP)
			cmd.msg.flags |= MIPI_DSI_MSG_USE_LPM;
		if ((command == NOTE_LOCAL_OFF && i == 10) ||
		    ((command == NOTE_LEVEL_ON || command == NOTE_LEVEL_OFF) && i == 1) ||
		    (command == NOTE_ADFR && value && i == 15)) {
			memcpy(payload, cmd.msg.tx_buf, cmd.msg.tx_len);
			if (command == NOTE_LOCAL_OFF)
				note_hbm_demura(s->attempted, s->requested, s->dc_min, payload);
			else if (command == NOTE_ADFR)
				payload[4] = note_hbm_adfr_byte(value);
			else {
				rc = note_hbm_rgb(s->attempted, command == NOTE_LEVEL_ON, payload + 1);
				if (rc)
					return rc;
			}
			cmd.msg.tx_buf = payload;
		}
		/* Generic host treats ESD-skipped transfers as zero: reject here. */
		if (atomic_read(&p->esd_recovery_pending))
			return -EIO;
		rc = dsi_host_transfer_sub(p->host, &cmd);
		if (rc)
			return rc < 0 ? rc : -EIO;
		if (cmd.post_wait_ms)
			usleep_range(cmd.post_wait_ms * 1000, cmd.post_wait_ms * 1000 + 10);
	}
	return 0;
}

int dsi_note_hbm_backlight_adfr(struct dsi_panel *p, u32 value)
{
	struct note_hbm_state *s = &p->note_hbm.state;
	int rc;

	/* Fixed 144/90Hz modes must not even query these optional tables. */
	if (!p->cur_mode ||
	    (p->cur_mode->timing.refresh_rate != 120 &&
	     p->cur_mode->timing.refresh_rate != 60 &&
	     p->cur_mode->timing.refresh_rate != 30))
		return -EOPNOTSUPP;
	rc = note_check_set(note_set(p, NOTE_ADFR, 0), 9, DSI_CMD_SET_STATE_LP);
	if (!rc)
		rc = note_check_set(note_set(p, NOTE_ADFR, 1), 18, DSI_CMD_SET_STATE_LP);
	if (!rc)
		rc = note_check_packet(note_set(p, NOTE_ADFR, 1), 15,
				       (const u8 *)"\xb1\xff\xff\xff\x77", 5);
	if (!rc && value && note_hbm_adfr_byte(value) < 0)
		rc = -EINVAL;
	if (!rc)
		rc = note_send(p, NOTE_ADFR, value);
	if (!rc && atomic_read(&p->esd_recovery_pending))
		rc = -EIO;
	s->adfr_valid = !rc;
	if (!rc)
		s->adfr = value;
	return rc < 0 ? rc : rc ? -EIO : 0;
}

static int note_restore(void *ctx)
{
	struct dsi_panel *p = ctx;
	u32 level = dsi_display_bl_to_panel(p, p->bl_config.bl_level);

	/* DISABLING bypasses the ordinary nonzero HBM gate. No nested setter lock. */
	return dsi_panel_set_backlight(p, level);
}

static void note_wait(void *ctx, u32 ms)
{
	usleep_range(ms * 1000, ms * 1000 + 100);
}
static const struct note_hbm_ops note_ops = {note_send, note_restore, note_wait};

/* display_lock held. No work needing this lock is synchronously cancelled. */
static int note_transaction(struct dsi_display *display, bool on, bool adfr, u32 code)
{
	struct dsi_panel *p = display->panel;
	struct note_hbm_state *s = &p->note_hbm.state;
	int rc, unvote;
	bool newly_on = false, use_adfr;

	if (!p->note_hbm.supported || display->trusted_vm_env || !display->hw_ownership)
		return -EOPNOTSUPP;
	if (!on && !adfr) {
		mutex_lock(&p->panel_lock);
		note_clear_ready(p);
		s->generation++;
		/* OFF is established by a completed panel ON or HBM restore.
		 * No transport is needed, even if this mode has no ADFR table.
		 * FAULT must still take the error path below.
		 */
		if (s->phase == NOTE_HBM_OFF) {
			mutex_unlock(&p->panel_lock);
			return 0;
		}
		mutex_unlock(&p->panel_lock);
	}
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		mutex_lock(&p->panel_lock);
		dsi_note_hbm_invalidate(p, false);
		mutex_unlock(&p->panel_lock);
		return rc;
	}
	mutex_lock(&p->panel_lock);
	rc = note_validate(p);
	if (rc)
		goto out;
	use_adfr = p->cur_mode->timing.refresh_rate != 144 &&
		p->cur_mode->timing.refresh_rate != 90;
	if (adfr) {
		if (!use_adfr) {
			rc = -EOPNOTSUPP;
			goto out;
		}
		if (s->phase != NOTE_HBM_OFF) {
			rc = -EBUSY;
			goto out;
		}
		rc = note_send(p, NOTE_ADFR, code);
		s->adfr_valid = !rc;
		if (!rc)
			s->adfr = code;
		else
			dsi_note_hbm_invalidate(p, false);
	} else {
		if (on && s->phase == NOTE_HBM_ON)
			goto out; /* idempotence must not restart the TE count. */
		note_clear_ready(p);
		rc = note_hbm_run(s, on, p->cur_mode->timing.refresh_rate,
				use_adfr, &note_ops, p);
		newly_on = !rc && on && s->phase == NOTE_HBM_ON;
	}
out:
	mutex_unlock(&p->panel_lock);
	unvote = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_CORE_CLK, DSI_CLK_OFF);
	mutex_lock(&p->panel_lock);
	if (unvote) {
		dsi_note_hbm_invalidate(p, false);
		if (!rc)
			rc = unvote;
	}
	if (!rc && newly_on && s->phase == NOTE_HBM_ON && !p->note_hbm.low_power)
		note_arm_ready(p);
	mutex_unlock(&p->panel_lock);
	return rc;
}

int dsi_note_hbm_quiesce(struct dsi_display *display)
{
	struct dsi_panel *p = display->panel;
	bool on;

	if (!p->note_hbm.supported)
		return 0;
	mutex_lock(&p->panel_lock);
	note_clear_ready(p);
	on = p->note_hbm.state.phase == NOTE_HBM_ON;
	mutex_unlock(&p->panel_lock);
	return on ? note_transaction(display, false, false, 0) : 0;
}

static ssize_t hbm_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t rc = -ENODEV;
	struct note_hbm_state *s;

	mutex_lock(&note_entry_lock);
	if (note_display) {
		mutex_lock(&note_display->panel->panel_lock);
		s = &note_display->panel->note_hbm.state;
		rc = s->phase == NOTE_HBM_FAULT ? -EIO : sysfs_emit(buf, "%u\n", s->phase == NOTE_HBM_ON ? 6 : 7);
		mutex_unlock(&note_display->panel->panel_lock);
	}
	mutex_unlock(&note_entry_lock);
	return rc;
}

static ssize_t note_store(const char *buf, size_t count, bool adfr)
{
	u32 value;
	int rc;

	rc = kstrtou32(buf, 10, &value);
	if (rc || (!adfr && value != 6 && value != 7) ||
	    (adfr && value && note_hbm_adfr_byte(value) < 0))
		return -EINVAL;
	mutex_lock(&note_entry_lock);
	if (!note_display) {
		rc = -ENODEV;
		goto out;
	}
	mutex_lock(&note_display->display_lock);
	rc = note_transaction(note_display, value == 6, adfr, value);
	mutex_unlock(&note_display->display_lock);
out:
	mutex_unlock(&note_entry_lock);
	return rc ? rc : count;
}

static ssize_t hbm_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	return note_store(buf, count, false);
}

static ssize_t minfps_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	return note_store(buf, count, true);
}

static ssize_t hbm_ready_status_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&note_ready_lock, flags);
	ready = note_display && note_ready;
	spin_unlock_irqrestore(&note_ready_lock, flags);
	return sysfs_emit(buf, "%u\n", ready);
}

static struct kobj_attribute hbm_attr = __ATTR_RW(hbm);
static struct kobj_attribute ready_attr = __ATTR_RO(hbm_ready_status);
static struct kobj_attribute minfps_attr = __ATTR_WO(minfps);
static struct attribute *note_attrs[] = {&hbm_attr.attr, &ready_attr.attr, &minfps_attr.attr, NULL};
static const struct attribute_group note_group = {.attrs = note_attrs};

int dsi_note_hbm_bind(struct dsi_display *display)
{
	unsigned long flags;
	int rc = 0;

	if (!display->panel->note_hbm.supported || display->trusted_vm_env)
		return 0;
	mutex_lock(&note_admin_lock);
	mutex_lock(&note_entry_lock);
	if (note_display) {
		rc = -EBUSY;
		goto out;
	}
	note_kobj = kobject_create_and_add("display_drivers", kernel_kobj);
	if (!note_kobj) {
		rc = -ENOMEM;
		goto out;
	}
	spin_lock_irqsave(&note_ready_lock, flags);
	note_display = display;
	note_armed = note_ready = false;
	spin_unlock_irqrestore(&note_ready_lock, flags);
	rc = sysfs_create_group(note_kobj, &note_group);
	if (rc) {
		spin_lock_irqsave(&note_ready_lock, flags);
		note_display = NULL;
		spin_unlock_irqrestore(&note_ready_lock, flags);
		kobject_put(note_kobj);
		note_kobj = NULL;
	}
out:
	mutex_unlock(&note_entry_lock);
	mutex_unlock(&note_admin_lock);
	return rc;
}

void dsi_note_hbm_unbind(struct dsi_display *display)
{
	struct kobject *kobj;
	unsigned long flags;

	mutex_lock(&note_admin_lock);
	if (note_display != display)
		goto out;
	/* Drain active sysfs calls before acquiring any locks they need. */
	kobj = note_kobj;
	sysfs_remove_group(kobj, &note_group);
	mutex_lock(&note_entry_lock);
	mutex_lock(&display->display_lock);
	if (dsi_note_hbm_quiesce(display))
		DSI_ERR("Note local HBM could not be restored during detach\n");
	mutex_lock(&display->panel->panel_lock);
	dsi_note_hbm_invalidate(display->panel, false);
	spin_lock_irqsave(&note_ready_lock, flags);
	note_display = NULL;
	note_kobj = NULL;
	note_armed = note_ready = false;
	spin_unlock_irqrestore(&note_ready_lock, flags);
	mutex_unlock(&display->panel->panel_lock);
	mutex_unlock(&display->display_lock);
	mutex_unlock(&note_entry_lock);
	/* IRQ cannot queue after the pointer is cleared under its own lock. */
	cancel_work_sync(&note_ready_work);
	kobject_put(kobj);
out:
	mutex_unlock(&note_admin_lock);
}
