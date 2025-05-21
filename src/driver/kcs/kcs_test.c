/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <fcntl.h>
#include <zephyr/posix/sys/ioctl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/dirent.h>
#include <zephyr/drivers/kcs.h>
#include <zephyr/sys/printk.h>
#include <driver/kcs/kcs_dev.h>
#include <linux/ipmi_bmc.h>

// 将函数返回类型改为 int
int kcs_main_test()
{
    int file;
    const char *file_name = "/dev/kcs/ipmi-kcs";
    char buf_data[] = "  hello world!";
    uint8_t buffer[10];
    uint8_t test_buf[] = {0, 0};
    /* addr */
    // test_buf[0] = 0;
    // test_buf[1] = 0;
    buf_data[0] = 0;
    buf_data[1] = 0;

    // 重新打开文件，用于后续测试
    printk("----open test file_name=%s----\n", file_name);
    file = open(file_name, O_RDWR | O_CLOEXEC);
    if (file < 0)
    {
        printk("Failed to open file: %d\n", file);
        return -1;
    }
    printk("Successfully opened file: %d\n", file);

    // 执行写操作
    printk("----write test----\n");
    ssize_t writtenBytes = write(file, (char *)buf_data, sizeof(buf_data) - 1);
    if (writtenBytes < 0)
    {
        printk("Failed to write to KCS device: %zd\n", writtenBytes);
        close(file);
        return -1;
    }
    printk("Wrote %zd bytes to KCS device\n", writtenBytes);

    // 执行读操作
    printk("----read test----\n");
    ssize_t readBytes = read(file, buffer, sizeof(buffer));
    if (readBytes < 0)
    {
        printk("Failed to read from KCS device: %zd\n", readBytes);
        close(file);
        return -1;
    }
    printk("Read %zd bytes from KCS device \n", readBytes);
    for (int i = 0; i < readBytes; i++) {
        printk("%02x ", buffer[i]);
    }
    printk("\n");

    // 设置SMS报警命令
    printk("----ioctl IPMI_BMC_IOCTL_SET_SMS_ATN test----\n");
    int ret = ioctl(file, IPMI_BMC_IOCTL_SET_SMS_ATN, NULL);
    if (ret < 0)
    {
        printk("Failed to perform IPMI_BMC_IOCTL_SET_SMS_ATN operation: %d\n", ret);
    }
    else
    {
        printk("IPMI_BMC_IOCTL_SET_SMS_ATN operation successful\n");
    }

    // 清除SMS警报状态
    printk("----ioctl IPMI_BMC_IOCTL_CLEAR_SMS_ATN test----\n");
    ret = ioctl(file, IPMI_BMC_IOCTL_CLEAR_SMS_ATN, NULL);
    if (ret < 0)
    {
        printk("Failed to perform IPMI_BMC_IOCTL_CLEAR_SMS_ATN operation: %d\n", ret);
    }
    else
    {
        printk("IPMI_BMC_IOCTL_CLEAR_SMS_ATN operation successful\n");
    }

    // 强制中止当前的 IPMI 操作
    printk("----ioctl IPMI_BMC_IOCTL_FORCE_ABORT test----\n");
    ret = ioctl(file, IPMI_BMC_IOCTL_FORCE_ABORT, NULL);
    if (ret < 0)
    {
        printk("Failed to perform IPMI_BMC_IOCTL_FORCE_ABORT operation: %d\n", ret);
    }
    else
    {
        printk("IPMI_BMC_IOCTL_FORCE_ABORT operation successful\n");
    }

    close(file);

    return ret;
}