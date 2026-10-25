/*
 * Goodix Gesture Module
 *
 * Copyright (C) 2019 - 2020 Goodix, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/input/mt.h>
#include "goodix_ts_core.h"


#define GOODIX_GESTURE_DOUBLE_TAP		0xCC
#define GOODIX_GESTURE_SINGLE_TAP		0x4C
#define GOODIX_GESTURE_FOD_DOWN			0x46
#define GOODIX_GESTURE_FOD_UP			0x55

/*
 * struct gesture_module - gesture module data
 * @registered: module register state
 * @sysfs_node_created: sysfs node state
 * @gesture_type: valid gesture type, each bit represent one gesture type
 * @gesture_data: store latest gesture code get from irq event
 * @gesture_ts_cmd: gesture command data
 */
struct gesture_module {
	atomic_t registered;
	struct mutex lock; /* requested gestures, active mode and FOD input */
	struct delayed_work fod_up_work;
	struct delayed_work gio4_rearm_work;
	bool gio4_rearm_pending;
	bool active;
	bool wake_armed;
	bool fod_pressed;
	struct goodix_ts_core *ts_core;
	struct goodix_ext_module module;
};

static struct gesture_module *gsx_gesture; /*allocated in gesture init module*/
static bool module_initialized;

/* A confirmed stock timeout bounds a lost FOD-UP to three seconds. */
#define GOODIX_M2468_FOD_TIMEOUT_MS 3000

static void gsx_release_fod(struct gesture_module *gsx)
{
	struct goodix_ts_core *cd = gsx->ts_core;

	if (!gsx->fod_pressed)
		return;
	input_report_key(cd->input_dev, BTN_TOUCH, 0);
	input_mt_slot(cd->input_dev, 0);
	input_mt_report_slot_state(cd->input_dev, MT_TOOL_FINGER, false);
	input_sync(cd->input_dev);
	gsx->fod_pressed = false;
}

static void gsx_fod_up_work(struct work_struct *work)
{
	struct gesture_module *gsx = container_of(to_delayed_work(work),
						 struct gesture_module, fod_up_work);
	struct goodix_ts_core *cd = gsx->ts_core;
	int ret;

	mutex_lock(&gsx->lock);
	if (atomic_read(&gsx->registered) && gsx->fod_pressed) {
		gsx_release_fod(gsx);
		if (gsx->active && atomic_read(&cd->suspended) &&
		    (cd->gesture_type & GESTURE_FOD_PRESS)) {
			ret = goodix_m2468_set_gio4(cd, false);
			if (ret)
				ts_err("M2468 GIO4 timeout disable failed: %d", ret);
			/* Stock re-enables GIO4 three seconds after the timeout. */
			gsx->gio4_rearm_pending = true;
			mod_delayed_work(system_wq, &gsx->gio4_rearm_work,
					 msecs_to_jiffies(GOODIX_M2468_FOD_TIMEOUT_MS));
		}
	}
	mutex_unlock(&gsx->lock);
}

static void gsx_gio4_rearm_work(struct work_struct *work)
{
	struct gesture_module *gsx = container_of(to_delayed_work(work),
						 struct gesture_module, gio4_rearm_work);
	struct goodix_ts_core *cd = gsx->ts_core;
	int ret;

	mutex_lock(&gsx->lock);
	if (gsx->gio4_rearm_pending && atomic_read(&gsx->registered) &&
	    gsx->active && atomic_read(&cd->suspended) &&
	    (cd->gesture_type & GESTURE_FOD_PRESS)) {
		ret = goodix_m2468_set_gio4(cd, true);
		if (ret)
			ts_err("M2468 GIO4 re-enable failed: %d", ret);
	}
	gsx->gio4_rearm_pending = false;
	mutex_unlock(&gsx->lock);
}

static int gsx_set_type(struct gesture_module *gsx, unsigned int bit, bool enable)
{
	struct goodix_ts_core *cd = gsx->ts_core;
	unsigned int type;
	int ret = 0;

	if (!cd->is_m2468) {
		if (enable)
			cd->gesture_type |= bit;
		else
			cd->gesture_type &= ~bit;
		return 0;
	}
	mutex_lock(&gsx->lock);
	type = enable ? cd->gesture_type | bit : cd->gesture_type & ~bit;
	if (!atomic_read(&gsx->registered)) {
		ret = -ENODEV;
		goto out;
	}
	if (gsx->active) {
		ret = cd->hw_ops->gesture(cd, type);
		if (ret) {
			int restore = 0;

			/* A failed mask write may follow a successful GIO4 disable. */
			if (cd->gesture_type & GESTURE_FOD_PRESS)
				restore = goodix_m2468_set_gio4(cd, true);
			if (!restore)
				restore = cd->hw_ops->gesture(cd, cd->gesture_type);
			if (restore)
				ts_err("M2468 gesture rollback failed: %d", restore);
			goto out;
		}
	} else if (atomic_read(&cd->suspended)) {
		/* Deep sleep cannot accept gesture commands until normal resume. */
		ret = -EBUSY;
		goto out;
	}
	cd->gesture_type = type;
	if (!(type & GESTURE_FOD_PRESS)) {
		cancel_delayed_work(&gsx->gio4_rearm_work);
		gsx->gio4_rearm_pending = false;
		cancel_delayed_work(&gsx->fod_up_work);
		gsx_release_fod(gsx);
	}
out:
	mutex_unlock(&gsx->lock);
	return ret;
}

static unsigned int gsx_get_type(struct gesture_module *gsx)
{
	unsigned int type;

	mutex_lock(&gsx->lock);
	type = gsx->ts_core->gesture_type;
	mutex_unlock(&gsx->lock);
	return type;
}

static ssize_t gsx_double_type_show(struct goodix_ext_module *module,
		char *buf)
{
	struct gesture_module *gsx = module->priv_data;
	unsigned char type;

	if (!gsx)
		return -EIO;

	if (atomic_read(&gsx->registered) == 0) {
		ts_err("gesture module is not registered");
		return 0;
	}

	type = gsx_get_type(gsx);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			(type & GESTURE_DOUBLE_TAP) ? "enable" : "disable");
}

static ssize_t gsx_double_type_store(struct goodix_ext_module *module,
		const char *buf, size_t count)
{
	struct gesture_module *gsx = module->priv_data;
	int ret;

	if (!gsx)
		return -EIO;

	if (atomic_read(&gsx->registered) == 0) {
		ts_err("gesture module is not registered");
		return 0;
	}

	if (!count)
		return -EINVAL;

	if (buf[0] == '1') {
		ts_info("enable double tap");
		ret = gsx_set_type(gsx, GESTURE_DOUBLE_TAP, true);
		if (ret)
			return ret;
	} else if (buf[0] == '0') {
		ts_info("disable double tap");
		ret = gsx_set_type(gsx, GESTURE_DOUBLE_TAP, false);
		if (ret)
			return ret;
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t gsx_single_type_show(struct goodix_ext_module *module,
		char *buf)
{
	struct gesture_module *gsx = module->priv_data;
	unsigned char type;

	if (!gsx)
		return -EIO;

	if (atomic_read(&gsx->registered) == 0) {
		ts_err("gesture module is not registered");
		return 0;
	}

	type = gsx_get_type(gsx);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			(type & GESTURE_SINGLE_TAP) ? "enable" : "disable");
}

static ssize_t gsx_single_type_store(struct goodix_ext_module *module,
		const char *buf, size_t count)
{
	struct gesture_module *gsx = module->priv_data;
	int ret;

	if (!gsx)
		return -EIO;

	if (atomic_read(&gsx->registered) == 0) {
		ts_err("gesture module is not registered");
		return 0;
	}

	if (!count)
		return -EINVAL;

	if (buf[0] == '1') {
		ts_info("enable single tap");
		ret = gsx_set_type(gsx, GESTURE_SINGLE_TAP, true);
		if (ret)
			return ret;
	} else if (buf[0] == '0') {
		ts_info("disable single tap");
		ret = gsx_set_type(gsx, GESTURE_SINGLE_TAP, false);
		if (ret)
			return ret;
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t gsx_fod_type_show(struct goodix_ext_module *module,
		char *buf)
{
	struct gesture_module *gsx = module->priv_data;
	unsigned char type;

	if (!gsx)
		return -EIO;

	if (atomic_read(&gsx->registered) == 0) {
		ts_err("gesture module is not registered");
		return 0;
	}

	type = gsx_get_type(gsx);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			(type & GESTURE_FOD_PRESS) ? "enable" : "disable");
}

static ssize_t gsx_fod_type_store(struct goodix_ext_module *module,
		const char *buf, size_t count)
{
	struct gesture_module *gsx = module->priv_data;
	int ret;

	if (!gsx)
		return -EIO;

	if (atomic_read(&gsx->registered) == 0) {
		ts_err("gesture module is not registered");
		return 0;
	}

	if (!count)
		return -EINVAL;

	if (buf[0] == '1') {
		ts_info("enable fod");
		ret = gsx_set_type(gsx, GESTURE_FOD_PRESS, true);
		if (ret)
			return ret;
	} else if (buf[0] == '0') {
		ts_info("disable fod");
		ret = gsx_set_type(gsx, GESTURE_FOD_PRESS, false);
		if (ret)
			return ret;
	} else {
		return -EINVAL;
	}

	return count;
}


const struct goodix_ext_attribute gesture_attrs[] = {
	__EXTMOD_ATTR(double_en, 0664,
			gsx_double_type_show, gsx_double_type_store),
	__EXTMOD_ATTR(single_en, 0664,
			gsx_single_type_show, gsx_single_type_store),
	__EXTMOD_ATTR(fod_en, 0664,
			gsx_fod_type_show, gsx_fod_type_store),
};

static int gsx_gesture_init(struct goodix_ts_core *cd,
		struct goodix_ext_module *module)
{
	struct gesture_module *gsx = module->priv_data;

	if (!cd || !cd->hw_ops->gesture) {
		ts_err("gesture unsupported");
		return -EINVAL;
	}

	gsx->ts_core = cd;
	gsx->ts_core->gesture_type = cd->is_m2468 ? GESTURE_FOD_PRESS : 0;
	atomic_set(&gsx->registered, 1);

	return 0;
}

static int gsx_gesture_exit(struct goodix_ts_core *cd,
		struct goodix_ext_module *module)
{
	struct gesture_module *gsx = module->priv_data;

	if (!cd || !cd->hw_ops->gesture) {
		ts_err("gesture unsupported");
		return -EINVAL;
	}

	atomic_set(&gsx->registered, 0);

	return 0;
}

static int gsx_m2468_ist(struct goodix_ts_core *cd,
			 struct gesture_module *gsx)
{
	struct goodix_ts_event event = { 0 };
	int x, y, key, ret;

	mutex_lock(&gsx->lock);
	if (!gsx->active) {
		mutex_unlock(&gsx->lock);
		return EVT_CONTINUE;
	}
	ret = cd->hw_ops->event_handler(cd, &event);
	if (ret || !(event.event_type & EVENT_GESTURE))
		goto out;

	switch (event.gesture_type) {
	case GOODIX_GESTURE_SINGLE_TAP:
	case GOODIX_GESTURE_DOUBLE_TAP:
		if (!(cd->gesture_type &
		      (event.gesture_type == GOODIX_GESTURE_SINGLE_TAP ?
		       GESTURE_SINGLE_TAP : GESTURE_DOUBLE_TAP)))
			break;
		key = event.gesture_type == GOODIX_GESTURE_DOUBLE_TAP ?
			KEY_NEXT_FAVORITE : KEY_WAKEUP;
		/* Stock mz_gesture_report(value=2) emits a press/release pulse. */
		input_report_key(cd->input_dev, key, 1);
		input_sync(cd->input_dev);
		input_report_key(cd->input_dev, key, 0);
		input_sync(cd->input_dev);
		break;
	case GOODIX_GESTURE_FOD_DOWN:
		if (!(cd->gesture_type & GESTURE_FOD_PRESS))
			break;
		x = get_unaligned_le16(event.gesture_data);
		y = get_unaligned_le16(event.gesture_data + 2);
		if (cd->board_data.invert_xy)
			swap(x, y);
		input_report_key(cd->input_dev, BTN_TOUCH, 1);
		input_mt_slot(cd->input_dev, 0);
		input_mt_report_slot_state(cd->input_dev, MT_TOOL_FINGER, true);
		input_report_abs(cd->input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(cd->input_dev, ABS_MT_POSITION_Y, y);
		input_report_abs(cd->input_dev, ABS_MT_WIDTH_MAJOR,
				 event.gesture_data[4]);
		input_sync(cd->input_dev);
		gsx->fod_pressed = true;
		mod_delayed_work(system_wq, &gsx->fod_up_work,
				 msecs_to_jiffies(GOODIX_M2468_FOD_TIMEOUT_MS));
		break;
	case GOODIX_GESTURE_FOD_UP:
		cancel_delayed_work(&gsx->fod_up_work);
		gsx_release_fod(gsx);
		break;
	default:
		break;
	}
out:
	mutex_unlock(&gsx->lock);
	return EVT_CANCEL_IRQEVT;
}

/**
 * gsx_gesture_ist - Gesture Irq handle
 * This functions is excuted when interrupt happended and
 * ic in doze mode.
 *
 * @cd: pointer to touch core data
 * @module: pointer to goodix_ext_module struct
 * return: 0 goon execute, EVT_CANCEL_IRQEVT  stop execute
 */
static int gsx_gesture_ist(struct goodix_ts_core *cd,
	struct goodix_ext_module *module)
{
	struct goodix_ts_hw_ops *hw_ops = cd->hw_ops;
	struct goodix_ts_event gs_event = {0};
	int fodx, fody, overlay_area;
	int ret;

	if (cd->is_m2468)
		return gsx_m2468_ist(cd, module->priv_data);
	if (atomic_read(&cd->suspended) == 0 || cd->gesture_type == 0)
		return EVT_CONTINUE;

	ret = hw_ops->event_handler(cd, &gs_event);
	if (ret) {
		ts_err("failed get gesture data");
		goto re_send_ges_cmd;
	}

	if (!(gs_event.event_type & EVENT_GESTURE)) {
		ts_err("invalid event type: 0x%x",
			cd->ts_event.event_type);
		goto re_send_ges_cmd;
	}

	switch (gs_event.gesture_type) {
	case GOODIX_GESTURE_SINGLE_TAP:
		if (cd->gesture_type & GESTURE_SINGLE_TAP) {
			ts_info("get SINGLE-TAP gesture");
			input_report_key(cd->input_dev, KEY_WAKEUP, 1);
			// input_report_key(cd->input_dev, KEY_GOTO, 1);
			input_sync(cd->input_dev);
			input_report_key(cd->input_dev, KEY_WAKEUP, 0);
			// input_report_key(cd->input_dev, KEY_GOTO, 0);
			input_sync(cd->input_dev);
		} else {
			ts_debug("not enable SINGLE-TAP");
		}
		break;
	case GOODIX_GESTURE_DOUBLE_TAP:
		if (cd->gesture_type & GESTURE_DOUBLE_TAP) {
			ts_info("get DOUBLE-TAP gesture");
			input_report_key(cd->input_dev, KEY_WAKEUP, 1);
			input_sync(cd->input_dev);
			input_report_key(cd->input_dev, KEY_WAKEUP, 0);
			input_sync(cd->input_dev);
		} else {
			ts_debug("not enable DOUBLE-TAP");
		}
		break;
	case GOODIX_GESTURE_FOD_DOWN:
		if (cd->gesture_type & GESTURE_FOD_PRESS) {
			ts_info("get FOD-DOWN gesture");
			fodx = le16_to_cpup((__le16 *)gs_event.gesture_data);
			fody = le16_to_cpup((__le16 *)(gs_event.gesture_data + 2));
			overlay_area = gs_event.gesture_data[4];
			ts_debug("fodx:%d fody:%d overlay_area:%d", fodx, fody, overlay_area);
			input_report_key(cd->input_dev, BTN_TOUCH, 1);
			input_mt_slot(cd->input_dev, 0);
			input_mt_report_slot_state(cd->input_dev, MT_TOOL_FINGER, 1);
			input_report_abs(cd->input_dev, ABS_MT_POSITION_X, fodx);
			input_report_abs(cd->input_dev, ABS_MT_POSITION_Y, fody);
			input_report_abs(cd->input_dev, ABS_MT_WIDTH_MAJOR, overlay_area);
			input_sync(cd->input_dev);
		} else {
			ts_debug("not enable FOD-DOWN");
		}
		break;
	case GOODIX_GESTURE_FOD_UP:
		if (cd->gesture_type & GESTURE_FOD_PRESS) {
			ts_info("get FOD-UP gesture");
			fodx = le16_to_cpup((__le16 *)gs_event.gesture_data);
			fody = le16_to_cpup((__le16 *)(gs_event.gesture_data + 2));
			overlay_area = gs_event.gesture_data[4];
			input_report_key(cd->input_dev, BTN_TOUCH, 0);
			input_mt_slot(cd->input_dev, 0);
			input_mt_report_slot_state(cd->input_dev,
					MT_TOOL_FINGER, 0);
			input_sync(cd->input_dev);
		} else {
			ts_debug("not enable FOD-UP");
		}
		break;
	default:
		ts_err("not support gesture type[%02X]", gs_event.gesture_type);
		break;
	}

re_send_ges_cmd:
	if (hw_ops->gesture(cd, 0))
		ts_info("warning: failed re_send gesture cmd");
	return EVT_CANCEL_IRQEVT;
}

/**
 * gsx_gesture_before_suspend - execute gesture suspend routine
 * This functions is excuted to set ic into doze mode
 *
 * @cd: pointer to touch core data
 * @module: pointer to goodix_ext_module struct
 * return: 0 goon execute, EVT_IRQCANCLED  stop execute
 */
static int gsx_gesture_before_suspend(struct goodix_ts_core *cd,
	struct goodix_ext_module *module)
{
	struct gesture_module *gsx = module->priv_data;
	const struct goodix_ts_hw_ops *hw_ops = cd->hw_ops;
	int ret;

	if (cd->is_m2468) {
		mutex_lock(&gsx->lock);
		if (!cd->gesture_type) {
			mutex_unlock(&gsx->lock);
			return EVT_CONTINUE;
		}
		ret = hw_ops->gesture(cd, cd->gesture_type);
		if (ret)
			goto fail;
		if (!gsx->wake_armed) {
			ret = enable_irq_wake(cd->irq);
			if (ret)
				goto fail;
			gsx->wake_armed = true;
		}
		gsx->active = true;
		hw_ops->irq_enable(cd, true);
		mutex_unlock(&gsx->lock);
		return EVT_CANCEL_SUSPEND;
fail:
		ts_err("M2468 gesture suspend failed: %d", ret);
		mutex_unlock(&gsx->lock);
		/* The core falls back to the ordinary sleep command. */
		return EVT_CONTINUE;
	}

	if (cd->gesture_type == 0)
		return EVT_CONTINUE;
	ret = hw_ops->gesture(cd, 0);
	if (ret)
		ts_err("failed enter gesture mode");
	hw_ops->irq_enable(cd, true);
	enable_irq_wake(cd->irq);
	return EVT_CANCEL_SUSPEND;
}

static int gsx_gesture_before_resume(struct goodix_ts_core *cd,
	struct goodix_ext_module *module)
{
	struct gesture_module *gsx = module->priv_data;
	const struct goodix_ts_hw_ops *hw_ops = cd->hw_ops;
	int ret;

	if (cd->is_m2468) {
		cancel_delayed_work_sync(&gsx->fod_up_work);
		cancel_delayed_work_sync(&gsx->gio4_rearm_work);
		mutex_lock(&gsx->lock);
		gsx->gio4_rearm_pending = false;
		gsx_release_fod(gsx);
		/* Requested bits may have changed while asleep. */
		if (gsx->wake_armed) {
			ret = disable_irq_wake(cd->irq);
			if (ret)
				goto out;
			gsx->wake_armed = false;
		}
		if (!gsx->active) {
			mutex_unlock(&gsx->lock);
			return EVT_CONTINUE;
		}
		ret = hw_ops->reset(cd, GOODIX_NORMAL_RESET_DELAY_MS);
		if (!ret)
			gsx->active = false;
out:
		mutex_unlock(&gsx->lock);
		return ret ? ret : EVT_CANCEL_RESUME;
	}

	if (cd->gesture_type == 0)
		return EVT_CONTINUE;
	disable_irq_wake(cd->irq);
	hw_ops->reset(cd, GOODIX_NORMAL_RESET_DELAY_MS);
	return EVT_CANCEL_RESUME;
}

static struct goodix_ext_module_funcs gsx_gesture_funcs = {
	.irq_event = gsx_gesture_ist,
	.init = gsx_gesture_init,
	.exit = gsx_gesture_exit,
	.before_suspend = gsx_gesture_before_suspend,
	.before_resume = gsx_gesture_before_resume,
};

int gesture_module_init(struct goodix_ts_core *cd)
{
	int ret;
	int i;
	struct kobject *def_kobj = goodix_get_default_kobj();
	struct kobj_type *def_kobj_type = goodix_get_default_ktype();

	gsx_gesture = kzalloc(sizeof(struct gesture_module), GFP_KERNEL);
	if (!gsx_gesture)
		return -ENOMEM;

	gsx_gesture->module.funcs = &gsx_gesture_funcs;
	gsx_gesture->module.priority = EXTMOD_PRIO_GESTURE;
	gsx_gesture->module.name = "Goodix_gsx_gesture";
	gsx_gesture->module.priv_data = gsx_gesture;

	atomic_set(&gsx_gesture->registered, 0);
	mutex_init(&gsx_gesture->lock);
	INIT_DELAYED_WORK(&gsx_gesture->fod_up_work, gsx_fod_up_work);
	INIT_DELAYED_WORK(&gsx_gesture->gio4_rearm_work, gsx_gio4_rearm_work);

	/* gesture sysfs init */
	ret = kobject_init_and_add(&gsx_gesture->module.kobj,
			def_kobj_type, def_kobj, "gesture");
	if (ret) {
		ts_err("failed create gesture sysfs node!");
		kobject_put(&gsx_gesture->module.kobj);
		goto err_out;
	}

	for (i = 0; i < ARRAY_SIZE(gesture_attrs) && !ret; i++)
		ret = sysfs_create_file(&gsx_gesture->module.kobj,
				&gesture_attrs[i].attr);
	if (ret) {
		ts_err("failed create gst sysfs files");
		while (--i >= 0)
			sysfs_remove_file(&gsx_gesture->module.kobj,
					&gesture_attrs[i].attr);

		kobject_put(&gsx_gesture->module.kobj);
		goto err_out;
	}

	gsx_gesture->ts_core = cd;
	module_initialized = true;
	ret = goodix_register_ext_module_no_wait(&gsx_gesture->module);
	if (ret) {
		gesture_module_exit();
		return ret;
	}
	if (cd->is_m2468) {
		ret = kobject_uevent(def_kobj, KOBJ_CHANGE);
		if (ret)
			ts_err("failed send gesture uevent, ret:%d", ret);
	}
	ts_info("gesture module init success");

	return 0;

err_out:
	ts_err("gesture module init failed!");
	kfree(gsx_gesture);
	gsx_gesture = NULL;
	return ret;
}

void gesture_module_exit(void)
{
	int i;

	ts_info("gesture module exit");
	if (!module_initialized)
		return;

	goodix_unregister_ext_module(&gsx_gesture->module);
	cancel_delayed_work_sync(&gsx_gesture->fod_up_work);
	cancel_delayed_work_sync(&gsx_gesture->gio4_rearm_work);
	mutex_lock(&gsx_gesture->lock);
	gsx_gesture->gio4_rearm_pending = false;
	gsx_release_fod(gsx_gesture);
	if (gsx_gesture->wake_armed) {
		int ret = disable_irq_wake(gsx_gesture->ts_core->irq);

		if (ret)
			ts_err("M2468 gesture wake cleanup failed: %d", ret);
		else
			gsx_gesture->wake_armed = false;
	}
	mutex_unlock(&gsx_gesture->lock);

	/* deinit sysfs */
	for (i = 0; i < ARRAY_SIZE(gesture_attrs); i++)
		sysfs_remove_file(&gsx_gesture->module.kobj,
					&gesture_attrs[i].attr);

	kobject_put(&gsx_gesture->module.kobj);
	kfree(gsx_gesture);
	gsx_gesture = NULL;
	module_initialized = false;
}
