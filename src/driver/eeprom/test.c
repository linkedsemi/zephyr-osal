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
#include <driver/eeprom/eeprom_dev.h>

// 将函数返回类型改为 int
int eeprom_main_test()
{
    int file;
    const char *file_name = "/sys/bus/i2c/device/2-0055/eeprom";
    char fru_data[] = "hello world!";
    uint8_t buffer[256];
    off_t offset = 16; // 初始化偏移量

    printk("----open test file_name=%s----\n", file_name);
    file = open(file_name, O_RDWR | O_CLOEXEC);
    if (file < 0)
    {
        printk("Failed to open file: %d\n", file);
        return -1;
    }
    printk("Successfully opened file: %d\n", file);

    // 执行seek操作
    printk("----seek test----\n");
    off_t result = lseek(file, offset, SEEK_SET);
    if (result == -1) {
        printk("Failed to seek in I2C device\n");
        close(file);
        return -1;
    }
    printk("Seek operation successful, new position: %ld\n", result);

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

    // 执行seek操作
    printk("----seek test----\n");
    offset = 0;
    result = lseek(file, offset, SEEK_SET);
    if (result == -1) {
        printk("Failed to seek in I2C device\n");
        close(file);
        return -1;
    }
    printk("Seek operation successful, new position: %ld\n", result);
    
    // 执行读操作
    printk("----read test----\n");
    ssize_t readBytes = read(file, buffer, sizeof(buffer));
    if (readBytes < 0)
    {
        printk("Failed to read from I2C device: %zd\n", readBytes);
        close(file);
        return -1;
    }
    printk("Read %zd bytes from I2C device \n", readBytes);
    for (int i = 0; i < readBytes; i++) {
        printk("%02x ", buffer[i]);
    }
    printk("\n");

    close(file);

    return 0;
}