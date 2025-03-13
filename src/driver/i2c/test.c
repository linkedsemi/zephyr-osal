/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
// #include <driver/i2c/i2c_dev.h>

void i2c_main_test()
{
    struct fs_file_t zfp;
    const char *file_name = "i2c-0";
    fs_mode_t mode = FS_O_READ | FS_O_WRITE;

    int ret = fs_open(&zfp, file_name, mode);
    if (ret != 0)
    {
        printk("Failed to open file: %d\n", ret);
        return;
    }

    // 执行读写操作
    uint8_t buffer[10];
    ret = fs_read(&zfp, buffer, sizeof(buffer));
    if (ret > 0)
    {
        printk("Read %d bytes from I2C device\n", ret);
    }

    ret = fs_write(&zfp, buffer, sizeof(buffer));
    if (ret > 0)
    {
        printk("Wrote %d bytes to I2C device\n", ret);
    }

    ret = ioctl(&zfp, buffer, sizeof(buffer));

    ret = fs_close(&zfp);
    if (ret != 0)
    {
        printk("Failed to close file: %d\n", ret);
    }
}
