// SPDX-License-Identifier: GPL-2.0-only
/* Reconstructed from the m2468 jiiov_fingerprint.ko (Linux 5.15.119). */

#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "jiiov_fingerprint.h"

#define anc_debug(fmt, ...) \
	printk(KERN_DEBUG "[D][ANC_DRIVER][%s] " fmt, __func__, ##__VA_ARGS__)
#define anc_warn(fmt, ...) \
	printk(KERN_WARNING "[W][ANC_DRIVER][%s] " fmt, __func__, ##__VA_ARGS__)
#define anc_error(fmt, ...) \
	printk(KERN_ERR "[E][ANC_DRIVER][%s] " fmt, __func__, ##__VA_ARGS__)

#define ANC_GPIO_POWER  BIT(0)
#define ANC_GPIO_RESET  BIT(1)
#define ANC_GPIO_IRQ    BIT(2)

static anc_device_t *g_anc_spi_device;
static struct vreg_config vreg_conf;
static struct sock *gp_netlink_sock;
static DEFINE_MUTEX(anc_netlink_lock);

static const char * const anc_pin_names[ANC_PIN_STATE_COUNT] = {
	[ANC_RESET_LOW] = "anc_reset_low",
	[ANC_RESET_HIGH] = "anc_reset_high",
	[ANC_IRQ_DEFAULT] = "anc_irq_default",
};

int netlink_send_message_to_user(const char *p_buffer, size_t length)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	u16 payload_length = length;
	int ret;

	if (!p_buffer || !length)
		return -1;

	anc_error("send message to user: %d\n", (u8)p_buffer[0]);
	skb = alloc_skb(NLMSG_SPACE(payload_length), GFP_ATOMIC);
	if (!skb) {
		anc_error("netlink alloc failure\n");
		return -1;
	}

	nlh = nlmsg_put(skb, 0, 0, ANC_NETLINK_PROTOCOL, payload_length, 0);
	if (!nlh) {
		anc_error("nlmsg_put failaure\n");
		kfree_skb(skb);
		return -1;
	}
	memcpy(nlmsg_data(nlh), p_buffer, payload_length);

	mutex_lock(&anc_netlink_lock);
	if (gp_netlink_sock) {
		ret = netlink_unicast(gp_netlink_sock, skb, ANC_NETLINK_PORT,
				      MSG_DONTWAIT);
	} else {
		kfree_skb(skb);
		ret = -1;
	}
	mutex_unlock(&anc_netlink_lock);
	return ret;
}

static void netlink_receive_message(struct sk_buff *p_sk_buffer)
{
	const char *message;
	size_t length;

	if (p_sk_buffer->len < NLMSG_HDRLEN)
		return;

	message = nlmsg_data(nlmsg_hdr(p_sk_buffer));
	/* Preserve the echo, without reading beyond an unterminated skb. */
	length = strnlen(message, p_sk_buffer->len - NLMSG_HDRLEN);
	anc_error("received message:%.*s, length:%lu\n",
		  (int)length, message, (unsigned long)length);
	netlink_send_message_to_user(message, length);
}

static struct netlink_kernel_cfg g_netlink_kernel_config = {
	.input = netlink_receive_message,
};

int anc_netlink_init(void)
{
	gp_netlink_sock = netlink_kernel_create(&init_net, ANC_NETLINK_PROTOCOL,
					       &g_netlink_kernel_config);
	if (!gp_netlink_sock) {
		anc_error("netlink_kernel_create error !\n");
		return -1;
	}
	anc_debug("anc_netlink_init\n");
	return 0;
}

void anc_netlink_exit(void)
{
	struct sock *sock;

	mutex_lock(&anc_netlink_lock);
	sock = gp_netlink_sock;
	gp_netlink_sock = NULL;
	mutex_unlock(&anc_netlink_lock);
	if (sock)
		netlink_kernel_release(sock);
	anc_debug("anc_netlink_exit!\n");
}

/* This function is also a no-op in the supplied module. */
int custom_send_command(CUSTOM_COMMAND command, void *p_param)
{
	return 0;
}

static int anc_parse_dts(struct anc_data *p_data)
{
	struct device_node *np = p_data->dev->of_node;
	int ret;

	p_data->vdd_use_gpio = of_property_read_bool(np, "anc,vdd_use_gpio");
	p_data->vdd_use_pmic = of_property_read_bool(np, "anc,vdd_use_pmic");
	p_data->use_gpio_init = of_property_read_bool(np, "anc,use_gpio_init");
	anc_debug("vdd_use_gpio = %d, vdd_use_pmic = %d, use_gpio_init = %d\n",
		  p_data->vdd_use_gpio, p_data->vdd_use_pmic,
		  p_data->use_gpio_init);
	if (!p_data->vdd_use_pmic)
		return 0;

	vreg_conf.name = "vdd";
	ret = of_property_read_u32_index(np, "anc,vdd_config", 0,
					 &vreg_conf.vmin);
	if (ret) {
		anc_error("read anc,vdd_config index 0 failed\n");
		return ret;
	}
	ret = of_property_read_u32_index(np, "anc,vdd_config", 1,
					 &vreg_conf.vmax);
	if (ret) {
		anc_error("read anc,vdd_config index 1 failed\n");
		return ret;
	}
	ret = of_property_read_u32_index(np, "anc,vdd_config", 2,
					 (u32 *)&vreg_conf.ua_load);
	if (ret) {
		anc_error("read anc,vdd_config index 2 failed\n");
		return ret;
	}
	anc_debug("vreg config: name = %s, vmin = %d, vmax = %d, ua_load = %d\n",
		  vreg_conf.name, vreg_conf.vmin, vreg_conf.vmax,
		  vreg_conf.ua_load);
	return 0;
}

static int vreg_setup(struct anc_data *p_data, const char *name, bool enable)
{
	struct regulator **vreg = &p_data->vreg[0];
	int ret;

	if (!vreg_conf.name || strncmp(vreg_conf.name, name,
				      strlen(vreg_conf.name))) {
		anc_error("Regulator %s not found\n", name);
		return -EINVAL;
	}

	if (enable) {
		if (!*vreg) {
			*vreg = regulator_get(p_data->dev, name);
			if (IS_ERR(*vreg)) {
				ret = PTR_ERR(*vreg);
				*vreg = NULL;
				anc_error("Unable to get %s\n", name);
				return ret;
			}
		}
		if (regulator_count_voltages(*vreg) > 0) {
			ret = regulator_set_voltage(*vreg, vreg_conf.vmin,
						    vreg_conf.vmax);
			if (ret)
				anc_error("Unable to set voltage on %s, %d\n",
					  name, ret);
		}
		ret = regulator_set_load(*vreg, vreg_conf.ua_load);
		if (ret < 0)
			anc_error("Unable to set current on %s, %d\n", name, ret);
		ret = regulator_enable(*vreg);
		if (ret) {
			anc_error("error enabling %s: %d\n", name, ret);
			regulator_put(*vreg);
			*vreg = NULL;
		}
		return ret;
	}

	if (!*vreg)
		return 0;
	ret = regulator_is_enabled(*vreg);
	if (ret) {
		regulator_disable(*vreg);
		anc_debug("disabled %s\n", name);
	}
	regulator_put(*vreg);
	*vreg = NULL;
	return ret;
}

static void anc_power_onoff(struct anc_data *p_data, bool power_onoff)
{
	int ret;

	if (!p_data) {
		anc_error("p_data is NULL\n");
		return;
	}
	anc_debug("power_onoff = %d \n", power_onoff);
	if (!p_data->resource_requested) {
		anc_warn("resource is not requested, return!\n");
		return;
	}

	if (p_data->vdd_use_gpio)
		gpio_set_value(p_data->pwr_gpio, power_onoff);
	if (p_data->vdd_use_pmic) {
		ret = vreg_setup(p_data, "vdd", power_onoff);
		if (ret)
			anc_error("use pmic to contrl power failed, ret_val: %d\n",
				  ret);
	}
}

static int select_pin_ctl(struct anc_data *p_data, const char *name)
{
	int i, ret;

	if (!p_data || !name) {
		anc_error("parameter is NULL, p_data:%p, name:%p\n", p_data, name);
		return -EINVAL;
	}
	for (i = 0; i < ANC_PIN_STATE_COUNT; i++) {
		if (!strncmp(anc_pin_names[i], name, strlen(anc_pin_names[i])))
			break;
	}
	if (i == ANC_PIN_STATE_COUNT) {
		anc_error("%s not found\n", name);
		return -EINVAL;
	}
	if (IS_ERR_OR_NULL(p_data->fingerprint_pinctrl) ||
	    IS_ERR_OR_NULL(p_data->pinctrl_states[i]))
		return -EINVAL;

	ret = pinctrl_select_state(p_data->fingerprint_pinctrl,
				   p_data->pinctrl_states[i]);
	if (ret)
		anc_error("cannot select %s, ret_val: %d\n", name, ret);
	else
		anc_debug("Selected %s success\n", name);
	return ret;
}

static int anc_request_named_gpio(struct anc_data *p_data, const char *label,
				  int *gpio)
{
	int number, ret;

	if (!p_data || !label || !gpio) {
		anc_error("parameter is NULL, p_data:%p, label:%p, gpio:%p\n",
			  p_data, label, gpio);
		return -EINVAL;
	}
	number = of_get_named_gpio(p_data->dev->of_node, label, 0);
	if (number < 0) {
		anc_error("failed to get '%s'\n", label);
		return number;
	}
	*gpio = number;
	/* Pair requests with gpio_free; devm_gpio_free is outside the GKI ABI. */
	ret = gpio_request(number, label);
	if (ret) {
		anc_error("failed to request gpio %d\n", number);
		*gpio = -EINVAL;
		return ret;
	}
	if (gpio == &p_data->pwr_gpio)
		p_data->requested_gpios |= ANC_GPIO_POWER;
	else if (gpio == &p_data->rst_gpio)
		p_data->requested_gpios |= ANC_GPIO_RESET;
	else if (gpio == &p_data->irq_gpio)
		p_data->requested_gpios |= ANC_GPIO_IRQ;
	anc_debug("%s %d\n", label, number);
	return 0;
}

static int use_pinctrl_init(struct anc_data *p_data)
{
	struct pinctrl_state *state;
	int i, ret;

	p_data->fingerprint_pinctrl = devm_pinctrl_get(p_data->dev);
	if (IS_ERR(p_data->fingerprint_pinctrl)) {
		ret = PTR_ERR(p_data->fingerprint_pinctrl);
		p_data->fingerprint_pinctrl = NULL;
		if (ret == -EPROBE_DEFER) {
			anc_debug("pinctrl not ready\n");
			return ret;
		}
		anc_error("Target does not use pinctrl\n");
		return -EINVAL;
	}

	for (i = 0; i < ANC_PIN_STATE_COUNT; i++) {
		state = pinctrl_lookup_state(p_data->fingerprint_pinctrl,
					     anc_pin_names[i]);
		if (IS_ERR(state)) {
			anc_error("cannot find '%s'\n", anc_pin_names[i]);
			return -EINVAL;
		}
		p_data->pinctrl_states[i] = state;
		anc_debug("found pin control %s\n", anc_pin_names[i]);
	}
	ret = select_pin_ctl(p_data, anc_pin_names[ANC_IRQ_DEFAULT]);
	if (ret) {
		anc_debug("set default irq failed, ret_val:%d\n", ret);
		return ret;
	}
	ret = select_pin_ctl(p_data, anc_pin_names[ANC_RESET_LOW]);
	if (ret)
		anc_debug("set reset low failed, ret_val:%d\n", ret);
	return ret;
}

static int use_gpio_init(struct anc_data *p_data)
{
	int ret;

	ret = anc_request_named_gpio(p_data, "anc,gpio_rst", &p_data->rst_gpio);
	if (ret) {
		anc_debug("request reset gpio failed, ret_val:%d\n", ret);
		return ret;
	}
	ret = gpio_direction_output(p_data->rst_gpio, 0);
	if (ret) {
		anc_debug("reset gpio direction output failed, ret_val:%d\n", ret);
		return ret;
	}
	ret = anc_request_named_gpio(p_data, "anc,gpio_irq", &p_data->irq_gpio);
	if (ret) {
		anc_debug("request irq gpio failed, ret_val:%d\n", ret);
		return ret;
	}
	ret = gpio_direction_input(p_data->irq_gpio);
	if (ret)
		anc_debug("irq gpio direction input failed, ret_val:%d\n", ret);
	return ret;
}

static int anc_gpio_deinit(struct anc_data *p_data)
{
	if (!p_data) {
		anc_error("p_data is NULL\n");
		return -EINVAL;
	}
	anc_debug("deinit gpio\n");
	if (p_data->requested_gpios & ANC_GPIO_POWER)
		gpio_free(p_data->pwr_gpio);
	if (!IS_ERR_OR_NULL(p_data->fingerprint_pinctrl)) {
		anc_debug("devm_pinctrl_put\n");
		devm_pinctrl_put(p_data->fingerprint_pinctrl);
	}
	p_data->fingerprint_pinctrl = NULL;
	memset(p_data->pinctrl_states, 0, sizeof(p_data->pinctrl_states));
	if (p_data->requested_gpios & ANC_GPIO_RESET)
		gpio_free(p_data->rst_gpio);
	if (p_data->requested_gpios & ANC_GPIO_IRQ)
		gpio_free(p_data->irq_gpio);
	p_data->requested_gpios = 0;
	anc_debug("success\n");
	return 0;
}

static int anc_gpio_init(struct anc_data *p_data)
{
	int ret;

	anc_debug("init gpio\n");
	if (p_data->vdd_use_gpio) {
		ret = anc_request_named_gpio(p_data, "anc,gpio_pwr", &p_data->pwr_gpio);
		if (ret) {
			anc_error("request power gpio failed, ret_val:%d\n", ret);
			goto fail;
		}
		ret = gpio_direction_output(p_data->pwr_gpio, 0);
		if (ret) {
			anc_error("power gpio direction output failed, ret_val:%d\n", ret);
			goto fail;
		}
	}
	if (!p_data->use_gpio_init) {
		ret = use_pinctrl_init(p_data);
		if (ret) {
			anc_error("use pinctrl init failed, ret_val:%d\n", ret);
			goto fail;
		}
	}
	/* Both reset/IRQ GPIOs are requested even when reset uses pinctrl. */
	ret = use_gpio_init(p_data);
	if (ret) {
		anc_error("use gpio init failed, ret_val:%d\n", ret);
		goto fail;
	}
	if (of_property_read_bool(p_data->dev->of_node, "anc,enable-on-boot")) {
		anc_debug("Enabling hardware\n");
		anc_power_onoff(p_data, true);
	}
	anc_debug("success\n");
	return 0;
fail:
	anc_gpio_deinit(p_data);
	return ret;
}

static int anc_platform_init(struct anc_data *p_data)
{
	int ret;

	if (!p_data) {
		anc_error("p_data is NULL\n");
		return -EINVAL;
	}
	if (p_data->resource_requested) {
		anc_warn("resource is already requested, return!\n");
		return 0;
	}
	ret = anc_gpio_init(p_data);
	if (ret) {
		anc_error("Failed to init gpio, ret_val: %d\n", ret);
		return ret;
	}
	p_data->resource_requested = true;
	return 0;
}

static int anc_platform_free(struct anc_data *p_data)
{
	int ret;

	if (!p_data) {
		anc_error("p_data is NULL\n");
		return -EINVAL;
	}
	if (!p_data->resource_requested) {
		anc_warn("resource is already released, return!\n");
		return 0;
	}
	ret = anc_gpio_deinit(p_data);
	if (ret)
		anc_error("Failed to deinit gpio\n");
	else
		p_data->resource_requested = false;
	return ret;
}

static int set_reset_level(struct anc_data *p_data, bool level)
{
	int ret;

	if (p_data->use_gpio_init) {
		gpio_set_value(p_data->rst_gpio, level);
		return 0;
	}
	ret = select_pin_ctl(p_data, anc_pin_names[level ? ANC_RESET_HIGH : ANC_RESET_LOW]);
	if (ret)
		anc_error("set reset level:%d failed, ret_val: %d\n", level, ret);
	return ret;
}

static int anc_reset(struct anc_data *p_data)
{
	int ret;

	if (!p_data) {
		anc_error("p_data is NULL\n");
		return -EINVAL;
	}
	anc_debug("hardware reset\n");
	mutex_lock(&p_data->lock);
	ret = set_reset_level(p_data, false);
	if (ret)
		goto out;
	mdelay(10);
	ret = set_reset_level(p_data, true);
	if (ret)
		goto out;
	mdelay(10);
out:
	mutex_unlock(&p_data->lock);
	return ret;
}

static void anc_enable_irq(struct anc_data *p_data)
{
	if (!p_data) {
		anc_error("p_data is NULL\n");
		return;
	}
	anc_debug("enable irq\n");
	if (!p_data->resource_requested) {
		anc_warn("resource is not requested, return!\n");
		return;
	}
	if (atomic_read_acquire(&p_data->irq_enabled)) {
		anc_warn("IRQ has been enabled\n");
		return;
	}
	enable_irq(p_data->irq);
	atomic_set(&p_data->irq_enabled, 1);
}

static void anc_disable_irq(struct anc_data *p_data)
{
	if (!p_data) {
		anc_error("p_data is NULL\n");
		return;
	}
	anc_debug("disable irq\n");
	if (!p_data->resource_requested) {
		anc_warn("resource is not requested, return!\n");
		return;
	}
	if (!atomic_read_acquire(&p_data->irq_enabled)) {
		anc_warn("IRQ has been disabled\n");
		return;
	}
	disable_irq(p_data->irq);
	atomic_set(&p_data->irq_enabled, 0);
}

static void anc_do_irq_work(struct work_struct *work)
{
	char event = ANC_NETLINK_EVENT_IRQ;

	netlink_send_message_to_user(&event, sizeof(event));
}

static irqreturn_t anc_irq_handler(int irq, void *handle)
{
	struct anc_data *p_data = handle;

	anc_debug("irq handler, %d\n", p_data->irq_mask_flag);
	if (!p_data->irq_mask_flag) {
		__pm_wakeup_event(p_data->fp_wakelock, ANC_WAKEUP_TIMEOUT_MS);
		schedule_work(&p_data->work_queue);
	}
	return IRQ_HANDLED;
}

static int init_irq(struct anc_data *p_data)
{
	int ret = 0;

	if (!p_data) {
		anc_error("p_data is NULL\n");
		return -EINVAL;
	}
	anc_debug("init irq\n");
	p_data->irq = gpio_to_irq(p_data->irq_gpio);
	if (!p_data->irq_init) {
		ret = devm_request_threaded_irq(p_data->dev, p_data->irq, NULL,
						anc_irq_handler,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						dev_name(p_data->dev), p_data);
		if (ret) {
			anc_error("Could not request irq %d, ret_val: %d\n", p_data->irq, ret);
		} else {
			p_data->irq_init = 1;
			enable_irq_wake(p_data->irq);
			atomic_set(&p_data->irq_enabled, 1);
		}
	}
	/* Work is initialized at probe so an immediate IRQ cannot race INIT_WORK. */
	return ret;
}

static void deinit_irq(struct anc_data *p_data)
{
	if (!p_data) {
		anc_error("p_data is NULL\n");
		return;
	}
	anc_debug("deinit irq\n");
	if (p_data->irq_init == 1) {
		disable_irq_wake(p_data->irq);
		devm_free_irq(p_data->dev, p_data->irq, p_data);
		p_data->irq_init = 0;
	}
	cancel_work_sync(&p_data->work_queue);
}

static void set_irq_flag(struct anc_data *p_data, bool mask)
{
	anc_debug("set irq flag mask : %d\n", mask);
	p_data->irq_mask_flag = mask;
}

static void anc_wake_lock(struct anc_data *p_data)
{
	if (!p_data) {
		anc_error("p_data is NULL\n");
		return;
	}
	anc_debug("wake lock\n");
	__pm_stay_awake(p_data->fp_wakelock);
}

static void anc_wake_unlock(struct anc_data *p_data)
{
	if (!p_data) {
		anc_error("p_data is NULL\n");
		return;
	}
	anc_debug("wake unlock\n");
	__pm_relax(p_data->fp_wakelock);
	__pm_wakeup_event(p_data->fp_wakelock, ANC_WAKEUP_TIMEOUT_MS);
}

static int set_product_id(struct anc_data *p_data, const void __user *product)
{
	if (!access_ok(product, sizeof(p_data->product_info)) ||
	    copy_from_user(&p_data->product_info, product, sizeof(p_data->product_info))) {
		anc_error("Failed to copy product info from user to kernel\n");
		return -EFAULT;
	}
	anc_debug("set product id: %.*s\n", ANC_PRODUCT_ID_LEN, p_data->product_info.product_id);
	custom_send_command(CUSTOM_COMMAND_SET_PRODUCT_ID, &p_data->product_info);
	return 0;
}

static int get_irq_pin_level(int irq_gpio, u8 __user *pin_level)
{
	int level, ret;
	u8 value;

	level = gpio_get_value(irq_gpio);
	if (level < 0) {
		anc_error("read irq pin level fail, level:%d\n", level);
		return -EINVAL;
	}
	anc_debug("read irq pin level : %d\n", level);
	value = level;
	/* The supplied binary returns the uncopied byte count, including +1. */
	ret = copy_to_user(pin_level, &value, sizeof(value));
	if (ret)
		anc_error("Failed to copy irq pin level from kernel to user\n");
	else
		anc_debug("get irq pin level:%d\n", value);
	return ret;
}

static long anc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct anc_data *p_data = file->private_data;

	if (!p_data) {
		anc_error("get anc data failed\n");
		return -EINVAL;
	}
	if (_IOC_TYPE(cmd) != ANC_IOC_MAGIC) {
		anc_error("magic number does not match\n");
		return -ENOTTY;
	}
	anc_debug("cmd = %d\n", _IOC_NR(cmd));
	switch (cmd) {
	case ANC_IOC_RESET:
		return anc_reset(p_data);
	case ANC_IOC_REQUEST_RESOURCE:
		return anc_platform_init(p_data);
	case ANC_IOC_RELEASE_RESOURCE:
		return anc_platform_free(p_data);
	case ANC_IOC_ENABLE_IRQ:
		anc_enable_irq(p_data);
		return 0;
	case ANC_IOC_DISABLE_IRQ:
		anc_disable_irq(p_data);
		return 0;
	case ANC_IOC_INIT_IRQ:
		return init_irq(p_data);
	case ANC_IOC_DEINIT_IRQ:
		deinit_irq(p_data);
		return 0;
	case ANC_IOC_MASK_IRQ:
		set_irq_flag(p_data, true);
		return 0;
	case ANC_IOC_UNMASK_IRQ:
		set_irq_flag(p_data, false);
		return 0;
	case ANC_IOC_ENABLE_POWER:
		anc_power_onoff(p_data, true);
		return 0;
	case ANC_IOC_DISABLE_POWER:
		anc_power_onoff(p_data, false);
		return 0;
	case ANC_IOC_CLEAR_FLAG:
		return 0;
	case ANC_IOC_WAKE_LOCK:
		anc_wake_lock(p_data);
		return 0;
	case ANC_IOC_WAKE_UNLOCK:
		anc_wake_unlock(p_data);
		return 0;
	case ANC_IOC_SET_PRODUCT_ID:
		return set_product_id(p_data, (const void __user *)arg);
	case ANC_IOC_GET_IRQ_PIN_LEVEL:
		return get_irq_pin_level(p_data->irq_gpio, (u8 __user *)arg);
	default:
		return -EINVAL;
	}
}

#ifdef CONFIG_COMPAT
static long anc_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return anc_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static int anc_open(struct inode *inode, struct file *file)
{
	struct anc_data *p_data = container_of(inode->i_cdev, struct anc_data, cdev);

	if (!p_data) {
		anc_error("get anc date failed\n");
		return -EINVAL;
	}
	file->private_data = p_data;
	return 0;
}

static const struct file_operations anc_fops = {
	.owner = THIS_MODULE,
	.open = anc_open,
	.unlocked_ioctl = anc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = anc_compat_ioctl,
#endif
};

static ssize_t resource_control_set(struct device *dev, struct device_attribute *attr,
				    const char *buffer, size_t count)
{
	struct anc_data *p_data = dev_get_drvdata(dev);
	ssize_t ret = count;

	if (!p_data) {
		anc_error("dev get data failed\n");
		return -EINVAL;
	}
	mutex_lock(&p_data->lock);
	if (!strncmp(buffer, "request", 7)) {
		anc_debug("request resource\n");
		anc_platform_init(p_data);
	} else if (!strncmp(buffer, "release", 7)) {
		anc_debug("release resource\n");
		anc_platform_free(p_data);
	} else {
		anc_error("the resource name is not matched, name: %s\n", buffer);
		ret = -EINVAL;
	}
	mutex_unlock(&p_data->lock);
	return ret;
}

static ssize_t forward_netlink_event_set(struct device *dev, struct device_attribute *attr,
					 const char *buffer, size_t count)
{
	static const char * const events[] = {
		"test", "irq", "screen_off", "screen_on",
		"touch_down", "touch_up", "ui_ready", "exit",
	};
	char event;
	unsigned int i;

	anc_debug("forward netlink event: %s\n", buffer);
	for (i = 0; i < ARRAY_SIZE(events); i++) {
		if (!strncmp(buffer, events[i], strlen(events[i]))) {
			event = i;
			/* Original sysfs ABI returns netlink_unicast's result. */
			return netlink_send_message_to_user(&event, sizeof(event));
		}
	}
	anc_error("don't support the netlink evnet: %s\n", buffer);
	return -EINVAL;
}

static ssize_t irq_control_set(struct device *dev, struct device_attribute *attr,
			       const char *buffer, size_t count)
{
	struct anc_data *p_data = dev_get_drvdata(dev);
	ssize_t ret = count;

	if (!p_data)
		return -EINVAL;
	mutex_lock(&p_data->lock);
	if (!strncmp(buffer, "enable", 6))
		anc_enable_irq(p_data);
	else if (!strncmp(buffer, "disable", 7))
		anc_disable_irq(p_data);
	else {
		anc_error("the irq name is not matched, name: %s\n", buffer);
		ret = -EINVAL;
	}
	mutex_unlock(&p_data->lock);
	return ret;
}

static ssize_t hw_reset_set(struct device *dev, struct device_attribute *attr,
			    const char *buffer, size_t count)
{
	struct anc_data *p_data = dev_get_drvdata(dev);
	int ret;

	if (!p_data) {
		anc_error("dev get data failed\n");
		return -EINVAL;
	}
	if (strncmp(buffer, "reset", 5)) {
		anc_error("the reset name is not matched, name: %s\n", buffer);
		return -EINVAL;
	}
	ret = anc_reset(p_data);
	if (ret) {
		anc_error("hardware reset failed, ret_val: %d\n", ret);
		return ret;
	}
	return count;
}

static ssize_t device_power_set(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct anc_data *p_data = dev_get_drvdata(dev);
	ssize_t ret = count;

	if (!p_data) {
		anc_error("dev get data failed\n");
		return -EINVAL;
	}
	mutex_lock(&p_data->lock);
	if (!strncmp(buffer, "on", 2)) {
		anc_debug("device power on\n");
		anc_power_onoff(p_data, true);
	} else if (!strncmp(buffer, "off", 3)) {
		anc_debug("device power off\n");
		anc_power_onoff(p_data, false);
	} else {
		anc_error("the power name is not matched, name: %s\n", buffer);
		ret = -EINVAL;
	}
	mutex_unlock(&p_data->lock);
	return ret;
}

static ssize_t pinctl_set(struct device *dev, struct device_attribute *attr,
			  const char *buffer, size_t count)
{
	struct anc_data *p_data = dev_get_drvdata(dev);
	int ret;

	if (!p_data) {
		anc_error("dev get data failed\n");
		return -EINVAL;
	}
	mutex_lock(&p_data->lock);
	anc_debug("pinctl select %s\n", buffer);
	ret = select_pin_ctl(p_data, buffer);
	mutex_unlock(&p_data->lock);
	if (ret)
		return ret;
	return count;
}

static DEVICE_ATTR(pinctl_set, 0200, NULL, pinctl_set);
static DEVICE_ATTR(device_power, 0200, NULL, device_power_set);
static DEVICE_ATTR(hw_reset, 0200, NULL, hw_reset_set);
static DEVICE_ATTR(irq_set, 0200, NULL, irq_control_set);
static DEVICE_ATTR(netlink_event, 0200, NULL, forward_netlink_event_set);
static DEVICE_ATTR(resource_set, 0200, NULL, resource_control_set);

static struct attribute *attributes[] = {
	&dev_attr_pinctl_set.attr,
	&dev_attr_device_power.attr,
	&dev_attr_hw_reset.attr,
	&dev_attr_irq_set.attr,
	&dev_attr_netlink_event.attr,
	&dev_attr_resource_set.attr,
	NULL,
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static int anc_create_device(struct anc_data *p_data)
{
	struct device *dev;
	int ret;

	p_data->class = class_create(THIS_MODULE, ANC_DEVICE_NAME);
	if (IS_ERR(p_data->class)) {
		anc_error("class_create failed\n");
		return -ENODEV;
	}
	ret = alloc_chrdev_region(&p_data->devt, 0, 1, ANC_DEVICE_NAME);
	if (ret < 0) {
		anc_error("Failed to alloc char device region, ret_val: %d\n", ret);
		goto destroy_class;
	}
	anc_debug("Major number of device = %d\n", MAJOR(p_data->devt));
	dev = device_create(p_data->class, NULL, p_data->devt, p_data, ANC_DEVICE_NAME);
	if (IS_ERR(dev)) {
		anc_error("Failed to create char device\n");
		ret = -ENODEV;
		goto unregister_region;
	}
	cdev_init(&p_data->cdev, &anc_fops);
	p_data->cdev.owner = THIS_MODULE;
	ret = cdev_add(&p_data->cdev, p_data->devt, 1);
	if (ret < 0) {
		anc_error("Failed to add char device\n");
		goto destroy_device;
	}
	return 0;
destroy_device:
	device_destroy(p_data->class, p_data->devt);
unregister_region:
	unregister_chrdev_region(p_data->devt, 1);
destroy_class:
	class_destroy(p_data->class);
	return ret;
}

static void anc_destroy_device(struct anc_data *p_data)
{
	cdev_del(&p_data->cdev);
	device_destroy(p_data->class, p_data->devt);
	unregister_chrdev_region(p_data->devt, 1);
	class_destroy(p_data->class);
}

static int anc_probe(anc_device_t *pdev)
{
	struct anc_data *p_data;
	int ret;

	anc_debug("entry\n");
	p_data = devm_kzalloc(&pdev->dev, sizeof(*p_data), GFP_KERNEL);
	if (!p_data) {
		anc_error("Failed to allocate memory for device data\n");
		return -ENOMEM;
	}
	p_data->dev = &pdev->dev;
	p_data->pwr_gpio = -EINVAL;
	p_data->rst_gpio = -EINVAL;
	p_data->irq_gpio = -EINVAL;
	p_data->irq = -EINVAL;
	p_data->resource_requested = false;
	p_data->irq_mask_flag = false;
	p_data->irq_init = 0;
	atomic_set(&p_data->irq_enabled, 0);
	mutex_init(&p_data->lock);
	INIT_WORK(&p_data->work_queue, anc_do_irq_work);
	platform_set_drvdata(pdev, p_data);
	g_anc_spi_device = pdev;

	ret = anc_parse_dts(p_data);
	if (ret) {
		anc_error("parse dts failed\n");
		goto free_data;
	}
	ret = anc_create_device(p_data);
	if (ret)
		goto free_data;
	p_data->fp_wakelock = wakeup_source_register(&pdev->dev, "anc_fp_wakelock");
	if (!p_data->fp_wakelock) {
		ret = -ENOMEM;
		goto destroy_device;
	}
	anc_debug("Create sysfs path = %s\n", kobject_name(&pdev->dev.kobj));
	ret = sysfs_create_group(&pdev->dev.kobj, &attribute_group);
	if (ret) {
		anc_error("Could not create sysfs\n");
		goto unregister_wakeup;
	}
	/* Hardware resources are acquired by ioctl/resource_set, not by probe. */
	anc_debug("Success\n");
	return 0;
unregister_wakeup:
	wakeup_source_unregister(p_data->fp_wakelock);
destroy_device:
	anc_destroy_device(p_data);
free_data:
	platform_set_drvdata(pdev, NULL);
	if (g_anc_spi_device == pdev)
		g_anc_spi_device = NULL;
	devm_kfree(&pdev->dev, p_data);
	anc_error("Probe Failed, ret_val = %d\n", ret);
	return ret;
}

static int anc_remove(anc_device_t *pdev)
{
	struct anc_data *p_data = platform_get_drvdata(pdev);

	if (!p_data) {
		anc_error("get data handle failed\n");
		return -EINVAL;
	}
	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
	deinit_irq(p_data);
	anc_power_onoff(p_data, false);
	if (p_data->resource_requested)
		anc_platform_free(p_data);
	wakeup_source_unregister(p_data->fp_wakelock);
	anc_destroy_device(p_data);
	platform_set_drvdata(pdev, NULL);
	if (g_anc_spi_device == pdev)
		g_anc_spi_device = NULL;
	devm_kfree(&pdev->dev, p_data);
	return 0;
}

static void anc_shutdown(anc_device_t *pdev)
{
	struct anc_data *p_data = platform_get_drvdata(pdev);

	anc_debug("entry\n");
	if (!p_data) {
		anc_error("get data handle failed\n");
		return;
	}
	anc_power_onoff(p_data, false);
}

static const struct of_device_id anc_of_match[] = {
	{ .compatible = ANC_OF_COMPATIBLE },
	{ }
};
MODULE_DEVICE_TABLE(of, anc_of_match);

static anc_driver_t anc_driver = {
	.probe = anc_probe,
	.remove = anc_remove,
	.shutdown = anc_shutdown,
	.driver = {
		.name = ANC_DEVICE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = anc_of_match,
	},
};

static int __init ancfp_init(void)
{
	int ret;

	anc_debug("entry\n");
	ret = platform_driver_register(&anc_driver);
	if (ret) {
		anc_error("register spi driver failed, ret_val:%d\n", ret);
		return ret;
	}
	anc_debug("success\n");
	/* The original init return value ignores failure to create Netlink. */
	anc_netlink_init();
	return 0;
}

static void __exit ancfp_exit(void)
{
	anc_debug("entry\n");
	anc_netlink_exit();
	platform_driver_unregister(&anc_driver);
}

module_init(ancfp_init);
module_exit(ancfp_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("JIIOV");
MODULE_DESCRIPTION("JIIOV fingerprint sensor device driver");
