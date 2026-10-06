// SPDX-License-Identifier: GPL-2.0-only
/* M2468 active local HBM. See reports/HBM-IMPLEMENTATION-PLAN.zh-CN.md. */
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include "dsi_display.h"
#include "dsi_panel.h"
#include "dsi_clk.h"
#include "dsi_m2468_hbm.h"

int dsi_m2468_hbm_set_lut(struct dsi_panel *panel, const void *data, size_t size)
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
static DEFINE_MUTEX(m2468_admin_lock);
static DEFINE_MUTEX(m2468_entry_lock);
static DEFINE_SPINLOCK(m2468_ready_lock);
static struct dsi_display *m2468_display;
static struct kobject *m2468_kobj;
static u64 m2468_ready_generation;
static u32 m2468_remaining;
static bool m2468_armed, m2468_ready;

static void m2468_ready_work_fn(struct work_struct *work)
{
	/* Only wake readers; never replay a captured true/false value. */
	mutex_lock(&m2468_entry_lock);
	if (m2468_kobj)
		sysfs_notify(m2468_kobj, NULL, "hbm_ready_status");
	mutex_unlock(&m2468_entry_lock);
}
static DECLARE_WORK(m2468_ready_work, m2468_ready_work_fn);

static void m2468_clear_ready(struct dsi_panel *panel)
{
	unsigned long flags;

	spin_lock_irqsave(&m2468_ready_lock, flags);
	if (m2468_display && m2468_display->panel == panel) {
		m2468_ready_generation++;
		m2468_armed = false;
		m2468_ready = false;
		schedule_work(&m2468_ready_work);
	}
	spin_unlock_irqrestore(&m2468_ready_lock, flags);
}

static void m2468_arm_ready(struct dsi_panel *panel)
{
	unsigned long flags;

	spin_lock_irqsave(&m2468_ready_lock, flags);
	if (m2468_display && m2468_display->panel == panel) {
		m2468_ready_generation = panel->m2468_hbm.state.generation;
		m2468_remaining = panel->m2468_hbm.vblanks;
		m2468_armed = true;
		m2468_ready = false;
	}
	spin_unlock_irqrestore(&m2468_ready_lock, flags);
}

void dsi_m2468_hbm_te(void *display)
{
	unsigned long flags;

	spin_lock_irqsave(&m2468_ready_lock, flags);
	if (m2468_display == display && m2468_armed && m2468_remaining &&
	    !atomic_read(&m2468_display->panel->esd_recovery_pending)) {
		if (!--m2468_remaining) {
			m2468_ready = true;
			m2468_armed = false;
			schedule_work(&m2468_ready_work);
		}
	}
	spin_unlock_irqrestore(&m2468_ready_lock, flags);
}

void dsi_m2468_hbm_invalidate(struct dsi_panel *panel, bool initialized)
{
	if (!panel->m2468_hbm.supported)
		return;
	m2468_clear_ready(panel);
	m2468_hbm_invalidate(&panel->m2468_hbm.state, initialized);
	if (initialized)
		panel->m2468_hbm.low_power = false;
}

void dsi_m2468_hbm_low_power(struct dsi_panel *panel, bool low_power)
{
	if (!panel->m2468_hbm.supported)
		return;
	m2468_clear_ready(panel);
	panel->m2468_hbm.low_power = low_power;
	panel->m2468_hbm.state.generation++;
	panel->m2468_hbm.state.adfr_valid = false;
}

void dsi_m2468_hbm_init(struct dsi_panel *panel)
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
	rc = of_property_read_u32_array(root, "meizu,board-id", board, 2);
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
	panel->m2468_hbm.supported = true;
	panel->m2468_hbm.vblanks = vblanks;
	panel->m2468_hbm.state.dc_min = dc_min;
	/* Splash handoff is not proof of a known local-HBM state. */
	panel->m2468_hbm.state.phase = M2468_HBM_FAULT;
}

static struct dsi_panel_cmd_set *m2468_set(struct dsi_panel *p,
		enum m2468_hbm_command command, u32 value)
{
	enum dsi_cmd_set_type type;

	switch (command) {
	case M2468_LOCAL_ON: type = DSI_CMD_SET_M2468_LOCAL_ON; break;
	case M2468_LOCAL_OFF: type = DSI_CMD_SET_M2468_LOCAL_OFF; break;
	case M2468_LEVEL_ON:
	case M2468_LEVEL_OFF: type = DSI_CMD_SET_M2468_LOCAL_LEVEL; break;
	case M2468_ADFR:
		type = value ? DSI_CMD_SET_M2468_ADFR_ON : DSI_CMD_SET_M2468_ADFR_OFF;
		break;
	default: return NULL;
	}
	return &p->cur_mode->priv_info->cmd_sets[type];
}

static int m2468_check_set(struct dsi_panel_cmd_set *set, u32 count,
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

static int m2468_check_packet(struct dsi_panel_cmd_set *set, u32 index,
		const u8 *data, u32 length)
{
	if (set->count <= index || set->cmds[index].msg.tx_len != length ||
	    memcmp(set->cmds[index].msg.tx_buf, data, length))
		return -EINVAL;
	return 0;
}

static int m2468_validate(struct dsi_panel *p)
{
	struct dsi_panel_cmd_set *set;
	int rc;

	if (!p->panel_initialized || atomic_read(&p->esd_recovery_pending))
		return -EIO;
	if (p->m2468_hbm.low_power || p->power_mode != SDE_MODE_DPMS_ON ||
	    p->panel_mode != DSI_OP_CMD_MODE)
		return -EOPNOTSUPP;
	if (!p->cur_mode || !p->cur_mode->priv_info)
		return -EAGAIN;
	rc = m2468_check_set(m2468_set(p, M2468_LOCAL_ON, 0), 54, DSI_CMD_SET_STATE_HS);
	if (rc)
		return rc;
	set = m2468_set(p, M2468_LOCAL_OFF, 0);
	rc = m2468_check_set(set, 50, DSI_CMD_SET_STATE_HS);
	if (rc)
		return rc;
	rc = m2468_check_packet(set, 10, (const u8 *)"\x88\x00\x01\xf0", 4);
	if (rc)
		return rc;
	set = m2468_set(p, M2468_LEVEL_ON, 0);
	rc = m2468_check_set(set, 3, DSI_CMD_SET_STATE_HS);
	if (rc)
		return rc;
	rc = m2468_check_packet(set, 1, (const u8 *)"\xc2\xdf\xdf\xdf\x6a\x00\x6d", 7);
	if (rc)
		return rc;
	rc = m2468_check_set(m2468_set(p, M2468_ADFR, 0), 0, DSI_CMD_SET_STATE_LP);
	if (rc)
		return rc; /* 144/90Hz: empty, never a successful ADFR-off. */
	set = m2468_set(p, M2468_ADFR, 1);
	rc = m2468_check_set(set, 18, DSI_CMD_SET_STATE_LP);
	if (rc)
		return rc;
	return m2468_check_packet(set, 15, (const u8 *)"\xb1\xff\xff\xff\x77", 5);
}

static int m2468_send(void *ctx, enum m2468_hbm_command command, u32 value)
{
	struct dsi_panel *p = ctx;
	struct m2468_hbm_state *s = &p->m2468_hbm.state;
	struct dsi_panel_cmd_set *set = m2468_set(p, command, value);
	struct dsi_cmd_desc cmd;
	u8 payload[7];
	u32 i;
	int rc;

	for (i = 0; i < set->count; i++) {
		/* Private descriptor/payload: immutable mode tables survive retries. */
		cmd = set->cmds[i];
		cmd.ctrl_flags = 0;
		cmd.msg.flags &= ~MIPI_DSI_MSG_USE_LPM;
		if (set->state == DSI_CMD_SET_STATE_LP)
			cmd.msg.flags |= MIPI_DSI_MSG_USE_LPM;
		if ((command == M2468_LOCAL_OFF && i == 10) ||
		    ((command == M2468_LEVEL_ON || command == M2468_LEVEL_OFF) && i == 1) ||
		    (command == M2468_ADFR && value && i == 15)) {
			memcpy(payload, cmd.msg.tx_buf, cmd.msg.tx_len);
			if (command == M2468_LOCAL_OFF)
				m2468_hbm_demura(s->attempted, s->requested, s->dc_min, payload);
			else if (command == M2468_ADFR)
				payload[4] = m2468_hbm_adfr_byte(value);
			else {
				rc = m2468_hbm_rgb(s->attempted, command == M2468_LEVEL_ON, payload + 1);
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

static int m2468_restore(void *ctx)
{
	struct dsi_panel *p = ctx;
	u32 level = dsi_display_bl_to_panel(p, p->bl_config.bl_level);

	/* DISABLING bypasses the ordinary nonzero HBM gate. No nested setter lock. */
	return dsi_panel_set_backlight(p, level);
}

static void m2468_wait(void *ctx, u32 ms)
{
	usleep_range(ms * 1000, ms * 1000 + 100);
}
static const struct m2468_hbm_ops m2468_ops = {m2468_send, m2468_restore, m2468_wait};

/* display_lock held. No work needing this lock is synchronously cancelled. */
static int m2468_transaction(struct dsi_display *display, bool on, bool adfr, u32 code)
{
	struct dsi_panel *p = display->panel;
	struct m2468_hbm_state *s = &p->m2468_hbm.state;
	int rc, unvote;
	bool newly_on = false;

	if (!p->m2468_hbm.supported || display->trusted_vm_env || !display->hw_ownership)
		return -EOPNOTSUPP;
	if (!on && !adfr) {
		mutex_lock(&p->panel_lock);
		m2468_clear_ready(p);
		s->generation++;
		mutex_unlock(&p->panel_lock);
	}
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		mutex_lock(&p->panel_lock);
		dsi_m2468_hbm_invalidate(p, false);
		mutex_unlock(&p->panel_lock);
		return rc;
	}
	mutex_lock(&p->panel_lock);
	rc = m2468_validate(p);
	if (rc)
		goto out;
	if (adfr) {
		if (s->phase != M2468_HBM_OFF) {
			rc = -EBUSY;
			goto out;
		}
		rc = m2468_send(p, M2468_ADFR, code);
		s->adfr_valid = !rc;
		if (!rc)
			s->adfr = code;
		else
			dsi_m2468_hbm_invalidate(p, false);
	} else {
		if (on && s->phase == M2468_HBM_ON)
			goto out; /* idempotence must not restart the TE count. */
		m2468_clear_ready(p);
		rc = m2468_hbm_run(s, on, p->cur_mode->timing.refresh_rate, &m2468_ops, p);
		newly_on = !rc && on && s->phase == M2468_HBM_ON;
	}
out:
	mutex_unlock(&p->panel_lock);
	unvote = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_CORE_CLK, DSI_CLK_OFF);
	mutex_lock(&p->panel_lock);
	if (unvote) {
		dsi_m2468_hbm_invalidate(p, false);
		if (!rc)
			rc = unvote;
	}
	if (!rc && newly_on && s->phase == M2468_HBM_ON && !p->m2468_hbm.low_power)
		m2468_arm_ready(p);
	mutex_unlock(&p->panel_lock);
	return rc;
}

int dsi_m2468_hbm_quiesce(struct dsi_display *display)
{
	struct dsi_panel *p = display->panel;
	bool on;

	if (!p->m2468_hbm.supported)
		return 0;
	mutex_lock(&p->panel_lock);
	m2468_clear_ready(p);
	on = p->m2468_hbm.state.phase == M2468_HBM_ON;
	mutex_unlock(&p->panel_lock);
	return on ? m2468_transaction(display, false, false, 0) : 0;
}

static ssize_t hbm_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t rc = -ENODEV;
	struct m2468_hbm_state *s;

	mutex_lock(&m2468_entry_lock);
	if (m2468_display) {
		mutex_lock(&m2468_display->panel->panel_lock);
		s = &m2468_display->panel->m2468_hbm.state;
		rc = s->phase == M2468_HBM_FAULT ? -EIO : sysfs_emit(buf, "%u\n", s->phase == M2468_HBM_ON ? 6 : 7);
		mutex_unlock(&m2468_display->panel->panel_lock);
	}
	mutex_unlock(&m2468_entry_lock);
	return rc;
}

static ssize_t m2468_store(const char *buf, size_t count, bool adfr)
{
	u32 value;
	int rc;

	rc = kstrtou32(buf, 10, &value);
	if (rc || (!adfr && value != 6 && value != 7) ||
	    (adfr && value && m2468_hbm_adfr_byte(value) < 0))
		return -EINVAL;
	mutex_lock(&m2468_entry_lock);
	if (!m2468_display) {
		rc = -ENODEV;
		goto out;
	}
	mutex_lock(&m2468_display->display_lock);
	rc = m2468_transaction(m2468_display, value == 6, adfr, value);
	mutex_unlock(&m2468_display->display_lock);
out:
	mutex_unlock(&m2468_entry_lock);
	return rc ? rc : count;
}

static ssize_t hbm_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	return m2468_store(buf, count, false);
}

static ssize_t minfps_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	return m2468_store(buf, count, true);
}

static ssize_t hbm_ready_status_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&m2468_ready_lock, flags);
	ready = m2468_display && m2468_ready;
	spin_unlock_irqrestore(&m2468_ready_lock, flags);
	return sysfs_emit(buf, "%u\n", ready);
}

static struct kobj_attribute hbm_attr = __ATTR_RW(hbm);
static struct kobj_attribute ready_attr = __ATTR_RO(hbm_ready_status);
static struct kobj_attribute minfps_attr = __ATTR_WO(minfps);
static struct attribute *m2468_attrs[] = {&hbm_attr.attr, &ready_attr.attr, &minfps_attr.attr, NULL};
static const struct attribute_group m2468_group = {.attrs = m2468_attrs};

int dsi_m2468_hbm_bind(struct dsi_display *display)
{
	unsigned long flags;
	int rc = 0;

	if (!display->panel->m2468_hbm.supported || display->trusted_vm_env)
		return 0;
	mutex_lock(&m2468_admin_lock);
	mutex_lock(&m2468_entry_lock);
	if (m2468_display) {
		rc = -EBUSY;
		goto out;
	}
	m2468_kobj = kobject_create_and_add("display_drivers", kernel_kobj);
	if (!m2468_kobj) {
		rc = -ENOMEM;
		goto out;
	}
	spin_lock_irqsave(&m2468_ready_lock, flags);
	m2468_display = display;
	m2468_armed = m2468_ready = false;
	spin_unlock_irqrestore(&m2468_ready_lock, flags);
	rc = sysfs_create_group(m2468_kobj, &m2468_group);
	if (rc) {
		spin_lock_irqsave(&m2468_ready_lock, flags);
		m2468_display = NULL;
		spin_unlock_irqrestore(&m2468_ready_lock, flags);
		kobject_put(m2468_kobj);
		m2468_kobj = NULL;
	}
out:
	mutex_unlock(&m2468_entry_lock);
	mutex_unlock(&m2468_admin_lock);
	return rc;
}

void dsi_m2468_hbm_unbind(struct dsi_display *display)
{
	struct kobject *kobj;
	unsigned long flags;

	mutex_lock(&m2468_admin_lock);
	if (m2468_display != display)
		goto out;
	/* Drain active sysfs calls before acquiring any locks they need. */
	kobj = m2468_kobj;
	sysfs_remove_group(kobj, &m2468_group);
	mutex_lock(&m2468_entry_lock);
	mutex_lock(&display->display_lock);
	if (dsi_m2468_hbm_quiesce(display))
		DSI_ERR("M2468 local HBM could not be restored during detach\n");
	mutex_lock(&display->panel->panel_lock);
	dsi_m2468_hbm_invalidate(display->panel, false);
	spin_lock_irqsave(&m2468_ready_lock, flags);
	m2468_display = NULL;
	m2468_kobj = NULL;
	m2468_armed = m2468_ready = false;
	spin_unlock_irqrestore(&m2468_ready_lock, flags);
	mutex_unlock(&display->panel->panel_lock);
	mutex_unlock(&display->display_lock);
	mutex_unlock(&m2468_entry_lock);
	/* IRQ cannot queue after the pointer is cleared under its own lock. */
	cancel_work_sync(&m2468_ready_work);
	kobject_put(kobj);
out:
	mutex_unlock(&m2468_admin_lock);
}
