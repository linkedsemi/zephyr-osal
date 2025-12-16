/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/fdtable.h>
#include <driver/eeprom/eeprom_dev.h>

LOG_MODULE_REGISTER(eeprom_dev, LOG_LEVEL_ERR); // 注册日志模块

#define CONFIG_EEPROM_DEV_MAX_SIZE 65536

#define DT_DRV_COMPAT atmel_at24

#define EEPROM_LABEL "eeprom"

struct eeprom_device_info eeprom_devices = {
    .dev = DEVICE_DT_GET(DT_NODELABEL(eeprom)),
    .opened = ATOMIC_INIT(0)
};

static int eeprom_index = 0; // 当前遍历的 EEPROM 设备索引

static struct fs_mount_t eepromfs_mnt = {
	.type = FS_EEPROM,
	.fs_data = &eeprom_devices,
};

static const char *getFileName(const char *filePath)
{
	const char *lastSlash = strrchr(filePath, '/');
	if (lastSlash != NULL)
	{
		return lastSlash + 1;
	}
	return filePath;
}

/**
 * @brief 打开EEPROM设备文件
 * @param zfp 文件系统文件指针
 * @return 0 成功，负数表示错误
 */
static int eepromdev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
	LOG_DBG("Opening EEPROM device file: %s", file_name);

    // 直接验证文件名是否匹配
    if (strcmp(getFileName(file_name), EEPROM_LABEL) != 0) {
        LOG_ERROR("Invalid EEPROM device name: %s (expected: %s)",
               file_name, EEPROM_LABEL);
        return -ENODEV;
    }

    struct eeprom_device_info *client = &eeprom_devices;

	if (!atomic_cas(&client->opened, 0, 1))
	{
		LOG_ERROR("device name is opened");
		return -EINVAL;
	}

	if (!device_is_ready(client->dev))
	{
		LOG_ERROR("Error: EEPROM device with name '%s' is not ready\n", file_name);
		atomic_set(&client->opened, 0);
		return -ENODEV;
	}

	zfp->filep = client;
	zfp->mp = &eepromfs_mnt;

    LOG_DBG("EEPROM opened successfully");
	return 0;
}

/**
 * @brief 关闭EEPROM设备文件
 * @param zfp 文件系统文件指针
 * @return 0 成功，负数表示错误
 */
static int eepromdev_close(struct fs_file_t *zfp)
{
    if (zfp == NULL) {
        LOG_ERROR("Invalid file pointer (NULL)");
        return -EINVAL;
    }

    struct eeprom_device_info *client = zfp->filep;

    /* 原子操作确保线程安全 */
    if (client != NULL) {
        if (!atomic_cas(&client->opened, 1, 0)) {
            LOG_ERROR("EEPROM device was not properly opened");
        }
	client->offset = 0;
        LOG_DBG("Released EEPROM device lock");
    }

    /* 清理文件指针 */
    zfp->filep = NULL;
    zfp->mp = NULL;

    LOG_DBG("EEPROM device closed successfully");
    return 0;
}

/**
 * @brief EEPROM设备文件数据读取
 * @param zfp 文件系统文件指针
 * @return 0 成功，负数表示错误
 */
static ssize_t eepromdev_read(struct fs_file_t *zfp, void *ptr, size_t size)
{
	LOG_DBG("eepromdev_read start----\n");
	struct eeprom_device_info *client = zfp->filep;

	if (client == NULL)
	{
		LOG_ERROR("No client context");
		return -EINVAL;
	}

	if (size > CONFIG_EEPROM_DEV_MAX_SIZE)
	{
		size = CONFIG_EEPROM_DEV_MAX_SIZE;
	}

	int ret = eeprom_read(client->dev, client->offset, (uint8_t *)ptr, size);
	if (ret != 0)
	{
		LOG_ERROR("Read failed: %d", ret);
		return ret;
	}

	LOG_DBG("Read %zu bytes", size);
	return size;
}

/**
 * @brief EEPROM设备文件数据写入
 * @param zfp 文件系统文件指针
 * @return 0 成功，负数表示错误
 */
static ssize_t eepromdev_write(struct fs_file_t *zfp, const void *ptr, size_t size)
{
	LOG_DBG("eepromdev_write start----\n");
	struct eeprom_device_info *client = zfp->filep;
	const uint8_t *write_buf = (const uint8_t *)ptr;

	if (client == NULL)
	{
		LOG_ERROR("No client context");
		return -EINVAL;
	}

	if (ptr == NULL)
	{
		LOG_ERROR("Null buffer pointer");
		return -EFAULT;
	}

	if (size > CONFIG_EEPROM_DEV_MAX_SIZE)
	{
		size = CONFIG_EEPROM_DEV_MAX_SIZE;
		LOG_ERROR("Truncated write size to %d", CONFIG_EEPROM_DEV_MAX_SIZE);
	}

	int ret = eeprom_write(client->dev, client->offset, write_buf, size);
	if (ret != 0)
	{
		LOG_ERROR("Write failed: %d (len=%zu)", ret, size);
		return ret;
	}

	LOG_DBG("EEPROM writing %zu bytes to device", size);
	return size;
}

/**
 * @brief EEPROM设备文件指针偏移操作
 * @param fp 文件系统文件指针
 * @param off 偏移量
 * @param whence 偏移起始位置
 * @return 0 成功，负数表示错误
 */
static int eepromdev_lseek(struct fs_file_t *fp, off_t off, int whence)
{
    if (fp == NULL) {
        LOG_ERROR("Invalid file pointer (NULL)");
        return -EINVAL;
    }

    struct eeprom_device_info *client = fp->filep;
    if (client == NULL) {
        LOG_ERROR("No client context");
        return -EINVAL;
    }

    if (off < 0 || off >= CONFIG_EEPROM_DEV_MAX_SIZE) {
        LOG_ERROR("Invalid offset value: %ld", off);
        return -EINVAL;
    }

    switch (whence) {
        case FS_SEEK_SET:
			client->offset = off;
			LOG_DBG("Set offset to %ld", client->offset);
            break;
        case FS_SEEK_CUR:
            LOG_ERROR("FS_SEEK_CUR is not supported for EEPROM");
            return -EINVAL;
        case FS_SEEK_END:
            LOG_ERROR("FS_SEEK_END is not supported for EEPROM");
            return -EINVAL;
        default:
            LOG_ERROR("Invalid whence value: %d", whence);
            return -EINVAL;
    }

    LOG_DBG("EEPROM lseek to offset: %ld", off);
    return 0;
}

/**
 * @brief 获取当前EEPROM文件指针位置
 * @param fp 文件系统文件指针
 * @return 当前文件位置偏移量，负数表示错误
 */
static off_t eepromdev_tell(struct fs_file_t *fp)
{
    if (fp == NULL) {
        LOG_ERROR("Invalid file pointer (NULL)");
        return -EINVAL;
    }

    struct eeprom_device_info *client = fp->filep;
    if (client == NULL) {
        LOG_ERROR("No client context");
        return -EINVAL;
    }

    return client->offset;
}

/**
 * @brief 挂载EEPROM虚拟文件系统
 * @param mountp 挂载配置指针
 * @return 0 成功，负数表示错误码
 *
 * @retval -EINVAL 参数无效
 * @retval -ENODEV 设备未就绪
 * @retval -ENOENT 挂载点无效
 */
static int eepromdev_mount(struct fs_mount_t *mountp)
{
    /* 参数有效性检查 */
    if (mountp == NULL) {
        LOG_ERROR("Null mount pointer");
        return -EINVAL;
    }

    if (mountp->mnt_point == NULL || strlen(mountp->mnt_point) == 0) {
        LOG_ERROR("Empty mount point");
        return -ENOENT;
    }

    /* 单设备就绪检查 */
    if (!device_is_ready(eeprom_devices.dev)) {
        LOG_ERROR("EEPROM device not ready");
        return -ENODEV;
    }

    /* 挂载点格式验证 */
    if (mountp->mnt_point[0] != '/') {
        LOG_ERROR("Mount point must be absolute path");
        return -EINVAL;
    }

    LOG_DBG("Mounted EEPROM at %s", mountp->mnt_point);
    return 0;
}

/**
 * @brief 卸载EEPROM虚拟文件系统
 * @param mountp 挂载配置指针
 * @return 0 成功，负数表示错误码
 *
 * @retval -EINVAL 参数无效
 * @retval -EBUSY  设备正在使用
 */
static int eepromdev_unmount(struct fs_mount_t *mountp)
{
    /* 参数有效性检查 */
    if (mountp == NULL || mountp->mnt_point == NULL) {
        LOG_ERROR("Invalid mount configuration");
        return -EINVAL;
    }

    /* 检查设备使用状态（原子操作保证线程安全） */
    if (atomic_get(&eeprom_devices.opened)) {
        LOG_ERROR("Cannot unmount - device in use");
        return -EBUSY;
    }

    /* 清除挂载点（内存屏障保证可见性） */
    mountp->mnt_point = NULL;
    compiler_barrier();

    LOG_DBG("Unmounted EEPROM filesystem");
    return 0;
}

static int eepromdev_opendir(struct fs_dir_t *dp, const char *path)
{
	if (strcmp(path, EEPROM_MNT_POINT) != 0)
	{
		LOG_ERROR("Invalid directory path\n");
		return -EINVAL;
	}

	dp->dirp = &eeprom_index;

	LOG_DBG("EEPROM device opendir successfully\n");
	return 0;
}

static int eepromdev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
	int *index = dp->dirp;

	if (*index == 0)
	{
		// 构造文件条目信息
		snprintf(entry->name, sizeof(entry->name), "%s", "eeprom");
		entry->type = FS_DIR_ENTRY_FILE;
		entry->size = 0; // EEPROM 设备没有文件大小
		(*index)++;
		return 0;
	}

	// 没有更多条目
	memset(entry, 0, sizeof(struct fs_dirent));
	LOG_DBG("No more EEPROM devices to list\n");
	return 0;
}

static int eepromdev_closedir(struct fs_dir_t *dp)
{
    if (dp->dirp == NULL)
    {
        LOG_ERROR("Error: NULL pointer passed to eepromdev_closedir\n");
        return -EINVAL;
    }

    dp->dirp = NULL;

    LOG_DBG("EEPROM directory closed successfully\n");
    return 0;
}

static int eepromdev_statvfs(struct fs_mount_t *mountp, const char *path,
                            struct fs_statvfs *stat)
{
	if (!stat) {
		return -EINVAL;
	}

	stat->f_bsize = 1;   // 块大小
	stat->f_frsize = 1;  // 分配单元大小
	stat->f_blocks = 1; // 总块数
	stat->f_bfree = 1;  // 空闲块数

	return 0;
}

static int eepromdev_stat(struct fs_mount_t *mountp,
                         const char *path, struct fs_dirent *entry)
{
    // 检查路径是否为挂载点
    if (strcmp(path, EEPROM_MNT_POINT) == 0)
    {
        // 挂载点是目录
        entry->type = FS_DIR_ENTRY_DIR;
        entry->size = 0;
    }
    else if (strcmp(path, EEPROM_MNT_POINT "/eeprom") == 0)
    {
        // 唯一的 EEPROM 设备路径
        entry->type = FS_DIR_ENTRY_FILE;
        entry->size = 0;
    }
    else
    {
        LOG_ERROR("Invalid path: %s\n", path);
        return -EINVAL;
    }

    LOG_DBG("EEPROM device stat successful: %s\n", path);
    return 0;
}

/* File system interface */
static const struct fs_file_system_t eepromdev_fs = {
	.mount = eepromdev_mount,
	.unmount = eepromdev_unmount,
	.open = eepromdev_open,
	.close = eepromdev_close,
	.read = eepromdev_read,   // 从 EEPROM 设备中读取数据
	.write = eepromdev_write, // 将用户空间的数据写入到 EEPROM 设备
	.lseek = eepromdev_lseek,
	.tell = eepromdev_tell,
	.ioctl = NULL,
	.opendir = eepromdev_opendir,	 // fs ls访问
	.readdir = eepromdev_readdir,	 // fs ls访问
	.closedir = eepromdev_closedir, // fs ls访问
	.stat = eepromdev_stat,		 // fs cd访问
	.statvfs = eepromdev_statvfs,
};

static int eeprom_dev_init(void)
{
	LOG_DBG("eeprom /dev entries driver\n");

	LOG_DBG("eeprom_devices.dev->name = %s\n", eeprom_devices.dev->name);

	eepromfs_mnt.mnt_point = EEPROM_MNT_POINT;
	LOG_DBG("eepromfs_mnt: type: %d, mnt_point: %s, fs_data: %p\n", eepromfs_mnt.type, eepromfs_mnt.mnt_point, eepromfs_mnt.fs_data);

	int rc = fs_mount(&eepromfs_mnt);
	if (rc != 0)
	{
		LOG_ERROR("Error mounting as eepromfs: %d", rc);
		eepromfs_mnt.mnt_point = NULL;
		return rc;
	}

	return 0;
}

static int eeprom_init(void)
{
	LOG_DBG("eeprom_init----------start\n");

	int rc = fs_register(FS_EEPROM, &eepromdev_fs);
	if (rc < 0)
	{
		LOG_ERROR("eepromfs register error (%d)\n", rc);
	}
	else
	{
		LOG_ERROR("eepromfs fs registered\n");
	}

	rc = eeprom_dev_init();
	if (rc != 0)
	{
		LOG_ERROR("eepromfs init failed\n");
		return rc;
	}

	LOG_DBG("eeprom_init----------end\n");
	return rc;
}

// void eeprom_main()
// {
// 	eeprom_dev_init();
// }

// 系统初始化
SYS_INIT(eeprom_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
