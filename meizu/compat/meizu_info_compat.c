// SPDX-License-Identifier: GPL-2.0-only
/* Meizu M2468 board identity compatibility interface. */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "meizu_info_compat.h"

static struct proc_dir_entry *meizu_info_dir;
static u32 meizu_rf_id;

int nfc_read_rf_id(void)
{
	return meizu_rf_id;
}
EXPORT_SYMBOL(nfc_read_rf_id);

static int meizu_model_name_show(struct seq_file *m, void *unused)
{
	seq_puts(m, "m2468\n");
	return 0;
}

static int meizu_rf_id_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "0x%x\n", meizu_rf_id);
	return 0;
}

static int __init meizu_info_compat_init(void)
{
	struct device_node *bootinfo;
	int ret;

	bootinfo = of_find_node_by_path("/bootinfo");
	if (!bootinfo) {
		pr_err("meizu_info_compat: missing /bootinfo\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(bootinfo, "rf_id", &meizu_rf_id);
	of_node_put(bootinfo);
	if (ret) {
		pr_err("meizu_info_compat: cannot read RF-ID: %d\n", ret);
		return ret;
	}

	meizu_info_dir = proc_mkdir("mz_info", NULL);
	if (!meizu_info_dir)
		return -ENOMEM;

	if (!proc_create_single("model_name", 0444, meizu_info_dir,
				meizu_model_name_show) ||
	    !proc_create_single("rf_id", 0444, meizu_info_dir,
				meizu_rf_id_show)) {
		proc_remove(meizu_info_dir);
		return -ENOMEM;
	}

	pr_info("meizu_info_compat: RF-ID = 0x%x\n", meizu_rf_id);
	return 0;
}
module_init(meizu_info_compat_init);

static void __exit meizu_info_compat_exit(void)
{
	proc_remove(meizu_info_dir);
}
module_exit(meizu_info_compat_exit);

MODULE_DESCRIPTION("Meizu M2468 board identity compatibility ABI");
MODULE_LICENSE("GPL");
