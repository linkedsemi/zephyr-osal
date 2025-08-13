/*
 * Copyright (c) 2019 Bolt Innovation Management, LLC
 * Copyright (c) 2019 Peter Bigot Consulting, LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <ff.h>

#include "ramdisk.h"

static FATFS fatfs_data;

static struct fs_mount_t ramdisk_mp = {
	.type = FS_FATFS,
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
	.storage_dev = (void *)"/RAM",
	.mnt_point = "/RAM:",
	.fs_data = &fatfs_data,
};
int ramdisk_mount(const char* mp)
{
	int ret;

	ret = fs_mount(&ramdisk_mp);
	if (ret < 0)
	{
		ret = fs_mkfs(FS_FATFS, (uintptr_t)"RAM", NULL, 0);
		if (ret < 0)
		{
			printk_thread("mkfs ramdisk failed. ret %d", ret);
			return ret;
		}

		ret = fs_mount(&ramdisk_mp);
		if (ret < 0)
		{
			printk_thread("mount ramdisk failed. ret %d", ret);
		}
	}

	return ret;
}
