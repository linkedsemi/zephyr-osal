/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <fcntl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/dirent.h>
#include <zephyr/sys/printk.h>
#include <driver/hwmon/hwmon.h>

// hwmon test function
int hwmon_main_test(void)
{
    struct fs_file_t file;
    struct fs_dir_t dir;
    struct fs_dirent entry;
    struct fs_statvfs stat;
    char buffer[64];
    int ret;

    // 初始化所有结构体
    memset(&file, 0, sizeof(file));
    memset(&dir, 0, sizeof(dir));
    memset(&entry, 0, sizeof(entry));
    memset(&stat, 0, sizeof(stat));

    printk("\n==== HWMON Filesystem Test ====\n");

    // 1. Test opening root directory
    printk("\n1. Test opening hwmon root directory:\n");
    ret = fs_opendir(&dir, HWMON_MNT_POINT);
    if (ret) {
        printk("  Failed to open directory: %d\n", ret);
        return ret;
    }
    printk("  Successfully opened directory: %s\n", HWMON_MNT_POINT);

    // 2. Test reading directory contents
    printk("\n2. Test reading hwmon device list:\n");
    while (1) {
        ret = fs_readdir(&dir, &entry);
        if (ret || entry.name[0] == 0) {
            break;
        }
        printk("  Found device: %s (Type: %s)\n", 
               entry.name, 
               (entry.type == FS_DIR_ENTRY_DIR) ? "Directory" : "File");
    }
    fs_closedir(&dir);

    // 3. Test stat functionality
    printk("\n3. Test stat functionality:\n");
    char root_path[] = HWMON_MNT_POINT;
    ret = fs_stat(root_path, &entry);
    if (ret == 0) {
        printk("  Root directory status: %s (Type: %s)\n", 
               root_path,
               (entry.type == FS_DIR_ENTRY_DIR) ? "Directory" : "File");
    } else {
        printk("  Failed to get root directory status: %d\n", ret);
    }

    // 4. Test accessing specific device
    printk("\n4. Test accessing hwmon device:\n");
    ret = fs_opendir(&dir, HWMON_MNT_POINT "/hwmon0");
    if (ret == 0) {
        printk("  Successfully opened hwmon0 device directory\n");
        
        // Read device attributes
        while (1) {
            ret = fs_readdir(&dir, &entry);
            if (ret || entry.name[0] == 0) {
                break;
            }
            printk("    Attribute file: %s\n", entry.name);
        }
        fs_closedir(&dir);
    } else {
        printk("  Failed to open hwmon0 device directory: %d\n", ret);
    }

    // 5. Test reading specific attribute values
    printk("\n5. Test reading sensor values:\n");
    const char *temp_file = HWMON_MNT_POINT "/hwmon0/in1_input";
    ret = fs_open(&file, temp_file, FS_O_READ);
    if (ret == 0) {
        ssize_t bytes_read = fs_read(&file, buffer, sizeof(buffer)-1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            printk("  ADC voltage value: %s", buffer);
        } else {
            printk("  Failed to read voltage value: %d\n", (int)bytes_read);
        }
        fs_close(&file);
    } else {
        printk("  Failed to open ADC device file: %d\n", ret);
    }

    // 6. Test name attribute
    printk("\n6. Test reading device name:\n");
    const char *name_file = HWMON_MNT_POINT "/hwmon0/name";
    ret = fs_open(&file, name_file, FS_O_READ);
    if (ret == 0) {
        ssize_t bytes_read = fs_read(&file, buffer, sizeof(buffer)-1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            printk("  Device name: %s", buffer);
        } else {
            printk("  Failed to read device name: %d\n", (int)bytes_read);
        }
        fs_close(&file);
    } else {
        printk("  Failed to open name file: %d\n", ret);
    }

    // 7. Test statvfs functionality
    printk("\n7. Test filesystem statistics:\n");
    ret = fs_statvfs(HWMON_MNT_POINT, &stat);
    if (ret == 0) {
        printk("  Block size: %lu\n", stat.f_bsize);
        printk("  Fragment size: %lu\n", stat.f_frsize);
        printk("  Total blocks: %lu\n", stat.f_blocks);
        printk("  Free blocks: %lu\n", stat.f_bfree);
    } else {
        printk("  Failed to get filesystem statistics: %d\n", ret);
    }

    printk("\n==== HWMON Test Completed ====\n");
    return 0;
}