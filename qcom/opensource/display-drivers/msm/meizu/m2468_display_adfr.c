// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "../dsi/dsi_clk.h"
#include "../dsi/dsi_display.h"
#include "../dsi/dsi_panel.h"
#include "m2468_display_adfr.h"
#include "m2468_display_brightness.h"
#include "m2468_display_hbm.h"

#define M2468_ADFR_PANEL_NAME "ILI7838E_TIANMA_MEIZU"
#define M2468_ADFR_IDLE_DELAY_NS (1500 * NSEC_PER_MSEC)

struct m2468_dynamic_te_filter {
	u8 refresh_rate_mid;
	u8 refresh_rate_high;
	u8 refresh_rate_high_120hz_count;
	u8 refresh_rate_high_60hz_count;
	u8 refresh_rate_high_30hz_count;
	u8 refresh_rate_mid_20hz_count;
	u32 refresh_rate;
};

struct m2468_display_adfr_state {
	struct dsi_display *display;
	struct kobject *kobj;
	struct kthread_worker worker;
	struct task_struct *worker_task;
	struct kthread_work drop_work;
	struct hrtimer drop_timer;
	struct m2468_dynamic_te_filter dynamic_filter;
	ktime_t last_te_timestamp;
	atomic_t supported;
	atomic_t min_fps;
	atomic_t last_min_fps;
	atomic_t hbm_saved_min_fps;
	atomic_t fp_saved_min_fps;
	atomic_t hbm_suspended;
	atomic_t backlight_type;
	atomic_t timer_active;
	atomic_t exit_idle_count;
	atomic_t dynamic_te_enabled;
	atomic_t dynamic_te_read_count;
	atomic_t dynamic_te_read_rate;
	atomic_t enter_1hz_count;
	atomic_t aod_light_mode;
	atomic_t aod_presets_light_mode;
	atomic_t full_aod_enable;
	atomic_t debug_mode;
	atomic_t debug_saved_min_fps;
	int dynamic_te_gpio;
	int dynamic_te_irq;
	u32 normal_bl_max;
	u32 pending_backlight_type;
	bool dynamic_te_irq_registered;
	bool dynamic_te_irq_enabled;
	bool initialized;
};

static DEFINE_MUTEX(m2468_adfr_lifecycle_lock);
static DEFINE_MUTEX(m2468_star_lock);
static struct m2468_display_adfr_state m2468_adfr = {
	.dynamic_te_gpio = -EINVAL,
	.dynamic_te_irq = -EINVAL,
};
static char m2468_star_brightness[128] = "0 0.0";

static bool m2468_adfr_panel_supported(const struct dsi_panel *panel)
{
	return panel && panel->name &&
		!strcmp(panel->name, M2468_ADFR_PANEL_NAME);
}

static bool m2468_adfr_min_fps_valid(u32 min_fps)
{
	switch (min_fps) {
	case 0:
	case 1:
	case 16:
	case 32:
	case 48:
	case 64:
	case 96:
		return true;
	default:
		return false;
	}
}

static u8 m2468_adfr_min_fps_code(u32 min_fps)
{
	switch (min_fps) {
	case 1:
		return 0x77;
	case 16:
		return 0x0b;
	case 32:
		return 0x05;
	case 48:
		return 0x03;
	case 64:
		return 0x02;
	case 96:
		return 0x01;
	default:
		return 0x00;
	}
}

static int m2468_adfr_patch_min_fps(struct dsi_panel_cmd_set *set,
				    u32 min_fps)
{
	int i;
	u8 code;

	if (!set || !set->cmds || !set->count ||
	    !m2468_adfr_min_fps_valid(min_fps) || !min_fps)
		return -EINVAL;

	code = m2468_adfr_min_fps_code(min_fps);
	for (i = 0; i < set->count; i++) {
		u8 *payload = (u8 *)set->cmds[i].msg.tx_buf;

		if (payload && set->cmds[i].msg.tx_len == 5 &&
		    payload[0] == 0xb1) {
			payload[4] = code;
			return 0;
		}
	}

	return -EINVAL;
}

static int m2468_adfr_send_min_fps_locked(struct dsi_panel *panel,
					  u32 min_fps)
{
	struct dsi_display_mode_priv_info *priv_info;
	int rc;

	if (!m2468_adfr_panel_supported(panel) || !panel->cur_mode ||
	    !panel->cur_mode->priv_info)
		return -EINVAL;
	if (!m2468_adfr_min_fps_valid(min_fps))
		return -EINVAL;
	if (min_fps && atomic_read(&m2468_adfr.hbm_suspended))
		return -EBUSY;

	if (!min_fps) {
		rc = dsi_panel_m2468_tx_cmd_set(panel,
						 DSI_CMD_SET_M2468_ADFR_OFF);
	} else {
		priv_info = panel->cur_mode->priv_info;
		rc = m2468_adfr_patch_min_fps(
			&priv_info->cmd_sets[DSI_CMD_SET_M2468_ADFR_ON], min_fps);
		if (rc)
			return rc;
		rc = dsi_panel_m2468_tx_cmd_set(panel,
						 DSI_CMD_SET_M2468_ADFR_ON);
	}

	if (!rc) {
		atomic_set(&m2468_adfr.min_fps, min_fps);
		if (min_fps)
			atomic_set(&m2468_adfr.last_min_fps, min_fps);
	}
	return rc;
}

static int m2468_adfr_update_min_fps(u32 min_fps)
{
	struct dsi_display *display = READ_ONCE(m2468_adfr.display);
	struct dsi_panel *panel;
	bool clocks_enabled = false;
	int rc = 0;

	if (!atomic_read(&m2468_adfr.supported) || !display || !display->panel)
		return -ENODEV;
	if (!m2468_adfr_min_fps_valid(min_fps))
		return -EINVAL;

	panel = display->panel;
	if (atomic_read(&m2468_adfr.min_fps) == min_fps)
		return 0;
	if (panel->power_mode != SDE_MODE_DPMS_ON)
		return -EBUSY;

	mutex_lock(&display->display_lock);
	if (panel->panel_mode == DSI_OP_CMD_MODE && display->dsi_clk_handle) {
		rc = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
					  DSI_CLK_ON);
		if (rc)
			goto unlock_display;
		clocks_enabled = true;
	}

	mutex_lock(&panel->panel_lock);
	if (dsi_panel_initialized(panel))
		rc = m2468_adfr_send_min_fps_locked(panel, min_fps);
	else
		rc = -EBUSY;
	mutex_unlock(&panel->panel_lock);

	if (clocks_enabled)
		dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
				     DSI_CLK_OFF);

unlock_display:
	mutex_unlock(&display->display_lock);
	return rc;
}

static void m2468_adfr_status_switch(bool enable)
{
	u32 min_fps;

	if (!enable) {
		m2468_adfr_update_min_fps(0);
		return;
	}

	min_fps = atomic_read(&m2468_adfr.last_min_fps);
	if (!min_fps || min_fps == 1 ||
	    !m2468_adfr_min_fps_valid(min_fps))
		min_fps = 32;
	m2468_adfr_update_min_fps(min_fps);
}

void m2468_display_adfr_active(void)
{
	if (atomic_read(&m2468_adfr.supported))
		m2468_adfr_update_min_fps(32);
}

void m2468_display_adfr_panel_disable(void)
{
	if (!atomic_read(&m2468_adfr.supported))
		return;

	atomic_set(&m2468_adfr.timer_active, 0);
	hrtimer_cancel(&m2468_adfr.drop_timer);
	atomic_set(&m2468_adfr.min_fps, 0);
	atomic_set(&m2468_adfr.aod_light_mode, 2);
	atomic_set(&m2468_adfr.hbm_saved_min_fps, 0);
	atomic_set(&m2468_adfr.fp_saved_min_fps, 0);
	atomic_set(&m2468_adfr.hbm_suspended, 0);
	atomic_set(&m2468_adfr.backlight_type, 0);
}

bool m2468_display_adfr_full_aod(void)
{
	return atomic_read(&m2468_adfr.full_aod_enable);
}

void m2468_display_adfr_aod_exit(struct dsi_panel *panel)
{
	if (m2468_adfr_panel_supported(panel))
		atomic_set(&m2468_adfr.aod_light_mode, 2);
}

int m2468_display_adfr_hbm_locked(struct dsi_panel *panel, bool enable)
{
	u32 min_fps;
	int rc;

	if (!atomic_read(&m2468_adfr.supported) ||
	    !m2468_adfr_panel_supported(panel))
		return 0;
	if (enable) {
		if (atomic_read(&m2468_adfr.hbm_suspended))
			return 0;
		min_fps = atomic_read(&m2468_adfr.min_fps);
		if (min_fps) {
			rc = m2468_adfr_send_min_fps_locked(panel, 0);
			if (rc)
				return rc;
		}
		if (!min_fps)
			min_fps = atomic_read(&m2468_adfr.hbm_saved_min_fps);
		atomic_set(&m2468_adfr.hbm_saved_min_fps, 0);
		atomic_set(&m2468_adfr.fp_saved_min_fps, min_fps);
		atomic_set(&m2468_adfr.hbm_suspended, 1);
		atomic_set(&m2468_adfr.timer_active, 0);
		hrtimer_cancel(&m2468_adfr.drop_timer);
		return 0;
	}
	atomic_set(&m2468_adfr.hbm_suspended, 0);
	min_fps = atomic_read(&m2468_adfr.fp_saved_min_fps);
	if (min_fps && panel->power_mode == SDE_MODE_DPMS_ON &&
	    !panel->m2468_bl.aod_active && panel->cur_mode &&
	    panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_M2468_ADFR_ON].count) {
		rc = m2468_adfr_send_min_fps_locked(panel, min_fps);
		if (rc)
			return rc;
	}
	atomic_set(&m2468_adfr.fp_saved_min_fps, 0);
	return 0;
}

static u32 m2468_adfr_normal_bl_max(struct dsi_panel *panel)
{
	u32 level = READ_ONCE(m2468_adfr.normal_bl_max);

	if (panel->m2468_bl.supported && panel->m2468_bl.normal_max)
		return panel->m2468_bl.normal_max;
	if (!level && panel)
		of_property_read_u32(panel->panel_of_node,
				     "qcom,mdss-brightness-normal-max-level", &level);
	return level ? level : panel->bl_config.bl_max_level;
}

int m2468_display_adfr_before_backlight(struct dsi_panel *panel, u32 level)
{
	u32 threshold;
	u32 type;
	int min_fps;
	int rc;

	if (!atomic_read(&m2468_adfr.supported) ||
	    !m2468_adfr_panel_supported(panel))
		return 0;
	if (atomic_read(&m2468_adfr.hbm_suspended))
		return 0;
	min_fps = atomic_read(&m2468_adfr.hbm_saved_min_fps);
	if (min_fps) {
		rc = m2468_adfr_send_min_fps_locked(panel, min_fps);
		if (rc)
			return rc;
		atomic_set(&m2468_adfr.hbm_saved_min_fps, 0);
	}

	threshold = m2468_adfr_normal_bl_max(panel);
	m2468_adfr.pending_backlight_type = threshold && level > threshold;
	type = atomic_read(&m2468_adfr.backlight_type);
	if (!threshold || m2468_adfr.pending_backlight_type == type)
		return 0;

	min_fps = atomic_read(&m2468_adfr.min_fps);
	rc = m2468_adfr_send_min_fps_locked(panel, 0);
	if (rc)
		return rc;
	atomic_set(&m2468_adfr.hbm_saved_min_fps, min_fps);
	return 0;
}

void m2468_display_adfr_after_backlight(struct dsi_panel *panel, bool success)
{
	u32 last_min_fps;

	if (!atomic_read(&m2468_adfr.supported) ||
	    !m2468_adfr_panel_supported(panel) ||
	    atomic_read(&m2468_adfr.hbm_suspended))
		return;

	last_min_fps = atomic_read(&m2468_adfr.hbm_saved_min_fps);
	if (last_min_fps) {
		if (m2468_adfr_send_min_fps_locked(panel,
						   m2468_adfr_min_fps_valid(last_min_fps) ?
						   last_min_fps : 32))
			return;
		atomic_set(&m2468_adfr.hbm_saved_min_fps, 0);
	}

	if (success)
		atomic_set(&m2468_adfr.backlight_type,
			   m2468_adfr.pending_backlight_type);
}

static bool m2468_adfr_regular_refresh_rate(u32 refresh_rate)
{
	return refresh_rate == 30 || refresh_rate == 60 ||
		refresh_rate == 120;
}

void m2468_display_adfr_reset_min_fps(struct dsi_panel *panel)
{
	if (atomic_read(&m2468_adfr.supported) &&
	    m2468_adfr_panel_supported(panel))
		atomic_set(&m2468_adfr.min_fps, 0);
}

void m2468_display_adfr_mode_switch(struct dsi_panel *panel)
{
	if (!atomic_read(&m2468_adfr.supported) ||
	    !m2468_adfr_panel_supported(panel) || !panel->cur_mode ||
	    panel->power_mode != SDE_MODE_DPMS_ON ||
	    panel->m2468_bl.aod_active)
		return;

	/* The mode-switch caller already holds the display and panel locks. */
	if (m2468_adfr_regular_refresh_rate(panel->cur_mode->timing.refresh_rate))
		m2468_adfr_send_min_fps_locked(panel, 32);
}

static void m2468_adfr_drop_work(struct kthread_work *work)
{
	if (atomic_read(&m2468_adfr.timer_active) &&
	    atomic_read(&m2468_adfr.min_fps) == 32)
		m2468_adfr_update_min_fps(1);
}

static enum hrtimer_restart m2468_adfr_drop_timer(struct hrtimer *timer)
{
	kthread_queue_work(&m2468_adfr.worker, &m2468_adfr.drop_work);
	return HRTIMER_NORESTART;
}

void m2468_display_adfr_handle_idle(bool enter_idle)
{
	struct dsi_display *display = READ_ONCE(m2468_adfr.display);
	struct dsi_panel *panel;
	bool dynamic_te;
	int exit_count;

	if (!atomic_read(&m2468_adfr.supported) || !display || !display->panel)
		return;

	panel = display->panel;
	if (panel->panel_mode != DSI_OP_CMD_MODE ||
	    panel->power_mode != SDE_MODE_DPMS_ON ||
	    !dsi_panel_initialized(panel) || !atomic_read(&m2468_adfr.min_fps))
		return;

	dynamic_te = atomic_read(&m2468_adfr.dynamic_te_enabled);
	if (enter_idle) {
		exit_count = atomic_read(&m2468_adfr.exit_idle_count);
		if (exit_count >= 2 || (exit_count && !dynamic_te &&
				       m2468_display_adfr_full_aod())) {
			if (!dynamic_te && atomic_read(&m2468_adfr.min_fps) != 32)
				m2468_adfr_update_min_fps(32);
			if (!atomic_xchg(&m2468_adfr.timer_active, 1))
				hrtimer_start(&m2468_adfr.drop_timer,
					      ns_to_ktime(M2468_ADFR_IDLE_DELAY_NS),
					      HRTIMER_MODE_REL);
		}
		atomic_set(&m2468_adfr.exit_idle_count, 0);
		return;
	}

	if (dynamic_te && atomic_inc_return(&m2468_adfr.exit_idle_count) < 2)
		return;
	if (atomic_xchg(&m2468_adfr.timer_active, 0)) {
		hrtimer_cancel(&m2468_adfr.drop_timer);
		if (dynamic_te && atomic_read(&m2468_adfr.exit_idle_count) < 2)
			return;
	}
	if (dynamic_te) {
		if (atomic_read(&m2468_adfr.min_fps) != 32)
			m2468_adfr_update_min_fps(32);
	} else {
		atomic_inc(&m2468_adfr.exit_idle_count);
	}
}

static u32 m2468_adfr_filter_dynamic_te(struct m2468_dynamic_te_filter *filter,
					u32 panel_refresh_rate,
					u32 min_fps,
					u32 measured_refresh_rate)
{
	u8 high_count;

	if (!filter)
		return 0;
	if (!min_fps) {
		filter->refresh_rate = panel_refresh_rate;
		return filter->refresh_rate;
	}
	if (min_fps == 1) {
		filter->refresh_rate = 1;
		return filter->refresh_rate;
	}

	if (measured_refresh_rate < 23) {
		filter->refresh_rate_high = 0;
		filter->refresh_rate_high_60hz_count = 0;
		filter->refresh_rate_high_120hz_count = 0;
		filter->refresh_rate_mid++;
		filter->refresh_rate_high_30hz_count = 0;
		if (measured_refresh_rate < 18) {
			filter->refresh_rate_mid_20hz_count = 0;
			return filter->refresh_rate;
		}
		filter->refresh_rate_mid_20hz_count++;
		if (filter->refresh_rate_mid_20hz_count == 2) {
			filter->refresh_rate = 10;
			filter->refresh_rate_mid_20hz_count = 1;
		}
		return filter->refresh_rate;
	}

	filter->refresh_rate_mid = 0;
	filter->refresh_rate_high++;
	high_count = filter->refresh_rate_high;
	if (measured_refresh_rate >= 66) {
		filter->refresh_rate_high_60hz_count = 0;
		filter->refresh_rate_high_30hz_count = 0;
		filter->refresh_rate_mid_20hz_count = 0;
		filter->refresh_rate_high_120hz_count++;
		if (filter->refresh_rate_high_120hz_count == 3) {
			filter->refresh_rate = 120;
			filter->refresh_rate_high_120hz_count = 2;
		}
	} else if (measured_refresh_rate >= 56) {
		filter->refresh_rate_high_120hz_count = 0;
		filter->refresh_rate_high_30hz_count = 0;
		filter->refresh_rate_mid_20hz_count = 0;
		filter->refresh_rate_high_60hz_count++;
		if (filter->refresh_rate_high_60hz_count == 3) {
			filter->refresh_rate = 60;
			filter->refresh_rate_high_60hz_count = 2;
		}
	} else {
		filter->refresh_rate_high_120hz_count = 0;
		filter->refresh_rate_high_60hz_count = 0;
		filter->refresh_rate_mid_20hz_count = 0;
		filter->refresh_rate_high_30hz_count++;
		if (filter->refresh_rate_high_30hz_count == 2) {
			filter->refresh_rate = 30;
			filter->refresh_rate_high_30hz_count = 1;
		}
	}

	if (high_count >= 3 && filter->refresh_rate <= 29) {
		filter->refresh_rate = 30;
		filter->refresh_rate_high = 1;
	}
	return filter->refresh_rate;
}

static u32 m2468_adfr_dynamic_te_rate(void)
{
	u32 refresh_rate = atomic_read(&m2468_adfr.dynamic_te_read_rate);
	u32 count;

	count = atomic_inc_return(&m2468_adfr.dynamic_te_read_count);
	if (count != 2)
		return refresh_rate;

	atomic_set(&m2468_adfr.dynamic_te_read_count, 0);
	refresh_rate = READ_ONCE(m2468_adfr.dynamic_filter.refresh_rate);
	atomic_set(&m2468_adfr.dynamic_te_read_rate, refresh_rate);
	if (atomic_read(&m2468_adfr.min_fps) == 32 &&
	    atomic_read(&m2468_adfr.timer_active)) {
		count = atomic_inc_return(&m2468_adfr.enter_1hz_count);
		if (count >= 4) {
			atomic_set(&m2468_adfr.enter_1hz_count, 3);
			refresh_rate = 1;
			atomic_set(&m2468_adfr.dynamic_te_read_rate, 1);
		}
	} else if (atomic_read(&m2468_adfr.min_fps) == 1) {
		refresh_rate = 1;
		atomic_set(&m2468_adfr.dynamic_te_read_rate, 1);
	} else {
		atomic_set(&m2468_adfr.enter_1hz_count, 0);
	}
	return refresh_rate;
}

static irqreturn_t m2468_adfr_dynamic_te_irq(int irq, void *data)
{
	struct dsi_display *display = data;
	struct dsi_panel *panel;
	ktime_t now;
	s64 delta_ms;
	u32 measured_refresh_rate = 0;

	if (!display || !display->panel || !display->panel->cur_mode)
		return IRQ_HANDLED;

	panel = display->panel;
	now = ktime_get();
	delta_ms = ktime_ms_delta(now, m2468_adfr.last_te_timestamp);
	m2468_adfr.last_te_timestamp = now;
	if (delta_ms > 0)
		measured_refresh_rate = 1000 / delta_ms;

	m2468_adfr_filter_dynamic_te(&m2468_adfr.dynamic_filter,
		panel->cur_mode->timing.refresh_rate,
		atomic_read(&m2468_adfr.min_fps), measured_refresh_rate);
	return IRQ_HANDLED;
}

static ssize_t m2468_adfr_minfps_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "display_adfr_is_support=%d minfps=%d\n",
			  atomic_read(&m2468_adfr.supported),
			  atomic_read(&m2468_adfr.min_fps));
}

static ssize_t m2468_adfr_minfps_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned int first;
	unsigned int second;
	int consumed = 0;
	int parsed;

	parsed = sscanf(buf, "%x%n", &first, &consumed);
	if (parsed < 1)
		return -EINVAL;
	parsed = sscanf(buf + consumed, "%x", &second);
	if (parsed < 1) {
		m2468_adfr_status_switch(first != 0);
		return count;
	}

	if (!first) {
		if (!atomic_xchg(&m2468_adfr.debug_mode, 1))
			atomic_set(&m2468_adfr.debug_saved_min_fps,
				   atomic_read(&m2468_adfr.min_fps));
		m2468_adfr_status_switch(false);
		m2468_adfr_update_min_fps(second);
	} else if (first == 1 && atomic_xchg(&m2468_adfr.debug_mode, 0)) {
		m2468_adfr_status_switch(true);
		second = atomic_read(&m2468_adfr.debug_saved_min_fps);
		m2468_adfr_update_min_fps(second);
	}
	return count;
}

static ssize_t m2468_adfr_dynamic_te_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	return sysfs_emit(buf, "%u\n", m2468_adfr_dynamic_te_rate());
}

static ssize_t m2468_adfr_dynamic_te_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	unsigned int enabled;

	if (kstrtouint(buf, 0, &enabled))
		return -EINVAL;
	if (!m2468_adfr.dynamic_te_irq_registered)
		return -ENODEV;

	enabled = !!enabled;
	if (enabled && !m2468_adfr.dynamic_te_irq_enabled) {
		m2468_adfr.last_te_timestamp = ktime_get();
		enable_irq(m2468_adfr.dynamic_te_irq);
		m2468_adfr.dynamic_te_irq_enabled = true;
	} else if (!enabled && m2468_adfr.dynamic_te_irq_enabled) {
		disable_irq(m2468_adfr.dynamic_te_irq);
		m2468_adfr.dynamic_te_irq_enabled = false;
	}
	atomic_set(&m2468_adfr.dynamic_te_enabled, enabled);
	return count;
}

static int m2468_adfr_set_aod_light_mode_locked(struct dsi_panel *panel,
						 unsigned int mode)
{
	int rc;

	if (!m2468_adfr_panel_supported(panel) || !dsi_panel_initialized(panel))
		return -ENODEV;
	if (!panel->m2468_bl.aod_active &&
	    (panel->m2468_bl.local_hbm || panel->m2468_bl.global_hbm))
		return -EBUSY;
	if (atomic_read(&m2468_adfr.aod_light_mode) == mode)
		return 0;
	rc = dsi_panel_m2468_tx_cmd_set(panel, mode ?
		DSI_CMD_SET_M2468_AOD_HIGH : DSI_CMD_SET_M2468_AOD_LOW);
	if (!rc)
		atomic_set(&m2468_adfr.aod_light_mode, mode);
	return rc;
}

static int m2468_adfr_set_aod_light_mode(unsigned int mode)
{
	struct dsi_display *display = READ_ONCE(m2468_adfr.display);
	struct dsi_panel *panel;
	bool clocks_enabled = false;
	int rc = 0;

	if (!display || !display->panel)
		return -ENODEV;
	panel = display->panel;
	if (panel->power_mode != SDE_MODE_DPMS_LP1 &&
	    panel->power_mode != SDE_MODE_DPMS_LP2)
		return 0;

	mutex_lock(&display->display_lock);
	if (panel->panel_mode == DSI_OP_CMD_MODE && display->dsi_clk_handle) {
		rc = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
					  DSI_CLK_ON);
		if (rc)
			goto unlock_display;
		clocks_enabled = true;
	}
	mutex_lock(&panel->panel_lock);
	rc = m2468_adfr_set_aod_light_mode_locked(panel, mode);
	mutex_unlock(&panel->panel_lock);
	if (clocks_enabled)
		dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
				     DSI_CLK_OFF);

unlock_display:
	mutex_unlock(&display->display_lock);
	return rc;
}

void m2468_display_adfr_aod_enter(struct dsi_panel *panel)
{
	int mode = atomic_read(&m2468_adfr.aod_presets_light_mode);

	if (!atomic_read(&m2468_adfr.supported) || mode < 0 || mode > 1)
		return;
	m2468_adfr_set_aod_light_mode_locked(panel, mode);
}

static ssize_t m2468_adfr_fullaod_enable_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  atomic_read(&m2468_adfr.full_aod_enable));
}

static ssize_t m2468_adfr_fullaod_enable_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	unsigned int enabled;

	if (kstrtouint(buf, 0, &enabled))
		return -EINVAL;
	atomic_set(&m2468_adfr.full_aod_enable, !!enabled);
	return count;
}

static ssize_t m2468_adfr_aod_light_mode_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  atomic_read(&m2468_adfr.aod_light_mode));
}

static ssize_t m2468_adfr_aod_light_mode_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	unsigned int mode;

	if (kstrtouint(buf, 0, &mode) || mode > 1)
		return -EINVAL;
	atomic_set(&m2468_adfr.aod_presets_light_mode, mode);
	if (atomic_read(&m2468_adfr.supported))
		m2468_adfr_set_aod_light_mode(mode);
	return count;
}

static ssize_t m2468_adfr_star_brightness_show(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       char *buf)
{
	ssize_t length;

	mutex_lock(&m2468_star_lock);
	length = sysfs_emit(buf, "%s\n", m2468_star_brightness);
	mutex_unlock(&m2468_star_lock);
	return length;
}

static ssize_t m2468_adfr_star_brightness_store(struct kobject *kobj,
						struct kobj_attribute *attr,
						const char *buf, size_t count)
{
	char backlight[32];
	char pcc[32];

	if (sscanf(buf, "%31s %31s", backlight, pcc) != 2)
		return -EINVAL;
	mutex_lock(&m2468_star_lock);
	scnprintf(m2468_star_brightness, sizeof(m2468_star_brightness),
		  "%s %s", backlight, pcc);
	mutex_unlock(&m2468_star_lock);
	return count;
}

static ssize_t m2468_adfr_synclights_enable_show(struct kobject *kobj,
						 struct kobj_attribute *attr,
						 char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(synclights_enable));
}

static ssize_t m2468_adfr_synclights_enable_store(struct kobject *kobj,
						  struct kobj_attribute *attr,
						  const char *buf,
						  size_t count)
{
	unsigned int enabled;

	if (kstrtouint(buf, 0, &enabled))
		return -EINVAL;
	WRITE_ONCE(synclights_enable, !!enabled);
	return count;
}

static struct kobj_attribute m2468_adfr_minfps_attr =
	__ATTR(adfr_minfps, 0644, m2468_adfr_minfps_show,
	       m2468_adfr_minfps_store);
static struct kobj_attribute m2468_adfr_dynamic_te_attr =
	__ATTR(dynamic_te, 0644, m2468_adfr_dynamic_te_show,
	       m2468_adfr_dynamic_te_store);
static struct kobj_attribute m2468_adfr_aod_light_mode_attr =
	__ATTR(aod_light_mode, 0644, m2468_adfr_aod_light_mode_show,
	       m2468_adfr_aod_light_mode_store);
static struct kobj_attribute m2468_adfr_fullaod_enable_attr =
	__ATTR(fullaod_enable, 0644, m2468_adfr_fullaod_enable_show,
	       m2468_adfr_fullaod_enable_store);
static struct kobj_attribute m2468_adfr_star_brightness_attr =
	__ATTR(star_brightness, 0644, m2468_adfr_star_brightness_show,
	       m2468_adfr_star_brightness_store);
static struct kobj_attribute m2468_adfr_synclights_enable_attr =
	__ATTR(synclights_enable, 0644, m2468_adfr_synclights_enable_show,
	       m2468_adfr_synclights_enable_store);

static struct attribute *m2468_adfr_attrs[] = {
	&m2468_adfr_minfps_attr.attr,
	&m2468_adfr_dynamic_te_attr.attr,
	&m2468_adfr_aod_light_mode_attr.attr,
	&m2468_adfr_fullaod_enable_attr.attr,
	&m2468_adfr_star_brightness_attr.attr,
	&m2468_adfr_synclights_enable_attr.attr,
	NULL,
};

static const struct attribute_group m2468_adfr_attr_group = {
	.attrs = m2468_adfr_attrs,
};

static void m2468_adfr_release(void *data)
{
	struct dsi_display *display = data;

	mutex_lock(&m2468_adfr_lifecycle_lock);
	if (!m2468_adfr.initialized || m2468_adfr.display != display) {
		mutex_unlock(&m2468_adfr_lifecycle_lock);
		return;
	}
	if (m2468_adfr.kobj) {
		m2468_display_hbm_deinit(m2468_adfr.kobj);
		sysfs_remove_group(m2468_adfr.kobj, &m2468_adfr_attr_group);
	}
	atomic_set(&m2468_adfr.supported, 0);
	atomic_set(&m2468_adfr.timer_active, 0);
	if (m2468_adfr.dynamic_te_irq_enabled)
		disable_irq(m2468_adfr.dynamic_te_irq);
	if (m2468_adfr.dynamic_te_irq_registered)
		devm_free_irq(&display->pdev->dev, m2468_adfr.dynamic_te_irq,
			      display);
	hrtimer_cancel(&m2468_adfr.drop_timer);
	kthread_flush_worker(&m2468_adfr.worker);
	if (m2468_adfr.worker_task)
		kthread_stop(m2468_adfr.worker_task);
	if (m2468_adfr.kobj)
		kobject_put(m2468_adfr.kobj);
	m2468_adfr.display = NULL;
	m2468_adfr.kobj = NULL;
	m2468_adfr.worker_task = NULL;
	m2468_adfr.dynamic_te_irq_registered = false;
	m2468_adfr.dynamic_te_irq_enabled = false;
	m2468_adfr.initialized = false;
	atomic_set(&m2468_adfr.supported, 0);
	mutex_unlock(&m2468_adfr_lifecycle_lock);
}

void m2468_display_adfr_deinit(struct dsi_display *display)
{
	if (READ_ONCE(m2468_adfr.initialized) &&
	    READ_ONCE(m2468_adfr.display) == display)
		devm_release_action(&display->pdev->dev, m2468_adfr_release, display);
}

int m2468_display_adfr_init(struct dsi_display *display)
{
	struct dsi_panel *panel;
	u32 config = 0;
	int irq;
	int rc;

	if (!display || !display->panel)
		return -EINVAL;
	panel = display->panel;
	if (!m2468_adfr_panel_supported(panel))
		return 0;
	of_property_read_u32(panel->panel_of_node,
			     "qcom,display-adfr-config", &config);
	if (!config)
		return 0;

	mutex_lock(&m2468_adfr_lifecycle_lock);
	if (m2468_adfr.initialized) {
		mutex_unlock(&m2468_adfr_lifecycle_lock);
		return 0;
	}
	m2468_adfr.display = display;
	memset(&m2468_adfr.dynamic_filter, 0,
	       sizeof(m2468_adfr.dynamic_filter));
	atomic_set(&m2468_adfr.supported, 1);
	atomic_set(&m2468_adfr.min_fps, 0);
	atomic_set(&m2468_adfr.last_min_fps, 32);
	atomic_set(&m2468_adfr.hbm_saved_min_fps, 0);
	atomic_set(&m2468_adfr.fp_saved_min_fps, 0);
	atomic_set(&m2468_adfr.hbm_suspended, 0);
	atomic_set(&m2468_adfr.dynamic_te_read_rate, 120);
	atomic_set(&m2468_adfr.aod_light_mode, 2);
	atomic_set(&m2468_adfr.aod_presets_light_mode, 2);
	of_property_read_u32(panel->panel_of_node,
			     "qcom,mdss-brightness-normal-max-level",
			     &m2468_adfr.normal_bl_max);

	kthread_init_worker(&m2468_adfr.worker);
	kthread_init_work(&m2468_adfr.drop_work, m2468_adfr_drop_work);
	m2468_adfr.worker_task = kthread_run(kthread_worker_fn,
					     &m2468_adfr.worker,
					     "m2468_adfr");
	if (IS_ERR(m2468_adfr.worker_task)) {
		rc = PTR_ERR(m2468_adfr.worker_task);
		m2468_adfr.worker_task = NULL;
		goto fail;
	}
	hrtimer_init(&m2468_adfr.drop_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL);
	m2468_adfr.drop_timer.function = m2468_adfr_drop_timer;

	m2468_adfr.kobj =
		kobject_create_and_add("display_drivers", kernel_kobj);
	if (!m2468_adfr.kobj) {
		rc = -ENOMEM;
		goto fail_worker;
	}
	rc = sysfs_create_group(m2468_adfr.kobj, &m2468_adfr_attr_group);
	if (rc)
		goto fail_kobject;
	rc = m2468_display_hbm_init(display, m2468_adfr.kobj);
	if (rc) {
		sysfs_remove_group(m2468_adfr.kobj, &m2468_adfr_attr_group);
		goto fail_kobject;
	}

	m2468_adfr.dynamic_te_gpio =
		of_get_named_gpio(panel->panel_of_node,
				  "qcom,dynamic-te-gpio", 0);
	if (gpio_is_valid(m2468_adfr.dynamic_te_gpio) &&
	    !display->trusted_vm_env) {
		m2468_adfr.dynamic_te_irq =
			gpio_to_irq(m2468_adfr.dynamic_te_gpio);
		if (m2468_adfr.dynamic_te_irq > 0) {
			irq = m2468_adfr.dynamic_te_irq;
			rc = devm_request_threaded_irq(&display->pdev->dev, irq,
						       NULL,
						       m2468_adfr_dynamic_te_irq,
						       IRQF_TRIGGER_RISING |
						       IRQF_ONESHOT,
						       "M2468_DYNAMIC_TE", display);
			if (!rc) {
				disable_irq(m2468_adfr.dynamic_te_irq);
				m2468_adfr.dynamic_te_irq_registered = true;
			}
		}
	}
	m2468_adfr.initialized = true;
	mutex_unlock(&m2468_adfr_lifecycle_lock);
	rc = devm_add_action(&display->pdev->dev, m2468_adfr_release, display);
	if (rc)
		m2468_adfr_release(display);
	return rc;

fail_kobject:
	kobject_put(m2468_adfr.kobj);
	m2468_adfr.kobj = NULL;
fail_worker:
	kthread_stop(m2468_adfr.worker_task);
	m2468_adfr.worker_task = NULL;
fail:
	m2468_adfr.display = NULL;
	atomic_set(&m2468_adfr.supported, 0);
	mutex_unlock(&m2468_adfr_lifecycle_lock);
	return rc;
}
