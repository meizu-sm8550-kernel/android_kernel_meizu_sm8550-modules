/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NOTE_JIIOV_H
#define _NOTE_JIIOV_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct pinctrl;
struct pinctrl_state;
struct regulator;
struct wakeup_source;

/* All state except dead/irq_mask is protected by lock or probe/remove lifetime. */
struct jiiov_data {
	struct device chardev;
	struct cdev cdev;
	struct device *dev;
	struct mutex lock;
	struct work_struct irq_work;
	struct wakeup_source *wakeup;
	struct pinctrl *pinctrl;
	struct pinctrl_state *reset_low;
	struct pinctrl_state *reset_high;
	struct pinctrl_state *irq_default;
	struct regulator *vdd;
	u32 vdd_config[3];
	u8 product_info[16];
	int rst_gpio;
	int irq_gpio;
	int irq;
	bool dead;
	bool resources;
	bool reset_owned;
	bool irq_gpio_owned;
	bool powered;
	bool load_configured;
	bool irq_requested;
	bool irq_module_ref;
	bool irq_enabled;
	bool irq_wake;
	bool irq_mask;
};

int jiiov_netlink_init(void);
void jiiov_netlink_exit(void);
int jiiov_netlink_send(u8 event);

#endif
