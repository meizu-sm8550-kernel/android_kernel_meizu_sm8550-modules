// SPDX-License-Identifier: GPL-2.0+

#include "nfc_common.h"

struct nfc_chip_query {
    const u8 *command;
    size_t command_len;
    unsigned int send_attempts;
    unsigned int response_count;
    unsigned int version_from_end;
    bool download;
    u8 versions[3];
};

static const u8 nci_reset[] = { 0x20, 0x00, 0x01, 0x00 };
static const u8 boot_version[] = {
    0x5a, 0x00, 0x05, 0x00, 0x4a, 0x02, 0x00, 0x00, 0x17
};

static const struct nfc_chip_query chip_queries[] = {
    {
        .command = nci_reset,
        .command_len = sizeof(nci_reset),
        .send_attempts = 2,
        .response_count = 2,
        .version_from_end = 3,
        .versions = { 0x00, 0xc0, 0xd0 },
    },
    {
        .command = boot_version,
        .command_len = sizeof(boot_version),
        .send_attempts = 1,
        .response_count = 1,
        .version_from_end = 4,
        .download = true,
        .versions = { 0x00, 0xb0, 0x00 },
    },
};

static int nfc_chip_receive(struct nfc_info *nfc, u8 *response, size_t size)
{
    unsigned int attempt;
    size_t payload_len;
    int ret;

    /* The stock guide polls IRQ ten times, at 5 ms intervals. */
    for (attempt = 0; attempt < 10; attempt++) {
        msleep(5);
        if (gpio_get_value(nfc->hw_res.irq_gpio))
            break;
    }
    if (attempt == 10)
        return -ETIMEDOUT;

    ret = i2c_master_recv(nfc->client, response, NCI_HDR_LEN);
    if (ret != NCI_HDR_LEN)
        return ret < 0 ? ret : -EIO;

    /* Both stock identification queries read one extra trailing byte. */
    payload_len = response[HEAD_PAYLOAD_BYTE] + 1;
    if (payload_len > size - NCI_HDR_LEN)
        return -EMSGSIZE;
    ret = i2c_master_recv(nfc->client, response + NCI_HDR_LEN, payload_len);
    if (ret != payload_len)
        return ret < 0 ? ret : -EIO;

    return NCI_HDR_LEN + payload_len;
}

int nfc_identify_chip(struct nfc_info *nfc)
{
    u8 response[53];
    unsigned int query, attempt, packet, version;
    int ret = -ENODEV;
    int received;

    nfc->chip_name = "unknown";
    for (query = 0; query < ARRAY_SIZE(chip_queries); query++) {
        const struct nfc_chip_query *info = &chip_queries[query];

        nfc_fw_download_control(nfc, info->download);
        nfc_hard_reset(nfc);
        for (attempt = 0; attempt < info->send_attempts; attempt++) {
            ret = i2c_master_send(nfc->client, info->command,
                                   info->command_len);
            if (ret == info->command_len)
                break;
            msleep(10);
        }
        if (ret != info->command_len)
            continue;

        /* Normal mode consumes RESET_RSP then RESET_NTF. */
        received = -EIO;
        for (packet = 0; packet < info->response_count; packet++)
            received = nfc_chip_receive(nfc, response, sizeof(response));

        if (info->download) {
            nfc_fw_download_control(nfc, OFF);
            nfc_hard_reset(nfc);
        }
        if (received < 0) {
            ret = received;
            continue;
        }
        if (received < NCI_HDR_LEN + 1 + info->version_from_end) {
            ret = -EPROTO;
            continue;
        }
        version = response[received - 1 - info->version_from_end] & 0xf0;
        for (attempt = 0; attempt < ARRAY_SIZE(info->versions); attempt++) {
            if (version == info->versions[attempt]) {
                nfc->chip_name = "thn31";
                nfc->tms->vendor = "thn31";
                return 0;
            }
        }
        ret = -ENODEV;
    }

    nfc_fw_download_control(nfc, OFF);
    return ret < 0 ? ret : -ENODEV;
}

static ssize_t chip_name_show(struct device *dev,
                              struct device_attribute *attr, char *buf)
{
    struct nfc_info *nfc = dev_get_drvdata(dev);

    return scnprintf(buf, PAGE_SIZE, "%s\n", nfc->chip_name);
}
static DEVICE_ATTR_RO(chip_name);

int nfc_create_sysfs_interfaces(struct nfc_info *nfc)
{
    int ret;

    ret = device_create_file(nfc->i2c_dev, &dev_attr_chip_name);
    if (ret)
        return ret;
    ret = sysfs_create_link(NULL, &nfc->i2c_dev->kobj, "nfc");
    if (ret)
        device_remove_file(nfc->i2c_dev, &dev_attr_chip_name);
    return ret;
}

void nfc_remove_sysfs_interfaces(struct nfc_info *nfc)
{
    sysfs_remove_link(NULL, "nfc");
    device_remove_file(nfc->i2c_dev, &dev_attr_chip_name);
}
