// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022 TsingTeng MicroSystem Co., Ltd.
 */

#include "tms_common.h"
#include "meizu_info_compat.h"

/*********** PART0: Global Variables Area ***********/
struct tms_info tms = {0};
unsigned int shutdown = 0;
static bool tms_driver_registered;

/*********** PART1: Declare Area ***********/

/*********** PART2: Function Area ***********/
struct tms_info *tms_common_data_binding(void)
{
    return &tms;
}

void tms_gpio_set(unsigned int gpio, bool state, unsigned long predelay,
                  unsigned long postdelay)
{
    if (!gpio_is_valid(gpio)) {
        TMS_ERR("Gpio %d is invalid\n", gpio);
        return;
    }

    if (predelay) {
        usleep_range(predelay, predelay + 100);
    }

    gpio_set_value(gpio, state);
    TMS_DEBUG("Set gpio[%d] %s\n", gpio, state == OFF ? "LOW" : "HIGH");

    if (postdelay) {
        usleep_range(postdelay, postdelay + 100);
    }
}

void tms_device_unregister(struct dev_register *dev)
{
    device_destroy(tms.class, dev->devno);
    cdev_del(&dev->chrdev);
    unregister_chrdev_region(dev->devno, dev->count);
    TMS_DEBUG("Unregister device\n");
}

int tms_device_register(struct dev_register *dev, void *data)
{
    int ret;

    dev->class = tms.class;
    ret = alloc_chrdev_region(&dev->devno, 0, dev->count, dev->name);

    if (ret < 0) {
        TMS_ERR("Alloc chrdev region failed, ret = %d\n", ret);
        return ret;
    }

    cdev_init(&dev->chrdev, dev->fops);
    dev->chrdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->chrdev, dev->devno, dev->count);

    if (ret < 0) {
        TMS_ERR("Add char device failed, ret = %d\n", ret);
        goto err_free_devno;
    }

    dev->creation = device_create(tms.class, NULL, dev->devno, data, "%s", dev->name);

    if (IS_ERR(dev->creation)) {
        ret = PTR_ERR(dev->creation);
        TMS_ERR("Create the device failed, ret = %d\n", ret);
        goto err_delete_cdev;
    }

    TMS_DEBUG("Register device success\n");
    return SUCCESS;
err_delete_cdev:
    cdev_del(&dev->chrdev);
err_free_devno:
    unregister_chrdev_region(dev->devno, dev->count);
    return ret;
}

static ssize_t proc_shutdown_read(struct file *file, char __user *buf,
                                       size_t count, loff_t *ppos)
{
    char page[PAGESIZE] = {0};
    snprintf(page, PAGESIZE - 1, "%u\n", shutdown);
    return simple_read_from_buffer(buf, count, ppos, page, strlen(page));
}

static ssize_t proc_shutdown_write(struct file *file,
                                        const char __user *buf, size_t count, loff_t *lo)
{
    int tmp = 0;
    char buffer[4] = {0};

    if ((count > 2) || copy_from_user(buffer, buf, count)) {
        return -EPERM;
    }

    if (sscanf(buffer, "%d", &tmp) == 1) {
        shutdown = tmp;
    } else {
        return -EPERM;
    }

    return count;
}

DECLARE_PROC_OPS(proc_shutdown_ops, simple_open, proc_shutdown_read, proc_shutdown_write, NULL, NULL);

/*********** PART3: TMS Common Start Area ***********/
static int tms_proc_init(void)
{
    int ret;
    struct proc_dir_entry *prEntry_tmp = NULL;

    tms.prEntry = proc_mkdir("tmsdev", NULL);
    if (tms.prEntry == NULL) {
        TMS_ERR("Couldn't create tmsdev proc entry\n");
        return -ENOMEM;
    }

    ret = tms_debuger_proc_create(tms.prEntry);
    if (ret) {
        TMS_ERR("Couldn't create debuger procs entry\n");
        return ret;
    }

    prEntry_tmp = proc_create_data("shutdown", 0664, tms.prEntry,
                                   &proc_shutdown_ops, NULL);
    if (prEntry_tmp == NULL) {
        TMS_ERR("Couldn't create shutdown proc entry\n");
        return -ENODEV;
    }

    return SUCCESS;
}

static int tms_common_probe(void)
{
    int ret;

    /* step1 : binding common data and function */
    tms.vendor           = "tms";
    tms.registe_device   = tms_device_register;
    tms.unregiste_device = tms_device_unregister;
    tms.set_gpio         = tms_gpio_set;

    /* step2 : init debuger */
    ret = tms_debuger_init();
    if (ret) {
        TMS_ERR("debuger init failed, ret = %d\n", ret);
        goto exit;
    }

    /* step3 : init proc */
    ret = tms_proc_init();
    if (ret) {
        TMS_ERR("NFC device proc create failed.\n");
        goto exit;
    }

    /* step4 : tms class register */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,3,0)
    tms.class = class_create(THIS_MODULE, DEVICES_CLASS_NAME);
#else
    tms.class = class_create(DEVICES_CLASS_NAME);
#endif

    if (IS_ERR(tms.class)) {
        ret = PTR_ERR(tms.class);
        TMS_ERR("Failed to register device class\n");
        goto exit;
    }

    TMS_INFO("Successfully\n");
    return SUCCESS;

exit:
    TMS_ERR("Failed, ret = %d\n", ret);
    return ret;
}

/*********** PART4: TMS Module Init Area ***********/
static void tms_common_remove(void)
{
    if (!IS_ERR_OR_NULL(tms.class))
        class_destroy(tms.class);
    if (tms.prEntry)
        remove_proc_subtree("tmsdev", NULL);
    tms_debuger_deinit();
    memset(&tms, 0, sizeof(tms));
}

static int __init tms_driver_init(void)
{
    int ret = 0;
    int rf_id = nfc_read_rf_id();

    if (rf_id == MEIZU_NFC_RF_ID_NXP) {
        TMS_INFO("Skipping TMS NFC for RF-ID %d\n", rf_id);
        return 0;
    }

    TMS_INFO("Loading TMS NFC for RF-ID %d\n", rf_id);

    TMS_INFO("Kernel : %d.%d.%d, Driver version : %x\n",
             (LINUX_VERSION_CODE >> 16) & 0xFFFF,
             (LINUX_VERSION_CODE >> 8) & 0xFF,
             LINUX_VERSION_CODE & 0xFF,
             DRIVER_VERSION);
    ret = tms_common_probe();
    if (ret) {
        TMS_ERR("Failed to init Common\n");
        goto err;
    }

    ret = nfc_driver_init();
    tms_driver_registered = !ret;
err:
    if (ret)
        tms_common_remove();
    return ret;
}

static void __exit tms_driver_exit(void)
{
    if (!tms_driver_registered)
        return;

    TMS_INFO("Enter\n");
    nfc_driver_exit();

    tms_common_remove();
}

module_init(tms_driver_init);
module_exit(tms_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Guoliang Wu <wugl@tsinghuaic.com>");
MODULE_DESCRIPTION("Board Support Driver for TMS Chip");
