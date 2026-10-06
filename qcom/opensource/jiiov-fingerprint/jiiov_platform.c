// SPDX-License-Identifier: GPL-2.0-only
/* M2468 JIIOV platform ABI, reconstructed from the supplied stock module. */
#include <linux/compat.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "jiiov.h"

static dev_t jiiov_devt;
static struct class *jiiov_class;
static atomic_t jiiov_claimed = ATOMIC_INIT(0);

/* Resource, power and IRQ helpers require data->lock. */
static void jiiov_resources_drop(struct jiiov_data *data)
{
	if (data->irq_gpio_owned) {
		gpio_free(data->irq_gpio);
		data->irq_gpio_owned = false;
	}
	if (data->reset_owned) {
		gpio_free(data->rst_gpio);
		data->reset_owned = false;
	}
	if (data->pinctrl) {
		pinctrl_put(data->pinctrl);
		data->pinctrl = NULL;
	}
	data->reset_low = NULL;
	data->reset_high = NULL;
	data->irq_default = NULL;
	data->resources = false;
}

static int jiiov_resources_request(struct jiiov_data *data)
{
	int ret;

	if (data->resources)
		return 0;
	data->pinctrl = pinctrl_get(data->dev);
	if (IS_ERR(data->pinctrl)) {
		ret = PTR_ERR(data->pinctrl);
		data->pinctrl = NULL;
		return ret;
	}
	data->reset_low = pinctrl_lookup_state(data->pinctrl, "anc_reset_low");
	if (IS_ERR(data->reset_low)) {
		ret = PTR_ERR(data->reset_low);
		goto fail;
	}
	data->reset_high = pinctrl_lookup_state(data->pinctrl, "anc_reset_high");
	if (IS_ERR(data->reset_high)) {
		ret = PTR_ERR(data->reset_high);
		goto fail;
	}
	data->irq_default = pinctrl_lookup_state(data->pinctrl, "anc_irq_default");
	if (IS_ERR(data->irq_default)) {
		ret = PTR_ERR(data->irq_default);
		goto fail;
	}
	ret = pinctrl_select_state(data->pinctrl, data->irq_default);
	if (ret)
		goto fail;
	ret = pinctrl_select_state(data->pinctrl, data->reset_low);
	if (ret)
		goto fail;
	ret = gpio_request(data->rst_gpio, "jiiov_reset");
	if (ret)
		goto fail;
	data->reset_owned = true;
	ret = gpio_direction_output(data->rst_gpio, 0);
	if (ret)
		goto fail;
	ret = gpio_request(data->irq_gpio, "jiiov_irq");
	if (ret)
		goto fail;
	data->irq_gpio_owned = true;
	ret = gpio_direction_input(data->irq_gpio);
	if (ret)
		goto fail;
	data->resources = true;
	return 0;
fail:
	jiiov_resources_drop(data);
	return ret;
}

static int jiiov_power(struct jiiov_data *data, bool on)
{
	struct regulator *vdd;
	int ret, count;

	if (!on) {
		if (!data->vdd)
			return 0;
		/* Avoid 5.15's late DRMS error after consuming enable_count. */
		ret = regulator_set_load(data->vdd, 0);
		if (ret < 0)
			return ret;
		data->load_configured = false;
		ret = regulator_disable(data->vdd);
		if (ret)
			return ret;
		data->powered = false;
		regulator_put(data->vdd);
		data->vdd = NULL;
		module_put(THIS_MODULE);
		return 0;
	}
	if (!data->resources)
		return -ENODEV;
	if (data->powered) {
		/* An earlier failed disable may have removed this consumer's load. */
		if (!data->load_configured) {
			ret = regulator_set_load(data->vdd, data->vdd_config[2]);
			if (ret < 0)
				return ret;
			data->load_configured = true;
		}
		return 0;
	}
	/* Sysfs callers, unlike open files, do not supply a module reference. */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	vdd = regulator_get(data->dev, "vdd");
	if (IS_ERR(vdd)) {
		ret = PTR_ERR(vdd);
		goto put_module;
	}
	count = regulator_count_voltages(vdd);
	if (count < 0) {
		ret = count;
		goto put;
	}
	if (count) {
		ret = regulator_set_voltage(vdd, data->vdd_config[0], data->vdd_config[1]);
		if (ret)
			goto put;
	}
	ret = regulator_set_load(vdd, data->vdd_config[2]);
	if (ret < 0)
		goto put;
	ret = regulator_enable(vdd);
	if (ret) {
		regulator_set_load(vdd, 0);
		goto put;
	}
	data->vdd = vdd;
	data->powered = true;
	data->load_configured = true;
	return 0;
put:
	regulator_put(vdd);
put_module:
	module_put(THIS_MODULE);
	return ret;
}

static int jiiov_reset(struct jiiov_data *data)
{
	int ret;

	if (!data->resources)
		return -ENODEV;
	ret = pinctrl_select_state(data->pinctrl, data->reset_low);
	if (ret)
		return ret;
	usleep_range(10000, 10100);
	ret = pinctrl_select_state(data->pinctrl, data->reset_high);
	if (ret)
		return ret;
	usleep_range(10000, 10100);
	return 0;
}

static void jiiov_irq_work(struct work_struct *work)
{
	struct jiiov_data *data = container_of(work, struct jiiov_data, irq_work);

	/* Mask only filters new IRQs; an already queued event remains valid. */
	if (!READ_ONCE(data->dead))
		jiiov_netlink_send(1);
}

static irqreturn_t jiiov_irq_thread(int irq, void *context)
{
	struct jiiov_data *data = context;

	/* Never take data->lock: free_irq/disable_irq may wait while holding it. */
	if (!READ_ONCE(data->dead) && !READ_ONCE(data->irq_mask)) {
		pm_wakeup_ws_event(data->wakeup, 125, false);
		queue_work(system_wq, &data->irq_work);
	}
	return IRQ_HANDLED;
}

/* Caller has disabled IRQ wake, or is irreversibly removing a dead device. */
static void jiiov_irq_dispose(struct jiiov_data *data)
{
	if (data->irq_requested)
		free_irq(data->irq, data);
	data->irq_requested = false;
	data->irq_enabled = false;
	cancel_work_sync(&data->irq_work);
	/* A failed wake disable still owns its IRQ identity and module reference. */
	if (!data->irq_wake) {
		data->irq = -1;
		if (data->irq_module_ref) {
			data->irq_module_ref = false;
			module_put(THIS_MODULE);
		}
	}
}

static int jiiov_irq_free(struct jiiov_data *data)
{
	int ret;

	if (data->irq_wake) {
		ret = disable_irq_wake(data->irq);
		/* Linux restores wake_depth on failure; retain ownership for retry. */
		if (ret)
			return ret;
		data->irq_wake = false;
	}
	jiiov_irq_dispose(data);
	return 0;
}

static int jiiov_irq_request(struct jiiov_data *data)
{
	int ret;

	if (!data->resources)
		return -ENODEV;
	if (data->irq_requested)
		return 0;
	if (data->irq_wake)
		return -EBUSY;
	ret = gpio_to_irq(data->irq_gpio);
	if (ret < 0)
		return ret;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	data->irq_module_ref = true;
	data->irq = ret;
	ret = request_threaded_irq(data->irq, NULL, jiiov_irq_thread,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   dev_name(data->dev), data);
	if (ret) {
		data->irq = -1;
		data->irq_module_ref = false;
		module_put(THIS_MODULE);
		return ret;
	}
	data->irq_requested = true;
	data->irq_enabled = true;
	ret = enable_irq_wake(data->irq);
	if (ret) {
		jiiov_irq_free(data);
		return ret;
	}
	data->irq_wake = true;
	return 0;
}

static int jiiov_irq_enable(struct jiiov_data *data, bool enable)
{
	if (!data->irq_requested)
		return enable ? -ENODEV : 0;
	if (enable == data->irq_enabled)
		return 0;
	if (enable)
		enable_irq(data->irq);
	else
		disable_irq(data->irq);
	data->irq_enabled = enable;
	return 0;
}

static int jiiov_resources_release(struct jiiov_data *data)
{
	int ret, power_ret;

	ret = jiiov_irq_free(data);
	if (ret)
		return ret;
	power_ret = jiiov_power(data, false);
	/* Keep GPIO/pinctrl and the regulator handle available for an off retry. */
	if (power_ret)
		return power_ret;
	jiiov_resources_drop(data);
	return ret;
}

static void jiiov_wake_unlock(struct jiiov_data *data)
{
	__pm_relax(data->wakeup);
	pm_wakeup_ws_event(data->wakeup, 125, false);
}

static long jiiov_command(struct jiiov_data *data, unsigned int cmd, unsigned long arg)
{
	u8 product[16], level;
	int ret;

	if ((cmd & 0xff00) != 0x6100)
		return -ENOTTY;
	/* Compare the complete command: direction/size bits are part of the ABI. */
	switch (cmd) {
	case 0x6100:
		return jiiov_reset(data);
	case 0x6101:
		return jiiov_resources_request(data);
	case 0x6102:
		return jiiov_resources_release(data);
	case 0x6103:
		return jiiov_irq_enable(data, true);
	case 0x6104:
		return jiiov_irq_enable(data, false);
	case 0x6105:
		return jiiov_irq_request(data);
	case 0x6106:
		return jiiov_irq_free(data);
	case 0x6107:
		WRITE_ONCE(data->irq_mask, true);
		return 0;
	case 0x6108:
		WRITE_ONCE(data->irq_mask, false);
		return 0;
	case 0x610b:
		return jiiov_power(data, true);
	case 0x610c:
		return jiiov_power(data, false);
	case 0x610e:
		/* Verified stock no-op; this does not produce touch/UI events. */
		return 0;
	case 0x610f:
		__pm_stay_awake(data->wakeup);
		return 0;
	case 0x6110:
		jiiov_wake_unlock(data);
		return 0;
	case 0x40106112:
		if (copy_from_user(product, (void __user *)arg, sizeof(product)))
			return -EFAULT;
		memcpy(data->product_info, product, sizeof(product));
		return 0;
	case 0x80016113:
		if (!data->resources)
			return -ENODEV;
		ret = gpio_get_value_cansleep(data->irq_gpio);
		if (ret < 0)
			return -EINVAL;
		level = ret;
		return copy_to_user((void __user *)arg, &level, sizeof(level)) ? -EFAULT : 0;
	default:
		return -EINVAL;
	}
}

static long jiiov_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct jiiov_data *data = file->private_data;
	long ret;

	if (!data)
		return -ENODEV;
	mutex_lock(&data->lock);
	ret = data->dead ? -ENODEV : jiiov_command(data, cmd, arg);
	mutex_unlock(&data->lock);
	return ret;
}

#ifdef CONFIG_COMPAT
static long jiiov_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return jiiov_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static int jiiov_open(struct inode *inode, struct file *file)
{
	struct jiiov_data *data = container_of(inode->i_cdev, struct jiiov_data, cdev);
	int ret = 0;

	/* cdev_device_add pins chardev during open; fd holds an additional ref. */
	get_device(&data->chardev);
	mutex_lock(&data->lock);
	if (data->dead)
		ret = -ENODEV;
	else
		file->private_data = data;
	mutex_unlock(&data->lock);
	if (ret)
		put_device(&data->chardev);
	return ret;
}

static int jiiov_release(struct inode *inode, struct file *file)
{
	struct jiiov_data *data = file->private_data;

	file->private_data = NULL;
	put_device(&data->chardev);
	return 0;
}

static const struct file_operations jiiov_fops = {
	.owner = THIS_MODULE,
	.open = jiiov_open,
	.release = jiiov_release,
	.unlocked_ioctl = jiiov_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = jiiov_compat_ioctl,
#endif
};

static bool jiiov_prefix(const char *buf, size_t count, const char *command, size_t len)
{
	return count >= len && !strncmp(buf, command, len);
}

static ssize_t jiiov_sysfs_command(struct device *dev, unsigned int cmd, size_t count)
{
	struct jiiov_data *data = dev_get_drvdata(dev);
	long ret;

	if (!data)
		return -ENODEV;
	mutex_lock(&data->lock);
	ret = data->dead ? -ENODEV : jiiov_command(data, cmd, 0);
	mutex_unlock(&data->lock);
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t resource_set_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	if (jiiov_prefix(buf, count, "request", 7))
		return jiiov_sysfs_command(dev, 0x6101, count);
	if (jiiov_prefix(buf, count, "release", 7))
		return jiiov_sysfs_command(dev, 0x6102, count);
	return -EINVAL;
}

static ssize_t device_power_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	if (jiiov_prefix(buf, count, "on", 2))
		return jiiov_sysfs_command(dev, 0x610b, count);
	if (jiiov_prefix(buf, count, "off", 3))
		return jiiov_sysfs_command(dev, 0x610c, count);
	return -EINVAL;
}

static ssize_t irq_set_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	if (jiiov_prefix(buf, count, "enable", 6))
		return jiiov_sysfs_command(dev, 0x6103, count);
	if (jiiov_prefix(buf, count, "disable", 7))
		return jiiov_sysfs_command(dev, 0x6104, count);
	return -EINVAL;
}

static ssize_t hw_reset_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	if (jiiov_prefix(buf, count, "reset", 5))
		return jiiov_sysfs_command(dev, 0x6100, count);
	return -EINVAL;
}

static ssize_t pinctl_set_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct jiiov_data *data = dev_get_drvdata(dev);
	struct pinctrl_state *state;
	int ret;

	if (!data)
		return -ENODEV;
	mutex_lock(&data->lock);
	if (data->dead || !data->resources) {
		ret = -ENODEV;
		goto unlock;
	}
	if (jiiov_prefix(buf, count, "anc_reset_low", 13))
		state = data->reset_low;
	else if (jiiov_prefix(buf, count, "anc_reset_high", 14))
		state = data->reset_high;
	else if (jiiov_prefix(buf, count, "anc_irq_default", 15))
		state = data->irq_default;
	else {
		ret = -EINVAL;
		goto unlock;
	}
	ret = pinctrl_select_state(data->pinctrl, state);
unlock:
	mutex_unlock(&data->lock);
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t netlink_event_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	static const char * const names[] = {
		"test", "irq", "screen_off", "screen_on",
		"touch_down", "touch_up", "ui_ready", "exit",
	};
	struct jiiov_data *data = dev_get_drvdata(dev);
	unsigned int event;
	int ret = -EINVAL;

	if (!data)
		return -ENODEV;
	mutex_lock(&data->lock);
	if (data->dead) {
		ret = -ENODEV;
		goto unlock;
	}
	for (event = 0; event < ARRAY_SIZE(names); event++) {
		if (jiiov_prefix(buf, count, names[event], strlen(names[event]))) {
			ret = jiiov_netlink_send(event);
			break;
		}
	}
unlock:
	mutex_unlock(&data->lock);
	/* Stock returns the send result, not the sysfs input count. */
	return ret;
}

static DEVICE_ATTR_WO(resource_set);
static DEVICE_ATTR_WO(device_power);
static DEVICE_ATTR_WO(irq_set);
static DEVICE_ATTR_WO(hw_reset);
static DEVICE_ATTR_WO(pinctl_set);
static DEVICE_ATTR_WO(netlink_event);

static struct attribute *jiiov_attributes[] = {
	&dev_attr_resource_set.attr,
	&dev_attr_device_power.attr,
	&dev_attr_irq_set.attr,
	&dev_attr_hw_reset.attr,
	&dev_attr_pinctl_set.attr,
	&dev_attr_netlink_event.attr,
	NULL,
};

static const struct attribute_group jiiov_attribute_group = {
	.attrs = jiiov_attributes,
};

static void jiiov_stop(struct jiiov_data *data)
{
	int ret = jiiov_irq_free(data);

	if (ret) {
		dev_err(data->dev, "IRQ wake disable failed: %d; wake state remains active\n", ret);
		/* An unbind cannot leave callbacks referring to the removed object. */
		jiiov_irq_dispose(data);
	}
	ret = jiiov_power(data, false);
	if (ret)
		dev_err(data->dev, "power cleanup failed: %d\n", ret);
	/* Never put an enabled regulator or force a shared supply off. */
	jiiov_resources_drop(data);
}

static int jiiov_remove(struct platform_device *pdev)
{
	struct jiiov_data *data = platform_get_drvdata(pdev);

	mutex_lock(&data->lock);
	WRITE_ONCE(data->dead, true);
	mutex_unlock(&data->lock);
	sysfs_remove_group(&pdev->dev.kobj, &jiiov_attribute_group);
	cdev_device_del(&data->cdev, &data->chardev);
	mutex_lock(&data->lock);
	jiiov_stop(data);
	wakeup_source_unregister(data->wakeup);
	data->wakeup = NULL;
	mutex_unlock(&data->lock);
	platform_set_drvdata(pdev, NULL);
	if (data->vdd || data->irq_wake) {
		/* Retained hardware references also pin the release callback code. */
		dev_err(data->dev, "shutdown failed; retaining hardware/device ownership; device restart required\n");
		return 0;
	}
	atomic_set(&jiiov_claimed, 0);
	put_device(&data->chardev);
	return 0;
}

static void jiiov_shutdown(struct platform_device *pdev)
{
	struct jiiov_data *data = platform_get_drvdata(pdev);

	mutex_lock(&data->lock);
	WRITE_ONCE(data->dead, true);
	jiiov_stop(data);
	mutex_unlock(&data->lock);
}

static void jiiov_device_release(struct device *dev)
{
	struct jiiov_data *data = container_of(dev, struct jiiov_data, chardev);

	put_device(data->dev);
	kfree(data);
}

static bool jiiov_has_supply_link(struct device_node *node)
{
	struct property *property;
	size_t len;

	for_each_property_of_node(node, property) {
		len = strlen(property->name);
		if (len >= 7 && !strcmp(property->name + len - 7, "-supply"))
			return true;
	}
	return false;
}

static int jiiov_validate_supply(struct device_node *node)
{
	struct device_node *supply, *parent;
	int ret = -EOPNOTSUPP;

	supply = of_parse_phandle(node, "vdd-supply", 0);
	if (!supply)
		return -EINVAL;
	parent = of_get_parent(supply);
	/*
	 * This is the M2468 rpmh-regulator.c VRM child layout. With no upstream
	 * supply/coupling and uA_load cleared, disable errors precede consuming
	 * enable_count. Do not apply that retry rule to arbitrary regulators.
	 */
	if (parent && of_device_is_compatible(parent, "qcom,rpmh-vrm-regulator") &&
	    !of_find_property(supply, "compatible", NULL) &&
	    !of_find_property(supply, "regulator-coupled-with", NULL) &&
	    !of_find_property(parent, "regulator-coupled-with", NULL) &&
	    !jiiov_has_supply_link(supply) && !jiiov_has_supply_link(parent))
		ret = 0;
	of_node_put(parent);
	of_node_put(supply);
	return ret;
}

static int jiiov_parse_dt(struct jiiov_data *data)
{
	struct device_node *node = data->dev->of_node;
	int ret;

	if (!node)
		return -ENODEV;
	/* Only the observed M2468 PMIC + pinctrl path is implemented. */
	if (!of_property_read_bool(node, "anc,vdd_use_pmic") ||
	    of_property_read_bool(node, "anc,vdd_use_gpio") ||
	    of_property_read_bool(node, "anc,use_gpio_init") ||
	    of_property_read_bool(node, "anc,enable-on-boot"))
		return -EOPNOTSUPP;
	if (!of_find_property(node, "vdd-supply", NULL))
		return -EINVAL;
	ret = jiiov_validate_supply(node);
	if (ret)
		return ret;
	ret = of_property_read_u32_array(node, "anc,vdd_config", data->vdd_config, 3);
	if (ret)
		return ret;
	if (!data->vdd_config[0] || data->vdd_config[0] > data->vdd_config[1] ||
	    data->vdd_config[1] > INT_MAX || data->vdd_config[2] > INT_MAX)
		return -EINVAL;
	/* Raw GPIO levels intentionally ignore the active-low flag, as stock does. */
	ret = of_get_named_gpio(node, "anc,gpio_rst", 0);
	if (ret < 0)
		return ret;
	data->rst_gpio = ret;
	ret = of_get_named_gpio(node, "anc,gpio_irq", 0);
	if (ret < 0)
		return ret;
	data->irq_gpio = ret;
	if (!gpio_is_valid(data->rst_gpio) || !gpio_is_valid(data->irq_gpio) ||
	    data->rst_gpio == data->irq_gpio)
		return -EINVAL;
	return 0;
}

static int jiiov_probe(struct platform_device *pdev)
{
	struct jiiov_data *data;
	int ret;

	if (atomic_cmpxchg(&jiiov_claimed, 0, 1))
		return -EBUSY;
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		atomic_set(&jiiov_claimed, 0);
		return -ENOMEM;
	}
	data->dev = get_device(&pdev->dev);
	data->irq = -1;
	data->dead = true;
	mutex_init(&data->lock);
	INIT_WORK(&data->irq_work, jiiov_irq_work);
	device_initialize(&data->chardev);
	data->chardev.release = jiiov_device_release;
	data->chardev.parent = &pdev->dev;
	data->chardev.class = jiiov_class;
	data->chardev.devt = jiiov_devt;
	ret = jiiov_parse_dt(data);
	if (ret)
		goto put;
	ret = dev_set_name(&data->chardev, "jiiov_fp");
	if (ret)
		goto put;
	data->wakeup = wakeup_source_register(&pdev->dev, "anc_fp_wakelock");
	if (!data->wakeup) {
		ret = -ENOMEM;
		goto put;
	}
	platform_set_drvdata(pdev, data);
	cdev_init(&data->cdev, &jiiov_fops);
	data->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&data->cdev, &data->chardev);
	if (ret)
		goto unregister_wakeup;
	ret = sysfs_create_group(&pdev->dev.kobj, &jiiov_attribute_group);
	if (ret)
		goto del;
	mutex_lock(&data->lock);
	WRITE_ONCE(data->dead, false);
	mutex_unlock(&data->lock);
	dev_info(&pdev->dev, "JIIOV ABI ready; hardware remains off until requested\n");
	return 0;
del:
	cdev_device_del(&data->cdev, &data->chardev);
unregister_wakeup:
	platform_set_drvdata(pdev, NULL);
	wakeup_source_unregister(data->wakeup);
put:
	atomic_set(&jiiov_claimed, 0);
	put_device(&data->chardev);
	return ret;
}

static const struct of_device_id jiiov_match[] = {
	{ .compatible = "jiiov,fingerprint" },
	{ }
};
MODULE_DEVICE_TABLE(of, jiiov_match);

static struct platform_driver jiiov_driver = {
	.probe = jiiov_probe,
	.remove = jiiov_remove,
	.shutdown = jiiov_shutdown,
	.driver = {
		.name = "jiiov_fingerprint",
		.of_match_table = jiiov_match,
	},
};

static int __init jiiov_init(void)
{
	int ret;

	ret = jiiov_netlink_init();
	if (ret)
		return ret;
	ret = alloc_chrdev_region(&jiiov_devt, 0, 1, "jiiov_fp");
	if (ret)
		goto netlink_exit;
	jiiov_class = class_create(THIS_MODULE, "jiiov_fp");
	if (IS_ERR(jiiov_class)) {
		ret = PTR_ERR(jiiov_class);
		goto unregister_region;
	}
	ret = platform_driver_register(&jiiov_driver);
	if (!ret)
		return 0;
	class_destroy(jiiov_class);
unregister_region:
	unregister_chrdev_region(jiiov_devt, 1);
netlink_exit:
	jiiov_netlink_exit();
	return ret;
}

static void __exit jiiov_exit(void)
{
	platform_driver_unregister(&jiiov_driver);
	class_destroy(jiiov_class);
	unregister_chrdev_region(jiiov_devt, 1);
	jiiov_netlink_exit();
}

module_init(jiiov_init);
module_exit(jiiov_exit);
MODULE_DESCRIPTION("Meizu M2468 JIIOV fingerprint platform and ANC HAL transport");
MODULE_LICENSE("GPL v2");
