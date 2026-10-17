/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GOODIX_M2468_H_
#define _GOODIX_M2468_H_

#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

struct goodix_ts_core;

struct goodix_m2468_state {
	struct mutex pm_lock; /* suspend/resume versus runtime firmware update */
	struct goodix_ts_core *core;
	struct notifier_block usb_notifier;
	struct work_struct usb_work;
	/* Firmware state is protected by report_rate_lock. */
	int usb_online;
	int usb_applied;
	bool usb_registered;
};

void goodix_m2468_init(struct goodix_ts_core *cd);
int goodix_m2468_usb_init(struct goodix_ts_core *cd);
void goodix_m2468_usb_exit(struct goodix_ts_core *cd);
void goodix_m2468_restore_usb_locked(struct goodix_ts_core *cd);
int goodix_m2468_set_gio4(struct goodix_ts_core *cd, bool enable);
int goodix_m2468_send_gesture(struct goodix_ts_core *cd, unsigned int type);
int goodix_m2468_sleep(struct goodix_ts_core *cd);

#endif /* _GOODIX_M2468_H_ */
