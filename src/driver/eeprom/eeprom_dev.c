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

#define MAX_EEPROM_DEVICES 8     /* The maximum number of I2C devices */

#define DT_DRV_COMPAT atmel_at24

#define EEPROM_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(EEPROM_NUM <= MAX_EEPROM_DEVICES,
			 "Exceeded maximum I2C device count");

#define EEPROM_DEVICE_PROCESS(inst)                         \
	{                                                       \
		.dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \
		.label = DT_INST_PROP(inst, label),                 \
		.opened = ATOMIC_INIT(0)},

struct eeprom_device_info eeprom_devices[] = {
	DT_INST_FOREACH_STATUS_OKAY(EEPROM_DEVICE_PROCESS)};

static struct fs_mount_t eepromfs_mnt = {
	.type = FS_EEPROM,
	.fs_data = &eeprom_devices,
};

static const char *getDeviceLabel(const char *filePath)
{
    // 查找 "/devices/" 部分，然后提取其后的设备标识符
    const char *devices_pos = strstr(filePath, "/devices/");
    if (devices_pos != NULL) {
        // 跳过 "/devices/" 部分
        const char *device_part = devices_pos + 9; // strlen("/devices/") = 9
        // 查找下一个斜杠的位置，以获取设备名
        const char *next_slash = strchr(device_part, '/');
        if (next_slash != NULL) {
            // 分配缓冲区存储设备标签
            static char label[64];
            size_t len = next_slash - device_part;
            if (len >= sizeof(label)) {
                len = sizeof(label) - 1;
            }
            strncpy(label, device_part, len);
            label[len] = '\0';
            return label;
        } else {
            // 如果没有找到下一个斜杠，直接返回设备部分
            return device_part;
        }
    }

    // 如果没有找到 "/devices/"，则使用原始的getFileName逻辑
    const char *lastSlash = strrchr(filePath, '/');
    if (lastSlash != NULL) {
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
    struct eeprom_device_info *client = NULL;

    // 遍历所有 I2C 设备实例，找到对应的设备
    const char *device_label = getDeviceLabel(file_name);
	for (size_t i = 0; i < EEPROM_NUM; i++)
	{
		if (strcmp(eeprom_devices[i].label, device_label) == 0)
		{
			client = &eeprom_devices[i];
			break;
		}
	}

	if (client == NULL)
	{
		LOG_ERROR("Error: EEPROM device with name '%s' not found, label '%s'", file_name, device_label);
		return -ENODEV;
	}

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

	if (zfp->mp == NULL)
	{
		LOG_ERROR("Error: File system mount point is NULL\n");
		atomic_set(&client->opened, 0);
		return -EINVAL;
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

    /* 关闭时将offset清零，确保下次打开时从起始位置开始操作 */
    if (client != NULL) {
        client->offset = 0;
    }

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

	if (ptr == NULL)
	{
		LOG_ERROR("Null buffer pointer");
		return -EFAULT;
	}

	if (size > CONFIG_EEPROM_DEV_MAX_SIZE)
	{
		LOG_DBG("Truncated read size from %zu to %d", size, CONFIG_EEPROM_DEV_MAX_SIZE);
		size = CONFIG_EEPROM_DEV_MAX_SIZE;
	}

	// 验证读取范围是否在有效范围内
	if (client->offset + size > CONFIG_EEPROM_DEV_MAX_SIZE)
	{
		LOG_DBG("Read range exceeds device size, adjusting size from %zu to %ld",
				size, CONFIG_EEPROM_DEV_MAX_SIZE - client->offset);
		size = CONFIG_EEPROM_DEV_MAX_SIZE - client->offset;
	}

	int ret = eeprom_read(client->dev, client->offset, (uint8_t *)ptr, size);
	if (ret != 0)
	{
		LOG_ERROR("Read failed at offset 0x%ld, size %zu: %d", client->offset, size, ret);
		return ret;
	}

	LOG_DBG("Read %zu bytes at offset 0x%ld successfully", size, client->offset);
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

	if (size == 0)
	{
		LOG_ERROR("Write size is 0, nothing to write");
		return 0;
	}

	if (size > CONFIG_EEPROM_DEV_MAX_SIZE)
	{
		LOG_ERROR("Truncated write size from %zu to %d", size, CONFIG_EEPROM_DEV_MAX_SIZE);
		size = CONFIG_EEPROM_DEV_MAX_SIZE;
	}

	// 验证写入范围是否在有效范围内
	if (client->offset + size > CONFIG_EEPROM_DEV_MAX_SIZE)
	{
		LOG_ERROR("Write range exceeds device size, adjusting size from %zu to %ld",
				size, CONFIG_EEPROM_DEV_MAX_SIZE - client->offset);
		size = CONFIG_EEPROM_DEV_MAX_SIZE - client->offset;
	}

	// 检查是否尝试写入EEPROM的关键区域（前8字节通常包含设备标识、CRC校验等）
	if (client->offset < 8 && (client->offset + size) > 0)
	{
		LOG_ERROR("Attempting to write to EEPROM critical area (offset %ld, size %zu). This may affect device metadata.",
				client->offset, size);
	}

	int ret = eeprom_write(client->dev, client->offset, write_buf, size);
	if (ret != 0)
	{
		LOG_ERROR("Write failed at offset 0x%ld, size %zu: %d", client->offset, size, ret);
		return ret;
	}

	LOG_DBG("Successfully wrote %zu bytes at offset 0x%ld to device", size, client->offset);
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

    /* 检查所有EEPROM设备是否就绪 */
    for (int i = 0; i < EEPROM_NUM; i++) {
        if (!device_is_ready(eeprom_devices[i].dev)) {
            LOG_ERROR("EEPROM device %d (%s) not ready", i,
                      eeprom_devices[i].dev->name ? eeprom_devices[i].dev->name : "unknown");
            return -ENODEV;
        }
        LOG_DBG("EEPROM device %d (%s) is ready", i,
                eeprom_devices[i].dev->name ? eeprom_devices[i].dev->name : "unknown");
    }

    /* 挂载点格式验证 */
    if (mountp->mnt_point[0] != '/') {
        LOG_ERROR("Mount point must be absolute path: %s", mountp->mnt_point);
        return -EINVAL;
    }

    LOG_DBG("Mounted EEPROM at %s with %d devices", mountp->mnt_point, EEPROM_NUM);
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
    if (mountp == NULL) {
        LOG_ERROR("Null mount pointer");
        return -EINVAL;
    }

    if (mountp->mnt_point == NULL) {
        LOG_ERROR("Mount point is NULL");
        return -EINVAL;
    }

    /* 检查所有设备使用状态 */
    for (int i = 0; i < EEPROM_NUM; i++) {
        if (atomic_get(&eeprom_devices[i].opened)) {
            LOG_ERROR("Cannot unmount - device %s (index %d) in use",
                     eeprom_devices[i].label ? eeprom_devices[i].label : "unknown",
                     i);
            return -EBUSY;
        }
        LOG_DBG("Device %s (index %d) is not in use",
                eeprom_devices[i].label ? eeprom_devices[i].label : "unknown",
                i);
    }

    /* 清除挂载点（内存屏障保证可见性） */
    mountp->mnt_point = NULL;

    LOG_DBG("Successfully unmounted EEPROM filesystem at %s",
            mountp->mnt_point ? mountp->mnt_point : "NULL");
    return 0;
}

static int eepromdev_opendir(struct fs_dir_t *dp, const char *path)
{
    LOG_DBG("Opening EEPROM directory: %s", path);

    if (!dp || !path) {
        LOG_ERROR("Invalid parameters: dp=%p, path=%p", dp, path);
        return -EINVAL;
    }

    // 根目录处理
    if (strcmp(path, EEPROM_MNT_POINT) == 0) {
        struct eeprom_dir_ctx *ctx = malloc(sizeof(struct eeprom_dir_ctx));
        if (!ctx) {
            LOG_ERROR("Failed to allocate memory for root directory context");
            return -ENOMEM;
        }

        ctx->type = ROOT_DIR;
        ctx->index = 0;
        dp->dirp = ctx;
        return 0;
    }

    // 设备目录处理
    const size_t base_len = strlen(EEPROM_MNT_POINT);
    if (strncmp(path, EEPROM_MNT_POINT, base_len) == 0 && path[base_len] == '/') {
        const char *dev_part = path + base_len + 1;

        // 遍历设备列表查找匹配的标签
        for (int i = 0; i < EEPROM_NUM; i++) {
            if (eeprom_devices[i].label && strcmp(eeprom_devices[i].label, dev_part) == 0) {
                struct eeprom_dir_ctx *ctx = malloc(sizeof(struct eeprom_dir_ctx));
                if (!ctx) {
                    LOG_ERROR("Failed to allocate memory for device directory context");
                    return -ENOMEM;
                }

                ctx->type = DEVICE_DIR;
				ctx->dev.devid = i;
				ctx->dev.attr_idx = 0;
				dp->dirp = ctx;
                return 0;
            }
        }
    }

    LOG_ERROR("Invalid EEPROM path: %s", path);
    return -ENOENT;
}

static int eepromdev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
    LOG_DBG("Reading directory entry");

    if (!dp || !dp->dirp || !entry) {
		LOG_ERROR("Invalid parameters");
		return -EINVAL;
	}

    struct eeprom_dir_ctx *ctx = dp->dirp;

    switch (ctx->type) {
	case ROOT_DIR:
		if (ctx->index < EEPROM_NUM && eeprom_devices[ctx->index].label) {
			strncpy(entry->name, eeprom_devices[ctx->index].label, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
			entry->type = FS_DIR_ENTRY_DIR;
			ctx->index++;
            LOG_DBG("Read directory entry: %s (type: dir)", entry->name);
			return 0;
		}
		break;
    case DEVICE_DIR:
        if (ctx->dev.attr_idx == 0) {
            entry->type = FS_DIR_ENTRY_FILE;
            strncpy(entry->name, "eeprom", sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            ctx->dev.attr_idx++;
            LOG_DBG("Read directory entry: %s (type: file)", entry->name);
            return 0;
	    }
        break;
    default:
		LOG_ERROR("Unknown directory type: %d", ctx->type);
		return -EINVAL;
	}

    memset(entry, 0, sizeof(*entry));
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
    LOG_DBG("Stat path: %s", path);

    if (!mountp || !path || !entry) {
        LOG_ERROR("Invalid parameters: mountp=%p, path=%p, entry=%p", mountp, path, entry);
        return -EINVAL;
    }

    // 根目录
    if (strcmp(path, EEPROM_MNT_POINT) == 0) {
        entry->type = FS_DIR_ENTRY_DIR;
        entry->size = 0;
        return 0;
    }

    // 设备目录和EEPROM文件
    const size_t base_len = strlen(EEPROM_MNT_POINT);
    if (strncmp(path, EEPROM_MNT_POINT, base_len) == 0 && path[base_len] == '/') {
        const char *subpath = path + base_len + 1;

        // 遍历设备列表查找匹配的标签
        for (int i = 0; i < EEPROM_NUM; i++) {
            if (eeprom_devices[i].label && strcmp(eeprom_devices[i].label, subpath) == 0) {
                // 匹配设备目录
                entry->type = FS_DIR_ENTRY_DIR;
                entry->size = 0;
                return 0;
            }

            // 检查设备下的eeprom文件路径格式：<device_label>/eeprom
            if (strlen(subpath) > strlen("eeprom") &&
                strncmp(subpath, eeprom_devices[i].label, strlen(eeprom_devices[i].label)) == 0 &&
                subpath[strlen(eeprom_devices[i].label)] == '/' &&
                strcmp(subpath + strlen(eeprom_devices[i].label) + 1, "eeprom") == 0) {
                // 匹配设备下的eeprom文件
                entry->type = FS_DIR_ENTRY_FILE;
                entry->size = CONFIG_EEPROM_DEV_MAX_SIZE;
                return 0;
            }
        }
    }

    LOG_ERROR("Invalid path for stat: %s", path);
    return -ENOENT;
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
	LOG_DBG("Initializing EEPROM devices under %s", EEPROM_MNT_POINT);

	LOG_DBG("EEPROM_NUM = %d\n", EEPROM_NUM);

	// 输出eeprom设备name和label信息
	for (int i=0; i < EEPROM_NUM; i++){
		if (eeprom_devices[i].dev->name != NULL){
			LOG_DBG("eeprom_devices[%d].dev->name = %s\n", i, eeprom_devices[i].dev->name);
		}
		if (eeprom_devices[i].label != NULL){
			LOG_DBG("eeprom_devices[%d].label = %s\n", i, eeprom_devices[i].label);
		}
	}

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
