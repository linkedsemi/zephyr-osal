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
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <driver/i2c/i2c_dev.h>

// 将函数返回类型改为 int
int i2c_main_test()
{
    int file;
    const char *file_name = "/dev/i2c-2";
    const char fru_data[] = "hello world!";
    uint8_t buffer[10];
    struct i2c_msg_linux msgs[2];
    struct i2c_rdwr_ioctl_data rdwr;
    uint8_t test_buf[] = {'t', 'e', 's', 't'};

    printk("----open test----\n");
    file = open(file_name, O_RDWR | O_CLOEXEC);
    if (file < 0)
    {
        printk("Failed to open file: %d\n", file);
        return -1;
    }

    // 设置从设备地址
    printk("----ioctl test----\n");
    int ret = ioctl(file, I2C_SLAVE, 0x50);
    if (ret < 0)
    {
        printk("Failed to set I2C slave address: %d\n", ret);
        close(file);
        return -1;
    }

    // 执行写操作
    printk("----write test----\n");
    ssize_t writtenBytes = write(file, (char *)fru_data, sizeof(fru_data) - 1);
    if (writtenBytes < 0)
    {
        printk("Failed to write to I2C device: %zd\n", writtenBytes);
        close(file);
        return -1;
    }
    printk("Wrote %zd bytes to I2C device\n", writtenBytes);

    // 执行读操作
    printk("----read test----\n");
    ssize_t readBytes = read(file, buffer, sizeof(buffer));
    if (readBytes < 0)
    {
        printk("Failed to read from I2C device: %zd\n", readBytes);
        close(file);
        return -1;
    }
    printk("Read %zd bytes from I2C device\n", readBytes);

    // 使用ioctl执行读写操作
    msgs[0].addr = 0x50;
    msgs[0].flags = 0;
    msgs[0].len = sizeof(test_buf);
    msgs[0].buf = test_buf;
    msgs[1].addr = 0x50;
    msgs[1].flags = I2C_MSG_READ;
    msgs[1].len = 6;
    msgs[1].buf = buffer;

    rdwr.msgs = msgs;
    rdwr.nmsgs = 2;

    printk("----ioctl I2C_RDWR test----\n");
    ret = ioctl(file, I2C_RDWR, &rdwr);
    if (ret < 0)
    {
        printk("Failed to perform I2C RDWR operation: %d\n", ret);
    }
    else
    {
        printk("I2C RDWR operation successful\n");
    }

    close(file);

    return ret;
}