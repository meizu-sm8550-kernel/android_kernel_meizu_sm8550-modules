// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/export.h>
#include <linux/kobject.h>
#include <linux/of.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "../dsi/dsi_display.h"
#include "../dsi/dsi_panel.h"
#include "../sde/sde_connector.h"
#include "../sde/sde_encoder.h"
#include "../sde/sde_encoder_phys.h"
#include "m2468_display_adfr.h"
#include "m2468_display_brightness.h"
#include "m2468_display_hbm.h"

enum m2468_hbm_mode {
	M2468_HBM_OFF,
	M2468_HBM_ON,
	M2468_HBM_HALF,
	M2468_HBM_THREE_QUARTER,
	M2468_HBM_BRIGHTNESS_OFF,
	M2468_HBM_500_NIT,
	M2468_HBM_LOCAL_ON,
	M2468_HBM_LOCAL_OFF,
	M2468_HBM_LOCAL_GREEN,
	M2468_HBM_RESET,
};

static struct {
	struct dsi_display *display;
	struct kobject *kobj;
	struct workqueue_struct *wq;
	struct work_struct notify_work;
	u32 vblank_count;
	u32 sequence;
	bool target;
	bool pending;
	bool notified;
} m2468_hbm;
static DEFINE_SPINLOCK(m2468_hbm_lock);
static atomic_t m2468_hbm_ready = ATOMIC_INIT(0);
static atomic_t m2468_fp_result = ATOMIC_INIT(2);
static atomic_t m2468_ofp_press = ATOMIC_INIT(0);
static atomic_t m2468_fp_scene = ATOMIC_INIT(0);

bool m2468_display_panel_supported(const struct dsi_panel *panel)
{
	return panel && panel->name &&
		!strcmp(panel->name, "ILI7838E_TIANMA_MEIZU");
}

int display_set_fingerprint_scene(unsigned int scene)
{
	atomic_set(&m2468_fp_scene, scene);
	return 0;
}
EXPORT_SYMBOL(display_set_fingerprint_scene);

unsigned int display_get_fingerprint_scene(void)
{
	return atomic_read(&m2468_fp_scene);
}
EXPORT_SYMBOL(display_get_fingerprint_scene);

static struct drm_encoder *m2468_display_encoder(struct dsi_display *display)
{
	if (!display || !display->drm_conn)
		return NULL;
	return to_sde_connector(display->drm_conn)->encoder;
}

int m2468_display_wait_for_vsync(struct dsi_panel *panel)
{
	struct dsi_display *display = READ_ONCE(m2468_hbm.display);
	struct drm_encoder *encoder;

	if (!display || display->panel != panel || !panel->panel_initialized)
		return -ENODEV;
	encoder = m2468_display_encoder(display);
	if (!encoder)
		return -ENODEV;
	return sde_encoder_m2468_wakeup(encoder, true);
}

void m2468_display_frame_delay(struct dsi_panel *panel, unsigned int frames)
{
	u32 delay;

	if (!panel->cur_mode)
		return;
	switch (panel->cur_mode->timing.refresh_rate) {
	case 144:
		delay = 7000;
		break;
	case 120:
		delay = 9000;
		break;
	case 90:
		delay = 13000;
		break;
	case 60:
		delay = 17000;
		break;
	case 30:
		delay = 33000;
		break;
	default:
		return;
	}
	delay *= frames;
	usleep_range(delay, delay + 100);
}

static void m2468_hbm_notify_work(struct work_struct *work)
{
	struct panel_event_notification notification = { 0 };
	struct dsi_display *display;
	struct kobject *kobj;
	unsigned long flags;
	u32 sequence;
	bool ready;

	for (;;) {
		spin_lock_irqsave(&m2468_hbm_lock, flags);
		display = m2468_hbm.display;
		kobj = m2468_hbm.kobj;
		ready = atomic_read(&m2468_hbm_ready);
		sequence = m2468_hbm.sequence;
		if (!display || ready == m2468_hbm.notified) {
			spin_unlock_irqrestore(&m2468_hbm_lock, flags);
			return;
		}
		m2468_hbm.notified = ready;
		spin_unlock_irqrestore(&m2468_hbm_lock, flags);

		notification.panel = &display->panel->drm_panel;
		notification.notif_type = ready ? DRM_PANEL_EVENT_UI_READY :
			DRM_PANEL_EVENT_UI_DISAPPEAR;
		notification.notif_data.early_trigger = true;
		panel_event_notification_trigger(PANEL_EVENT_NOTIFICATION_PRIMARY,
						 &notification);
		if (kobj)
			sysfs_notify(kobj, NULL, "hbm_ready_status");

		spin_lock_irqsave(&m2468_hbm_lock, flags);
		if (sequence == m2468_hbm.sequence) {
			spin_unlock_irqrestore(&m2468_hbm_lock, flags);
			return;
		}
		spin_unlock_irqrestore(&m2468_hbm_lock, flags);
	}
}

static void m2468_hbm_state_changed(struct dsi_panel *panel)
{
	struct m2468_panel_backlight *bl = &panel->m2468_bl;
	unsigned long flags;
	bool target = bl->local_hbm ||
		(bl->global_hbm && bl->ofp_enabled && (bl->ofp_type & BIT(0)));

	spin_lock_irqsave(&m2468_hbm_lock, flags);
	if (m2468_hbm.display && m2468_hbm.display->panel == panel) {
		m2468_hbm.target = target;
		m2468_hbm.vblank_count = 0;
		m2468_hbm.pending = target != !!atomic_read(&m2468_hbm_ready);
	}
	spin_unlock_irqrestore(&m2468_hbm_lock, flags);
}

void m2468_display_hbm_te_irq(struct sde_encoder_phys *phys_enc)
{
	struct dsi_display *display;
	unsigned long flags;

	if (!phys_enc || !phys_enc->connector ||
	    phys_enc->connector->connector_type != DRM_MODE_CONNECTOR_DSI ||
	    !phys_enc->ops.is_master || !phys_enc->ops.is_master(phys_enc))
		return;
	display = to_sde_connector(phys_enc->connector)->display;
	spin_lock_irqsave(&m2468_hbm_lock, flags);
	if (display && display == m2468_hbm.display && m2468_hbm.pending &&
	    ++m2468_hbm.vblank_count >= display->panel->m2468_bl.ready_vblanks) {
		m2468_hbm.pending = false;
		atomic_set(&m2468_hbm_ready, m2468_hbm.target);
		m2468_hbm.sequence++;
		if (m2468_hbm.wq)
			queue_work(m2468_hbm.wq, &m2468_hbm.notify_work);
	}
	spin_unlock_irqrestore(&m2468_hbm_lock, flags);
}

void m2468_display_hbm_panel_disable(struct dsi_panel *panel)
{
	struct m2468_panel_backlight *bl = &panel->m2468_bl;
	unsigned long flags;

	if (!bl->supported)
		return;
	WRITE_ONCE(bl->global_hbm, false);
	WRITE_ONCE(bl->local_hbm, false);
	bl->hbm_mode = M2468_HBM_RESET;
	bl->aod_active = false;
	bl->effective_bl = 0;
	bl->type = M2468_BACKLIGHT_PWM;
	atomic_set(&m2468_fp_result, 2);
	atomic_set(&m2468_ofp_press, 0);
	atomic_set(&m2468_fp_scene, 0);
	spin_lock_irqsave(&m2468_hbm_lock, flags);
	m2468_hbm.pending = false;
	m2468_hbm.target = false;
	m2468_hbm.vblank_count = 0;
	atomic_set(&m2468_hbm_ready, 0);
	m2468_hbm.sequence++;
	if (m2468_hbm.wq)
		queue_work(m2468_hbm.wq, &m2468_hbm.notify_work);
	spin_unlock_irqrestore(&m2468_hbm_lock, flags);
}

int m2468_display_backlight_prepare(struct dsi_panel *panel, u32 level)
{
	struct m2468_panel_backlight *bl = &panel->m2468_bl;
	bool full_aod;

	if (!bl->supported)
		return 0;
	bl->requested_bl = level;
	full_aod = m2468_display_adfr_full_aod();
	if ((full_aod && !level) || (!full_aod &&
	    (panel->power_mode == SDE_MODE_DPMS_LP1 ||
	     panel->power_mode == SDE_MODE_DPMS_LP2)))
		return 1;
	if (level && (bl->global_hbm || bl->local_hbm))
		return 1;
	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return -EINVAL;
	if (!level && bl->global_hbm) {
		WRITE_ONCE(bl->global_hbm, false);
		bl->hbm_mode = M2468_HBM_RESET;
		m2468_hbm_state_changed(panel);
	}
	return 0;
}

void m2468_display_backlight_done(struct dsi_panel *panel, u32 level)
{
	if (panel->m2468_bl.supported)
		panel->m2468_bl.effective_bl = level;
}

static u8 *m2468_hbm_payload(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u32 index, size_t size, u8 reg)
{
	struct dsi_panel_cmd_set *set;
	u8 *payload;

	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return NULL;
	set = &panel->cur_mode->priv_info->cmd_sets[type];
	if (!set->cmds || index >= set->count ||
	    set->cmds[index].msg.tx_len != size)
		return NULL;
	payload = (u8 *)set->cmds[index].msg.tx_buf;
	return payload && payload[0] == reg ? payload : NULL;
}

static int m2468_hbm_local_level(struct dsi_panel *panel, bool enable)
{
	/* Original ILI7838E C2 compensation, 50 DBV per entry from 3151. */
	static const u8 rgb[][3] = {
		{ 0xd9, 0xd9, 0xd9 }, { 0xd4, 0xd4, 0xd4 },
		{ 0xd0, 0xcf, 0xce }, { 0xcd, 0xcb, 0xca },
		{ 0xc8, 0xc6, 0xc5 }, { 0xc4, 0xc2, 0xc1 },
		{ 0xbe, 0xbc, 0xbb }, { 0xba, 0xb7, 0xb6 },
		{ 0xb6, 0xb4, 0xb3 }, { 0xb3, 0xb0, 0xae },
		{ 0xaf, 0xac, 0xa9 }, { 0xac, 0xa8, 0xa6 },
		{ 0xa8, 0xa4, 0xa2 }, { 0xa3, 0xa0, 0x9d },
		{ 0xa0, 0x9d, 0x9a }, { 0x9c, 0x99, 0x95 },
		{ 0x9a, 0x96, 0x93 }, { 0x97, 0x93, 0x90 },
		{ 0x95, 0x91, 0x8e },
	};
	u32 level = panel->m2468_bl.effective_bl;
	u32 index;
	u8 *payload;

	if (level < 3151)
		return 0;
	payload = m2468_hbm_payload(panel, DSI_CMD_SET_M2468_LOCAL_HBM_LEVEL,
				     1, 7, 0xc2);
	if (!payload)
		return -EINVAL;
	if (!enable) {
		memset(payload + 1, 0xdf, 3);
	} else {
		index = (level - 3151) / 50;
		if (index >= ARRAY_SIZE(rgb))
			return -ERANGE;
		memcpy(payload + 1, rgb[index], 3);
	}
	return dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_M2468_LOCAL_HBM_LEVEL);
}

static int m2468_hbm_local_off(struct dsi_panel *panel)
{
	struct m2468_panel_backlight *bl = &panel->m2468_bl;
	u8 *payload;
	int rc;

	payload = m2468_hbm_payload(panel, DSI_CMD_SET_M2468_LOCAL_HBM_OFF,
				     10, 4, 0x88);
	if (!payload)
		return -EINVAL;
	if (bl->effective_bl <= 408) {
		payload[1] = 0x40;
		payload[2] = 0x80;
	} else if (bl->requested_bl < bl->dc_min) {
		payload[1] = 0x20;
		payload[2] = 0x40;
	} else {
		payload[1] = 0;
		payload[2] = 1;
	}
	rc = dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_M2468_LOCAL_HBM_OFF);
	if (rc)
		return rc;
	rc = m2468_hbm_local_level(panel, false);
	if (rc)
		return rc;
	m2468_display_frame_delay(panel, 2);
	WRITE_ONCE(bl->local_hbm, false);
	m2468_hbm_state_changed(panel);

	if (!atomic_read(&m2468_fp_result) &&
	    (panel->power_mode == SDE_MODE_DPMS_LP1 ||
	     panel->power_mode == SDE_MODE_DPMS_LP2)) {
		if (m2468_display_adfr_full_aod())
			return 0;
		rc = m2468_display_wait_for_vsync(panel);
		if (rc)
			return rc;
		usleep_range(2000, 2100);
		rc = dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_LP1);
		if (rc)
			return rc;
		rc = dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_LP2);
		if (rc)
			return rc;
		bl->aod_active = true;
		m2468_display_adfr_aod_enter(panel);
		return 0;
	}
	return dsi_panel_set_backlight(panel, bl->requested_bl);
}

static int m2468_hbm_local_on(struct dsi_panel *panel, bool green)
{
	struct m2468_panel_backlight *bl = &panel->m2468_bl;
	int rc;

	if (!bl->ofp_enabled || !(bl->ofp_type & BIT(1)))
		return -EOPNOTSUPP;
	if (bl->aod_active) {
		rc = m2468_display_wait_for_vsync(panel);
		if (rc)
			return rc;
		usleep_range(2000, 2100);
		rc = dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_NOLP);
		if (rc)
			return rc;
		bl->aod_active = false;
		m2468_display_adfr_aod_exit(panel);
	}
	m2468_display_frame_delay(panel, 1);
	rc = dsi_panel_m2468_tx_cmd_set(panel, green ?
		DSI_CMD_SET_M2468_LOCAL_HBM_GREEN : DSI_CMD_SET_M2468_LOCAL_HBM_ON);
	if (rc)
		return rc;
	/* Track a completed ON command even if the following C2 write fails. */
	WRITE_ONCE(bl->local_hbm, true);
	rc = m2468_hbm_local_level(panel, true);
	if (rc)
		return rc;
	m2468_hbm_state_changed(panel);
	m2468_display_frame_delay(panel, 2);
	return 0;
}

static int m2468_hbm_global_off(struct dsi_panel *panel)
{
	struct dsi_panel_cmd_set *set;
	u32 level = panel->m2468_bl.requested_bl;
	u8 *payload;
	int rc;

	set = &panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_M2468_HBM_OFF];
	if (!set->count)
		return -EOPNOTSUPP;
	payload = m2468_hbm_payload(panel, DSI_CMD_SET_M2468_HBM_OFF,
				     set->count - 1, 3, 0x51);
	if (!payload)
		return -EINVAL;
	payload[1] = level >> 8;
	payload[2] = level;
	rc = dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_M2468_HBM_OFF);
	if (!rc) {
		WRITE_ONCE(panel->m2468_bl.global_hbm, false);
		m2468_hbm_state_changed(panel);
		rc = dsi_panel_set_backlight(panel, level);
	}
	return rc;
}

static bool m2468_hbm_mode_applied(struct m2468_panel_backlight *bl, u32 mode)
{
	if (mode != bl->hbm_mode)
		return false;
	switch (mode) {
	case M2468_HBM_LOCAL_ON:
	case M2468_HBM_LOCAL_GREEN:
		return bl->local_hbm;
	case M2468_HBM_OFF:
	case M2468_HBM_LOCAL_OFF:
	case M2468_HBM_BRIGHTNESS_OFF:
		return !bl->local_hbm && !bl->global_hbm;
	default:
		return bl->global_hbm;
	}
}

static int m2468_hbm_set_mode(struct dsi_display *display, u32 mode)
{
	static const enum dsi_cmd_set_type commands[] = {
		[M2468_HBM_ON] = DSI_CMD_SET_M2468_HBM_ON,
		[M2468_HBM_HALF] = DSI_CMD_SET_M2468_HBM_HALF,
		[M2468_HBM_THREE_QUARTER] = DSI_CMD_SET_M2468_HBM_THREE_QUARTER,
		[M2468_HBM_BRIGHTNESS_OFF] = DSI_CMD_SET_M2468_BRIGHTNESS_OFF,
		[M2468_HBM_500_NIT] = DSI_CMD_SET_M2468_500_NIT,
	};
	struct m2468_panel_backlight *bl;
	struct dsi_panel *panel;
	struct drm_encoder *encoder;
	bool clocks_on = false;
	bool suspend_adfr;
	int rc, tmp;

	if (!display || !display->panel)
		return -ENODEV;
	if (mode >= M2468_HBM_RESET)
		return -EINVAL;
	panel = display->panel;
	bl = &panel->m2468_bl;
	mutex_lock(&display->display_lock);
	if (panel->panel_mode == DSI_OP_CMD_MODE) {
		if (!display->dsi_clk_handle) {
			rc = -ENODEV;
			goto unlock_display;
		}
		rc = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_CORE_CLK, DSI_CLK_ON);
		if (rc)
			goto unlock_display;
		clocks_on = true;
	}
	mutex_lock(&panel->panel_lock);
	if (!panel->panel_initialized || panel->power_mode == SDE_MODE_DPMS_OFF ||
	    !panel->cur_mode || !panel->cur_mode->priv_info ||
	    atomic_read(&panel->esd_recovery_pending)) {
		rc = -EBUSY;
		goto unlock_panel;
	}
	if (m2468_hbm_mode_applied(bl, mode)) {
		rc = bl->local_hbm || bl->global_hbm ? 0 :
			m2468_display_adfr_hbm_locked(panel, false);
		goto unlock_panel;
	}
	encoder = m2468_display_encoder(display);
	if (!encoder) {
		rc = -ENODEV;
		goto unlock_panel;
	}
	rc = sde_encoder_m2468_wakeup(encoder, false);
	if (rc)
		goto unlock_panel;

	suspend_adfr = mode == M2468_HBM_ON || mode == M2468_HBM_HALF ||
		mode == M2468_HBM_THREE_QUARTER || mode == M2468_HBM_LOCAL_ON ||
		mode == M2468_HBM_LOCAL_GREEN;
	if (suspend_adfr) {
		rc = m2468_display_adfr_hbm_locked(panel, true);
		if (rc)
			goto unlock_panel;
	}

	if (mode == M2468_HBM_LOCAL_ON || mode == M2468_HBM_LOCAL_GREEN) {
		rc = m2468_hbm_local_on(panel, mode == M2468_HBM_LOCAL_GREEN);
	} else if (mode == M2468_HBM_LOCAL_OFF || mode == M2468_HBM_OFF) {
		if (bl->local_hbm)
			rc = m2468_hbm_local_off(panel);
		else if (mode == M2468_HBM_OFF)
			rc = m2468_hbm_global_off(panel);
		else
			rc = 0;
		if (!rc && mode == M2468_HBM_OFF && bl->global_hbm)
			rc = m2468_hbm_global_off(panel);
	} else {
		struct dsi_panel_cmd_set *set;

		set = &panel->cur_mode->priv_info->cmd_sets[commands[mode]];
		if (!set->count) {
			rc = -EOPNOTSUPP;
		} else if (bl->local_hbm) {
			rc = -EBUSY;
		} else {
			rc = dsi_panel_m2468_tx_cmd_set(panel, commands[mode]);
			if (!rc) {
				WRITE_ONCE(bl->global_hbm, mode != M2468_HBM_BRIGHTNESS_OFF);
				m2468_hbm_state_changed(panel);
			}
		}
	}
	if (!rc)
		bl->hbm_mode = mode;
	if (!bl->global_hbm && !bl->local_hbm) {
		tmp = m2468_display_adfr_hbm_locked(panel, false);
		if (!rc)
			rc = tmp;
	}
unlock_panel:
	mutex_unlock(&panel->panel_lock);
	if (clocks_on) {
		tmp = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_CORE_CLK, DSI_CLK_OFF);
		if (!rc)
			rc = tmp;
	}
unlock_display:
	mutex_unlock(&display->display_lock);
	return rc;
}

int m2468_display_hbm_aod_enter(struct dsi_panel *panel)
{
	struct m2468_panel_backlight *bl = &panel->m2468_bl;
	int rc;

	if (!bl->supported)
		return 0;
	if (m2468_display_adfr_full_aod() || bl->aod_active ||
	    bl->local_hbm || bl->global_hbm)
		return 1;
	rc = dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_LP1);
	if (rc)
		return rc;
	rc = dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_LP2);
	if (rc)
		return rc;
	bl->aod_active = true;
	m2468_display_adfr_aod_enter(panel);
	return 1;
}

int m2468_display_hbm_aod_exit(struct dsi_panel *panel)
{
	struct m2468_panel_backlight *bl = &panel->m2468_bl;
	int rc;

	if (!bl->supported)
		return 0;
	if (!m2468_display_adfr_full_aod() && bl->aod_active) {
		rc = dsi_panel_m2468_tx_cmd_set(panel, DSI_CMD_SET_NOLP);
		if (rc)
			return rc;
		bl->aod_active = false;
	}
	m2468_display_adfr_aod_exit(panel);
	return 1;
}

static ssize_t hbm_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct dsi_display *display = READ_ONCE(m2468_hbm.display);

	if (!display)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", READ_ONCE(display->panel->m2468_bl.hbm_mode));
}

static ssize_t hbm_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	u32 mode;
	int rc;

	if (kstrtou32(buf, 0, &mode))
		return -EINVAL;
	rc = m2468_hbm_set_mode(READ_ONCE(m2468_hbm.display), mode);
	return rc ? rc : count;
}

static ssize_t hbm_ready_status_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&m2468_hbm_ready));
}

static ssize_t fp_result_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	u32 result;

	if (kstrtou32(buf, 0, &result) || result > 1)
		return -EINVAL;
	atomic_set(&m2468_fp_result, result);
	return count;
}

static ssize_t ofp_press_status_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&m2468_ofp_press));
}

static ssize_t ofp_press_status_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	u32 pressed;

	if (kstrtou32(buf, 0, &pressed) || pressed > 1)
		return -EINVAL;
	atomic_set(&m2468_ofp_press, pressed);
	return count;
}

static ssize_t panel_status_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct dsi_display *display = READ_ONCE(m2468_hbm.display);

	if (!display)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", !!READ_ONCE(display->panel->bl_config.bl_level));
}

static ssize_t panel_name_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct dsi_display *display = READ_ONCE(m2468_hbm.display);

	if (!display)
		return -ENODEV;
	return sysfs_emit(buf, "%s\n", display->panel->name);
}

static ssize_t power_mode_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct dsi_display *display = READ_ONCE(m2468_hbm.display);

	if (!display)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", READ_ONCE(display->panel->power_mode));
}

static struct kobj_attribute hbm_attr = __ATTR_RW(hbm);
static struct kobj_attribute hbm_ready_attr = __ATTR_RO(hbm_ready_status);
static struct kobj_attribute fp_result_attr = __ATTR_WO(fp_result);
static struct kobj_attribute ofp_press_attr = __ATTR_RW(ofp_press_status);
static struct kobj_attribute panel_status_attr = __ATTR_RO(panel_status);
static struct kobj_attribute panel_name_attr = __ATTR_RO(panel_name);
static struct kobj_attribute power_mode_attr = __ATTR_RO(power_mode);

static struct attribute *m2468_hbm_attrs[] = {
	&hbm_attr.attr,
	&hbm_ready_attr.attr,
	&fp_result_attr.attr,
	&ofp_press_attr.attr,
	&panel_status_attr.attr,
	&panel_name_attr.attr,
	&power_mode_attr.attr,
	NULL,
};

static const struct attribute_group m2468_hbm_attr_group = {
	.attrs = m2468_hbm_attrs,
};

int m2468_display_hbm_init(struct dsi_display *display, struct kobject *kobj)
{
	struct m2468_panel_backlight *bl;
	struct device_node *np;
	struct workqueue_struct *wq;
	unsigned long flags;
	u32 enabled = 0;
	int rc;

	if (!display || !m2468_display_panel_supported(display->panel))
		return 0;
	bl = &display->panel->m2468_bl;
	np = display->panel->panel_of_node;
	bl->normal_max = 3153;
	bl->dc_min = 1107;
	bl->ready_vblanks = 1;
	of_property_read_u32(np, "qcom,mdss-brightness-normal-max-level", &bl->normal_max);
	of_property_read_u32(np, "qcom,mdss-dc-min-brightness", &bl->dc_min);
	of_property_read_u32(np, "qcom,mdss-pwm-dc-siwtch", &enabled);
	bl->pwm_dc_enabled = !!enabled;
	enabled = 0;
	of_property_read_u32(np, "xjmz,display-ofp-enable-config", &enabled);
	bl->ofp_enabled = !!enabled;
	of_property_read_u32(np, "xjmz,display-ofp-type-config", &bl->ofp_type);
	of_property_read_u32(np, "xjmz,display-ofp-hbm-take-effect-vblank-config",
			     &bl->ready_vblanks);
	bl->ready_vblanks = max(bl->ready_vblanks, 1U);
	bl->requested_bl = display->panel->bl_config.bl_level;
	bl->effective_bl = bl->requested_bl;
	bl->hbm_mode = M2468_HBM_OFF;
	bl->supported = true;
	INIT_WORK(&m2468_hbm.notify_work, m2468_hbm_notify_work);
	wq = alloc_ordered_workqueue("m2468_ofp", WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!wq) {
		bl->supported = false;
		return -ENOMEM;
	}
	spin_lock_irqsave(&m2468_hbm_lock, flags);
	m2468_hbm.display = display;
	m2468_hbm.kobj = kobj;
	m2468_hbm.wq = wq;
	m2468_hbm.pending = false;
	m2468_hbm.target = false;
	m2468_hbm.notified = false;
	m2468_hbm.sequence = 0;
	m2468_hbm.vblank_count = 0;
	atomic_set(&m2468_hbm_ready, 0);
	spin_unlock_irqrestore(&m2468_hbm_lock, flags);
	rc = sysfs_create_group(kobj, &m2468_hbm_attr_group);
	if (rc) {
		spin_lock_irqsave(&m2468_hbm_lock, flags);
		m2468_hbm.display = NULL;
		m2468_hbm.kobj = NULL;
		m2468_hbm.wq = NULL;
		spin_unlock_irqrestore(&m2468_hbm_lock, flags);
		bl->supported = false;
		destroy_workqueue(wq);
	}
	return rc;
}

void m2468_display_hbm_deinit(struct kobject *kobj)
{
	struct workqueue_struct *wq;
	unsigned long flags;

	sysfs_remove_group(kobj, &m2468_hbm_attr_group);
	spin_lock_irqsave(&m2468_hbm_lock, flags);
	wq = m2468_hbm.wq;
	m2468_hbm.wq = NULL;
	m2468_hbm.display = NULL;
	m2468_hbm.pending = false;
	spin_unlock_irqrestore(&m2468_hbm_lock, flags);
	if (wq)
		destroy_workqueue(wq);
	m2468_hbm.kobj = NULL;
}
