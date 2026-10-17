// SPDX-License-Identifier: GPL-2.0-only
/* M2468 protocols, based on the M2481 Goodix adaptation by guosasipo. */
#include <linux/power_supply.h>

#include "goodix_ts_core.h"

#define GOODIX_M2468_GESTURE_CMD		0xa6
#define GOODIX_M2468_TAP_DISABLE_MASK	0x1fff
#define GOODIX_M2468_FOD_DISABLE_MASK	0x2000

/* Caller holds report_rate_lock; direct writes share send_cmd's mailbox lock. */
static int goodix_m2468_write(struct goodix_ts_core *cd,
			      struct goodix_ts_cmd *cmd)
{
	int ret;

	if (cd->report_rate_blocked & ~GOODIX_RATE_SUSPEND)
		return -EBUSY;
	if (!cd->report_rate_ready || !cd->report_rate_powered ||
	    !cd->ic_info.misc.cmd_addr)
		return -EAGAIN;
	goodix_append_checksum(&cmd->buf[2], cmd->len - 2,
			       CHECKSUM_MODE_U8_LE);
	mutex_lock(&cd->cmd_lock);
	ret = cd->hw_ops->write(cd, cd->ic_info.misc.cmd_addr,
				cmd->buf, cmd->len + 2);
	if (!ret)
		msleep(20);
	mutex_unlock(&cd->cmd_lock);
	return ret > 0 ? -EIO : ret;
}

/* Stock goodix_FOD_function: GIO4 is controlled independently of tap masks. */
static int goodix_m2468_gio4_locked(struct goodix_ts_core *cd, bool enable)
{
	struct goodix_ts_cmd cmd = {
		.len = 6,
		.cmd = GOODIX_M2468_GESTURE_CMD,
	};

	cmd.data[1] = enable ? 0x40 : 0x60;
	return goodix_m2468_write(cd, &cmd);
}

int goodix_m2468_set_gio4(struct goodix_ts_core *cd, bool enable)
{
	int ret;

	mutex_lock(&cd->report_rate_lock);
	ret = goodix_m2468_gio4_locked(cd, enable);
	mutex_unlock(&cd->report_rate_lock);
	return ret;
}

int goodix_m2468_send_gesture(struct goodix_ts_core *cd, unsigned int type)
{
	struct goodix_ts_cmd cmd = {
		.len = 6,
		.cmd = GOODIX_M2468_GESTURE_CMD,
	};
	u16 mask = GOODIX_M2468_TAP_DISABLE_MASK |
		   GOODIX_M2468_FOD_DISABLE_MASK;
	int ret;

	/* The firmware enables the tap group; input filters individual tap types. */
	if (type & (GESTURE_SINGLE_TAP | GESTURE_DOUBLE_TAP))
		mask &= ~GOODIX_M2468_TAP_DISABLE_MASK;
	if (type & GESTURE_FOD_PRESS)
		mask &= ~GOODIX_M2468_FOD_DISABLE_MASK;
	cmd.data[0] = mask & 0xff;
	cmd.data[1] = mask >> 8;

	mutex_lock(&cd->report_rate_lock);
	/* Stock tap-only suspend disables GIO4 before selecting the tap mask. */
	if (!(type & GESTURE_FOD_PRESS)) {
		ret = goodix_m2468_gio4_locked(cd, false);
		if (ret)
			goto out;
	}
	ret = goodix_m2468_write(cd, &cmd);
out:
	mutex_unlock(&cd->report_rate_lock);
	return ret;
}

int goodix_m2468_sleep(struct goodix_ts_core *cd)
{
	struct goodix_ts_cmd cmd = { .len = 4, .cmd = 0x84 };
	int ret;

	mutex_lock(&cd->report_rate_lock);
	ret = goodix_m2468_write(cd, &cmd);
	mutex_unlock(&cd->report_rate_lock);
	return ret;
}

/* M2468 stock: USB insertion AND removal use 0xaf, with a Boolean payload. */
static int goodix_m2468_send_usb(struct goodix_ts_core *cd, bool online)
{
	struct goodix_ts_cmd cmd = { .len = 5, .cmd = 0xaf };

	cmd.data[0] = online;
	return cd->hw_ops->send_cmd(cd, &cmd);
}

void goodix_m2468_restore_usb_locked(struct goodix_ts_core *cd)
{
	struct goodix_m2468_state *state = &cd->m2468;
	int ret;

	if (!state->usb_registered || state->usb_online < 0 ||
	    state->usb_online == state->usb_applied ||
	    !cd->report_rate_ready || !cd->report_rate_powered ||
	    cd->report_rate_blocked || !cd->ic_info.misc.cmd_addr)
		return;
	ret = goodix_m2468_send_usb(cd, state->usb_online);
	if (ret)
		ts_err("M2468 USB noise-mode command failed: %d", ret);
	else
		state->usb_applied = state->usb_online;
}

static void goodix_m2468_usb_work(struct work_struct *work)
{
	struct goodix_m2468_state *state =
		container_of(work, struct goodix_m2468_state, usb_work);
	struct goodix_ts_core *cd = state->core;
	union power_supply_propval value;
	struct power_supply *supply;
	int ret;

	supply = power_supply_get_by_name("usb");
	if (!supply)
		return;
	ret = power_supply_get_property(supply, POWER_SUPPLY_PROP_ONLINE, &value);
	power_supply_put(supply);
	if (ret)
		return;

	mutex_lock(&cd->report_rate_lock);
	state->usb_online = !!value.intval;
	goodix_m2468_restore_usb_locked(cd);
	mutex_unlock(&cd->report_rate_lock);
}

static int goodix_m2468_usb_notifier(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct goodix_m2468_state *state =
		container_of(nb, struct goodix_m2468_state, usb_notifier);
	struct power_supply *supply = data;

	if (event != PSY_EVENT_PROP_CHANGED || !supply || !supply->desc ||
	    strcmp(supply->desc->name, "usb"))
		return NOTIFY_DONE;
	schedule_work(&state->usb_work);
	return NOTIFY_OK;
}

void goodix_m2468_init(struct goodix_ts_core *cd)
{
	struct goodix_m2468_state *state = &cd->m2468;

	mutex_init(&state->pm_lock);
	state->core = cd;
	state->usb_online = -1;
	state->usb_applied = -1;
	INIT_WORK(&state->usb_work, goodix_m2468_usb_work);
	state->usb_notifier.notifier_call = goodix_m2468_usb_notifier;
}

int goodix_m2468_usb_init(struct goodix_ts_core *cd)
{
	struct goodix_m2468_state *state = &cd->m2468;
	int ret;

	/* Publish readiness before registration can call back and schedule work. */
	mutex_lock(&cd->report_rate_lock);
	state->usb_registered = true;
	mutex_unlock(&cd->report_rate_lock);
	ret = power_supply_reg_notifier(&state->usb_notifier);
	if (ret) {
		mutex_lock(&cd->report_rate_lock);
		state->usb_registered = false;
		mutex_unlock(&cd->report_rate_lock);
		return ret;
	}
	schedule_work(&state->usb_work);
	return 0;
}

void goodix_m2468_usb_exit(struct goodix_ts_core *cd)
{
	struct goodix_m2468_state *state = &cd->m2468;

	if (!state->usb_registered)
		return;
	power_supply_unreg_notifier(&state->usb_notifier);
	cancel_work_sync(&state->usb_work);
	mutex_lock(&cd->report_rate_lock);
	state->usb_registered = false;
	mutex_unlock(&cd->report_rate_lock);
}
