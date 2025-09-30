/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <driver/kcs/kcs_dev.h>
#include <fcntl.h>
#include <linux/ipmi_bmc.h>
#include <stdio.h>
#include <zephyr/drivers/kcs.h>
#include <zephyr/kernel.h>
#include <zephyr/posix/dirent.h>
#include <zephyr/posix/sys/ioctl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

// 将函数返回类型改为 int
int kcs_main_test()
{
    int file;
    const char* file_name = "/dev/kcs/ipmi-kcs";
    uint8_t req_buf[256] = {0};
    uint8_t resp_buf[256] = {0};
    ssize_t readBytes = -1;
    ssize_t writtenBytes = -1;

    printk("----open test file_name=%s----\n", file_name);
    file = open(file_name, O_RDWR | O_CLOEXEC); // 阻塞打开，便于配合轮询等待
    if (file < 0)
    {
        printk("Failed to open file: %d\n", file);
        return -1;
    }
    printk("Successfully opened file: %d\n", file);

    // ---- 等待并读取host请求
    printk("----wait request (retry with deadline)----\n");
    const int timeout_ms = 30000; // 30s
    const int interval_ms = 50;   // 每次重试间隔
    int waited_ms = 0;

    while (1)
    {
        readBytes = read(file, req_buf, sizeof(req_buf));
        if (readBytes >= 0)
        {
            break; // 成功读到请求
        }

        // 这些错误码属于"尚未就绪/阶段不合适/可重试"，继续等待
        if (readBytes == -ENODATA || readBytes == -EAGAIN ||
            readBytes == -EPERM || errno == EINTR)
        {
            if (waited_ms >= timeout_ms)
            {
                printk("Timeout waiting KCS request (%d ms)\n", timeout_ms);
                close(file);
                return -1;
            }
            k_msleep(interval_ms);
            waited_ms += interval_ms;
            continue;
        }

        // 其他错误：直接失败
        printk("Failed to read from KCS device, err=%zd errno=%d\n", readBytes,
               errno);
        close(file);
        return -1;
    }

    if (readBytes < 2)
    {
        printk(
            "Invalid request length (%zd), need at least 2 bytes (netfn, cmd)\n",
            readBytes);
        close(file);
        return -1;
    }
    printk("Read %zd bytes from KCS device\n", readBytes);
    for (int i = 0; i < readBytes; i++)
    {
        printk("%02x ", req_buf[i]);
    }
    printk("\n");

    // ---- 构造并写入响应 ----
    size_t resp_len = 3;
    resp_buf[0] = req_buf[0] | 0x01; // response bit
    resp_buf[1] = req_buf[1];        // same command
    resp_buf[2] = 0x00;              // completion code: success
    // 如需 payload，可在 resp_buf[3..] 填充并增大 resp_len

    printk("----write response----\n");
    writtenBytes = write(file, resp_buf, resp_len);
    if (writtenBytes < 0)
    {
        printk("Failed to write to KCS device: %zd\n", writtenBytes);
        close(file);
        return -1;
    }
    printk("Wrote %zd bytes to KCS device\n", writtenBytes);

    // ---- 可选的 ioctl 演示 ----
    int ret = 0;
    // 设置SMS_ATN位
    // printk("----ioctl IPMI_BMC_IOCTL_SET_SMS_ATN test----\n");
    // int ret = ioctl(file, IPMI_BMC_IOCTL_SET_SMS_ATN, NULL);
    // if (ret < 0)
    // {
    //     printk("Failed to perform IPMI_BMC_IOCTL_SET_SMS_ATN operation:
    //     %d\n", ret);
    // }
    // else
    // {
    //     printk("IPMI_BMC_IOCTL_SET_SMS_ATN operation successful\n");
    // }

    // 清除SMS_ATN位
    // printk("----ioctl IPMI_BMC_IOCTL_CLEAR_SMS_ATN test----\n");
    // ret = ioctl(file, IPMI_BMC_IOCTL_CLEAR_SMS_ATN, NULL);
    // if (ret < 0)
    // {
    //     printk("Failed to perform IPMI_BMC_IOCTL_CLEAR_SMS_ATN operation:
    //     %d\n", ret);
    // }
    // else
    // {
    //     printk("IPMI_BMC_IOCTL_CLEAR_SMS_ATN operation successful\n");
    // }

    // // 强制中止当前的 IPMI 操作
    // printk("----ioctl IPMI_BMC_IOCTL_FORCE_ABORT test----\n");
    // ret = ioctl(file, IPMI_BMC_IOCTL_FORCE_ABORT, NULL);
    // if (ret < 0)
    // {
    //     printk("Failed to perform IPMI_BMC_IOCTL_FORCE_ABORT operation:
    //     %d\n", ret);
    // }
    // else
    // {
    //     printk("IPMI_BMC_IOCTL_FORCE_ABORT operation successful\n");
    // }

    close(file);

    return ret;
}

static int cmd_kcs_test(const struct shell* sh, size_t argc, char** argv)
{
    // shell_print(sh, "%s", cwd);
    kcs_main_test();

    return 0;
}

// SHELL_CMD_ARG_REGISTER(kcsTest, NULL, "kcs dev file test", cmd_kcs_test, 1,
// 3);
