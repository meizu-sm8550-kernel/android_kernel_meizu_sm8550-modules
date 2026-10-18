/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _JIIOV_FINGERPRINT_H_
#define _JIIOV_FINGERPRINT_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define ANC_DEVICE_NAME                 "jiiov_fp"
#define ANC_OF_COMPATIBLE               "jiiov,fingerprint"
#define ANC_IOC_MAGIC                   'a'
#define ANC_PRODUCT_ID_LEN              16
#define ANC_NETLINK_PROTOCOL            30
#define ANC_NETLINK_PORT                100
#define ANC_WAKEUP_TIMEOUT_MS           125

typedef struct {
	char product_id[ANC_PRODUCT_ID_LEN];
} ANC_SENSOR_PRODUCT_INFO;

/* The gaps and direction/size bits are part of the original userspace ABI. */
#define ANC_IOC_RESET                   _IO(ANC_IOC_MAGIC, 0x00)
#define ANC_IOC_REQUEST_RESOURCE        _IO(ANC_IOC_MAGIC, 0x01)
#define ANC_IOC_RELEASE_RESOURCE        _IO(ANC_IOC_MAGIC, 0x02)
#define ANC_IOC_ENABLE_IRQ              _IO(ANC_IOC_MAGIC, 0x03)
#define ANC_IOC_DISABLE_IRQ             _IO(ANC_IOC_MAGIC, 0x04)
#define ANC_IOC_INIT_IRQ                _IO(ANC_IOC_MAGIC, 0x05)
#define ANC_IOC_DEINIT_IRQ              _IO(ANC_IOC_MAGIC, 0x06)
#define ANC_IOC_MASK_IRQ                _IO(ANC_IOC_MAGIC, 0x07)
#define ANC_IOC_UNMASK_IRQ              _IO(ANC_IOC_MAGIC, 0x08)
#define ANC_IOC_ENABLE_POWER            _IO(ANC_IOC_MAGIC, 0x0b)
#define ANC_IOC_DISABLE_POWER           _IO(ANC_IOC_MAGIC, 0x0c)
#define ANC_IOC_CLEAR_FLAG              _IO(ANC_IOC_MAGIC, 0x0e)
#define ANC_IOC_WAKE_LOCK               _IO(ANC_IOC_MAGIC, 0x0f)
#define ANC_IOC_WAKE_UNLOCK             _IO(ANC_IOC_MAGIC, 0x10)
#define ANC_IOC_SET_PRODUCT_ID          \
	_IOW(ANC_IOC_MAGIC, 0x12, ANC_SENSOR_PRODUCT_INFO)
#define ANC_IOC_GET_IRQ_PIN_LEVEL       _IOR(ANC_IOC_MAGIC, 0x13, __u8)

typedef enum {
	CUSTOM_COMMAND_INIT = 0,
	CUSTOM_COMMAND_CLEAR_TOUCH_STATE = 1,
	CUSTOM_COMMAND_SET_PRODUCT_ID = 2,
	CUSTOM_COMMAND_DEINIT = 3,
} CUSTOM_COMMAND;

enum anc_netlink_event {
	ANC_NETLINK_EVENT_TEST = 0,
	ANC_NETLINK_EVENT_IRQ = 1,
	ANC_NETLINK_EVENT_SCR_OFF = 2,
	ANC_NETLINK_EVENT_SCR_ON = 3,
	ANC_NETLINK_EVENT_TOUCH_DOWN = 4,
	ANC_NETLINK_EVENT_TOUCH_UP = 5,
	ANC_NETLINK_EVENT_UI_READY = 6,
	ANC_NETLINK_EVENT_EXIT = 7,
	ANC_NETLINK_EVENT_INVALID = 8,
	ANC_NETLINK_EVENT_MAX = 9,
};

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

struct pinctrl;
struct pinctrl_state;
struct regulator;
struct wakeup_source;

typedef struct platform_driver anc_driver_t;
typedef struct platform_device anc_device_t;

enum anc_pin_state {
	ANC_RESET_LOW,
	ANC_RESET_HIGH,
	ANC_IRQ_DEFAULT,
	ANC_PIN_STATE_COUNT,
};

struct vreg_config {
	const char *name;
	u32 vmin;
	u32 vmax;
	int ua_load;
};

struct anc_data {
	struct device *dev;
	struct class *class;
	dev_t devt;
	struct cdev cdev;
	ANC_SENSOR_PRODUCT_INFO product_info;
	struct pinctrl *fingerprint_pinctrl;
	struct pinctrl_state *pinctrl_states[ANC_PIN_STATE_COUNT];
	struct regulator *vreg[1];
	struct wakeup_source *fp_wakelock;
	struct work_struct work_queue;
	int irq_gpio;
	int irq;
	atomic_t irq_enabled;
	bool irq_mask_flag;
	int irq_init;
	int pwr_gpio;
	int rst_gpio;
	struct mutex lock;
	bool resource_requested;
	bool vdd_use_gpio;
	bool vdd_use_pmic;
	bool use_gpio_init;
	/* Ownership bookkeeping occupies padding in the original arm64 layout. */
	u8 requested_gpios;
};

int netlink_send_message_to_user(const char *p_buffer, size_t length);
int anc_netlink_init(void);
void anc_netlink_exit(void);
int custom_send_command(CUSTOM_COMMAND command, void *p_param);
#endif

#endif
