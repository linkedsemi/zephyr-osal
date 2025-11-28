/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <driver/hwmon/hwmon.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/drivers/adc.h>
#include <ls_hal_adcv2.h>

LOG_MODULE_REGISTER(hwmon_dev, LOG_LEVEL_ERR);

#define CONFIG_HWMON_DEV_MAX_SIZE 8192
#define MAX_HWMON_DEVICES 8
#define MAX_ATTR_NAME_LEN 32

#define DT_DRV_COMPAT linkedsemi_hwmon

#define HWMON_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(HWMON_NUM <= MAX_HWMON_DEVICES,
             "Exceeded maximum HWMON device count");

#define HWMON_DEVICE_PROCESS(inst)                            \
	{                                                       \
		.dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \
		.label = DT_INST_PROP(inst, label)},

static struct hwmon_device_info hwmon_devices[] = {
	DT_INST_FOREACH_STATUS_OKAY(HWMON_DEVICE_PROCESS)
};

static struct fs_mount_t hwmonfs_mnt = {
	.type = FS_HWMON,
	.fs_data = &hwmon_devices,
};

// poll信号用于支持异步I/O
static struct k_poll_signal hwmon_poll_signal = K_POLL_SIGNAL_INITIALIZER(hwmon_poll_signal);

/**
 * 从路径中提取hwmon设备名
 */
static int get_hwmon_device(const char *path, char *output, size_t output_size)
{
	const size_t base_len = strlen(HWMON_MNT_POINT);

	if (strncmp(path, HWMON_MNT_POINT, base_len) != 0) {
		return -1;
	}

	const char *hwmon_start = path + base_len + 1;
	const char *current = hwmon_start;
	size_t length = 0;

	while (*current && *current != '/' && length < output_size - 1) {
		output[length++] = *current++;
	}

	output[length] = '\0';
	return (length > 0) ? 0 : -1;
}

/**
 * 从路径中提取属性名
 */
static int get_hwmon_attr(const char *path, char *result, size_t max_len)
{
	if (!path || !result || max_len == 0) {
		return -1;
	}

	const char *last_slash = strrchr(path, '/');
	if (!last_slash) {
		return -1;
	}

	const char *input_start = last_slash + 1;
	size_t len = strlen(input_start);

	if (len >= max_len) {
		return -1;
	}

	strncpy(result, input_start, len);
	result[len] = '\0';
	return 0;
}

static int hwmondev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
	LOG_DBG("Opening hwmon device: %s", file_name);

	struct hwmon_device_info *client = NULL;
	char device_name[20];
	char attr_name[20];

	if (get_hwmon_device(file_name, device_name, sizeof(device_name)) != 0) {
		LOG_ERROR("HWMON device not found in path: %s", file_name);
		return -ENODEV;
	}

	if (get_hwmon_attr(file_name, attr_name, sizeof(attr_name)) != 0) {
		LOG_ERROR("Failed to extract attribute from path: %s", file_name);
		return -ENODEV;
	}

	// 查找对应的设备
	for (size_t i = 0; i < HWMON_NUM; i++) {
		char hwmon_dev_name[32];
		snprintf(hwmon_dev_name, sizeof(hwmon_dev_name), "hwmon%zu", i);

		if (strcmp(device_name, hwmon_dev_name) == 0) {
			client = &hwmon_devices[i];
			break;
		}
	}

	if (!client) {
		LOG_ERROR("HWMON device '%s' not found", device_name);
		return -ENODEV;
	}

	// 分配文件句柄
	struct hwmon_file_handle *handle = k_malloc(sizeof(struct hwmon_file_handle));
	if (!handle) {
		LOG_ERROR("Failed to allocate file handle");
		return -ENOMEM;
	}

	handle->device = client;
	strncpy(handle->attr_name, attr_name, sizeof(handle->attr_name) - 1);
	handle->attr_name[sizeof(handle->attr_name) - 1] = '\0';
	atomic_set(&handle->opened, 0);
	handle->nonblocking = 0;

	// 检查文件是否已被打开
	if (!atomic_cas(&handle->opened, 0, 1)) {
		k_free(handle);
		LOG_ERROR("Attribute file '%s' is already opened", attr_name);
		return -EINVAL;
	}

	if (!device_is_ready(client->dev)) {
		atomic_set(&handle->opened, 0);
		k_free(handle);
		LOG_ERROR("HWMON device '%s' is not ready", device_name);
		return -ENODEV;
	}

	if (!zfp->mp) {
		atomic_set(&handle->opened, 0);
		k_free(handle);
		LOG_ERROR("Invalid mount point");
		return -EINVAL;
	}

	zfp->filep = handle;
	zfp->mp = &hwmonfs_mnt;

	LOG_DBG("HWMON device '%s' opened successfully", file_name);
	return 0;
}

static int hwmondev_close(struct fs_file_t *zfp)
{
	LOG_DBG("Closing hwmon device");

	if (zfp->filep) {
		struct hwmon_file_handle *handle = zfp->filep;
		atomic_set(&handle->opened, 0);
		k_free(handle);
		zfp->filep = NULL;
	}

	if (zfp->mp) {
		zfp->mp = NULL;
		LOG_DBG("HWMON mount closed");
	}

	return 0;
}

static ssize_t hwmondev_read_adc(const struct device *adc, unsigned int channel_id, uint16_t *result)
{
	if (!adc || !result) {
		return -EINVAL;
	}

	uint16_t *buffer = k_malloc(sizeof(uint16_t));
	if (!buffer) {
		LOG_ERROR("Failed to allocate ADC buffer");
		return -ENOMEM;
	}

	// 配置ADC通道
	struct adc_channel_cfg channel_config = {
		.channel_id = channel_id,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_SAMPLETIME_15CYCLES,
	};

	int ret = adc_channel_setup(adc, &channel_config);
	if (ret) {
		LOG_ERROR("ADC channel setup failed: %d", ret);
		k_free(buffer);
		return ret;
	}

	// 配置采样序列
	struct adc_sequence sequence = {
		.buffer = buffer,
		.buffer_size = sizeof(uint16_t),
	};

	ret = adc_read(adc, &sequence);
	if (ret) {
		LOG_ERROR("ADC read failed: %d", ret);
		k_free(buffer);
		return ret;
	}

	*result = (buffer[0] * 1400) / 4095;
	// 打印ADC设备名称、通道ID和读取结果
	LOG_DBG("ADC Device Name: %s, Channel ID: %u, Result: %u", adc->name, channel_id, *result);
	k_free(buffer);

	return 0;
}

static ssize_t hwmondev_read(struct fs_file_t *zfp, void *ptr, size_t size)
{
	LOG_DBG("Reading from hwmon device");

	if (!zfp || !ptr) {
		LOG_ERROR("Invalid parameters");
		return -EINVAL;
	}

	struct hwmon_file_handle *handle = zfp->filep;
	if (!handle) {
		LOG_ERROR("No client context");
		return -EINVAL;
	}

	struct hwmon_device_info *client = handle->device;
	const char *attr_name = handle->attr_name;

	if (size > CONFIG_HWMON_DEV_MAX_SIZE) {
		size = CONFIG_HWMON_DEV_MAX_SIZE;
	}

	// 检查是否已经读取完成
	if (zfp->flags & FS_FLAG_FILE_READ_END) {
		if (handle->nonblocking) {
			return -EAGAIN;
		}
		return 0;
	}

	const struct hwmon_ls_config *cfg = hwmon_ls_get_config(client->dev);
	if (!cfg) {
		LOG_ERROR("Failed to get hwmon configuration");
		return -ENODEV;
	}

	ssize_t len = 0;

	switch (cfg->class) {
	case HWMON_FAN:  // TODO:
		break;

	case HWMON_ADC:
		if (strlen(attr_name) > 0) {
			unsigned int channel_index;
			char attr_type[16];

			if (sscanf(attr_name, "in%u_%15s", &channel_index, attr_type) >= 1) {
				if (channel_index > 0 && (channel_index-1) < cfg->in_num) {
					if (strcmp(attr_type, "input") == 0) {
						uint16_t adc_value = 0;
						int ret = hwmondev_read_adc(cfg->in[channel_index-1].dev,
						                           cfg->in[channel_index-1].channel, &adc_value);
						if (ret == 0) {
							len = snprintf((char *)ptr, size, "%u\n", adc_value);
							zfp->flags |= FS_FLAG_FILE_READ_END;
							return len;
						} else {
							return ret;
						}
					}
				}
			} else if (strcmp(attr_name, "name") == 0) {
				len = snprintf((char *)ptr, size, "iio_hwmon\n");
				zfp->flags |= FS_FLAG_FILE_READ_END;
				return len;
			}
		}
		break;

	case HWMON_TEMP: // TODO:
		break;

	case HWMON_SENSOR:	// TODO:
		break;
	default:
		LOG_ERROR("Unsupported device class: %d", cfg->class);
		return -ENOTTY;
	}

	zfp->flags |= FS_FLAG_FILE_READ_END;
	return len > 0 ? len : 0;
}

static int hwmon_poll_prepare_ctx(struct hwmon_file_handle *handle,
                                  struct zvfs_pollfd *pfd,
                                  struct k_poll_event **pev,
                                  struct k_poll_event *pev_end)
{
	if (pfd->events & ZVFS_POLLIN) {
		if (*pev == pev_end) {
			return -ENOMEM;
		}

		(*pev)->obj = &hwmon_poll_signal;
		(*pev)->type = K_POLL_TYPE_SIGNAL;
		(*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
		(*pev)->state = K_POLL_STATE_NOT_READY;
		(*pev)++;

		// 立即触发信号，表示数据可读
		k_poll_signal_raise(&hwmon_poll_signal, 0);
	}

	return 0;
}

static int hwmon_poll_update_ctx(struct hwmon_file_handle *handle,
                                 struct zvfs_pollfd *pfd,
                                 struct k_poll_event **pev)
{
	if (pfd->events & ZVFS_POLLIN) {
		if ((*pev)->state != K_POLL_STATE_NOT_READY) {
			pfd->revents |= ZVFS_POLLIN;
		}
		(*pev)++;
	}

	return 0;
}

static int hwmondev_ioctl(struct fs_file_t *zfp, unsigned long request, va_list args)
{
	LOG_DBG("hwmon ioctl: 0x%lx", request);

	struct hwmon_file_handle *handle = zfp->filep;
	if (!handle) {
		LOG_ERROR("No client context");
		return -EINVAL;
	}

	switch (request) {
	case F_GETFL: // TODO:
		return O_RDWR;

	case F_SETFL: {
		int flags = va_arg(args, int);
		handle->nonblocking = (flags & O_NONBLOCK) != 0;
		LOG_DBG("Set nonblocking mode to %d", handle->nonblocking);
		return 0;
	}

	case ZFD_IOCTL_POLL_PREPARE: {
		struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
		struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
		struct k_poll_event *pev_end = va_arg(args, struct k_poll_event *);
		return hwmon_poll_prepare_ctx(handle, pfd, pev, pev_end);
	}

	case ZFD_IOCTL_POLL_UPDATE: {
		struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
		struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
		return hwmon_poll_update_ctx(handle, pfd, pev);
	}

	case ZFD_IOCTL_SET_LOCK:
		return 0;

	default:
		LOG_ERROR("Unsupported ioctl request: 0x%lx", request);
		return -ENOTTY;
	}

	return 0;
}

static int hwmondev_mount(struct fs_mount_t *mountp)
{
	LOG_DBG("Mounting hwmon filesystem");

	if (!mountp || !mountp->mnt_point) {
		LOG_ERROR("Invalid mount point");
		return -EINVAL;
	}

	for (size_t i = 0; i < HWMON_NUM; i++) {
		if (!device_is_ready(hwmon_devices[i].dev)) {
			LOG_ERROR("HWMON device %zu not ready", i);
			return -ENODEV;
		}
	}

	LOG_DBG("HWMON filesystem mounted successfully");
	return 0;
}

static int hwmondev_unmount(struct fs_mount_t *mountp)
{
	LOG_DBG("Unmounting hwmon filesystem");

	if (!mountp || !mountp->mnt_point) {
		LOG_ERROR("Invalid mount point");
		return -EINVAL;
	}

	mountp->mnt_point = NULL;
	LOG_DBG("HWMON filesystem unmounted");
	return 0;
}

static int hwmondev_opendir(struct fs_dir_t *dp, const char *path)
{
	LOG_DBG("Opening directory: %s", path);

	if (!dp || !path) {
		return -EINVAL;
	}

	// 根目录处理
	if (strcmp(path, HWMON_MNT_POINT) == 0) {
		struct hwmon_dir_ctx *ctx = k_malloc(sizeof(struct hwmon_dir_ctx));
		if (!ctx) {
			return -ENOMEM;
		}

		ctx->type = ROOT_DIR;
		ctx->index = 0;
		dp->dirp = ctx;
		return 0;
	}

	// 设备目录处理
	const size_t base_len = strlen(HWMON_MNT_POINT);
	if (strncmp(path, HWMON_MNT_POINT, base_len) == 0 && path[base_len] == '/') {
		const char *dev_part = path + base_len + 1;
		unsigned int device_index;

		if (sscanf(dev_part, "hwmon%u", &device_index) == 1) {
			if (device_index < HWMON_NUM) {
				struct hwmon_dir_ctx *ctx = k_malloc(sizeof(struct hwmon_dir_ctx));
				if (!ctx) {
					return -ENOMEM;
				}

				ctx->type = DEVICE_DIR;
				ctx->dev.devid = device_index;
				ctx->dev.attr_idx = 0;
				dp->dirp = ctx;
				return 0;
			}
		}
	}

	LOG_ERROR("Invalid hwmon path: %s", path);
	return -ENOENT;
}

static int hwmondev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
	LOG_DBG("Reading directory entry");

	if (!dp || !dp->dirp || !entry) {
		LOG_ERROR("Invalid parameters");
		return -EINVAL;
	}

	struct hwmon_dir_ctx *ctx = dp->dirp;

	switch (ctx->type) {
	case ROOT_DIR:
		if (ctx->index < HWMON_NUM) {
			snprintf(entry->name, sizeof(entry->name), "hwmon%zu", ctx->index);
			entry->type = FS_DIR_ENTRY_DIR;
			entry->size = 0;
			ctx->index++;
			return 0;
		}
		break;

	case DEVICE_DIR:
		if (ctx->dev.devid >= HWMON_NUM) {
			LOG_ERROR("Invalid device ID: %zu", ctx->dev.devid);
			return -EINVAL;
		}

		const struct hwmon_device_info *dev = &hwmon_devices[ctx->dev.devid];
		const struct hwmon_ls_config *cfg = hwmon_ls_get_config(dev->dev);

		if (!cfg) {
			LOG_ERROR("Failed to get hwmon config for device %zu", ctx->dev.devid);
			return -ENODEV;
		}

		// 根据设备类型和当前属性索引生成属性信息
		if (ctx->dev.attr_idx < cfg->in_num) {
			char attr_name[MAX_ATTR_NAME_LEN];

			switch (cfg->class) {
			case HWMON_FAN:	// TODO:
				break;
			case HWMON_TEMP:	// TODO:
				break;
			case HWMON_ADC:
				snprintf(attr_name, sizeof(attr_name), "in%u_input", ctx->dev.attr_idx + 1);
				break;
			case HWMON_SENSOR:	// TODO:
				break;
			default:
				LOG_ERROR("Unsupported device class: %d", cfg->class);
				return -ENOTTY;
			}

			strncpy(entry->name, attr_name, sizeof(entry->name) - 1);
			entry->name[sizeof(entry->name) - 1] = '\0';
			entry->type = FS_DIR_ENTRY_FILE;
			entry->size = sizeof(int);
			ctx->dev.attr_idx++;
			return 0;
		} else if (ctx->dev.attr_idx == cfg->in_num) {
			// 添加name属性
			strncpy(entry->name, "name", sizeof(entry->name) - 1);
			entry->name[sizeof(entry->name) - 1] = '\0';
			entry->type = FS_DIR_ENTRY_FILE;
			entry->size = sizeof(int);
			ctx->dev.attr_idx++;
			return 0;
		}
		break;

	default:
		LOG_ERROR("Unknown directory type: %d", ctx->type);
		return -EINVAL;
	}

	// 结束遍历
	memset(entry, 0, sizeof(*entry));
	return 0;
}

static int hwmondev_closedir(struct fs_dir_t *dp)
{
	LOG_DBG("Closing directory");

	if (!dp) {
		return -EINVAL;
	}

	if (dp->dirp) {
		k_free(dp->dirp);
		dp->dirp = NULL;
	}

	return 0;
}

/*
hwmondev_statvfs 函数实现文件系统的统计信息查询功能，用于返回文件系统的基本空间和容量信息。
hwmon不占用实际存储空间，只是提供传感器数据访问接口,这些统计信息对于hwmon这类虚拟文件系统没有实际意义.
初始化stat是为了填充 fs_statvfs 结构体的所有相关字段,避免上层调用报错.
*/
static int hwmondev_statvfs(struct fs_mount_t *mountp, const char *path,
                            struct fs_statvfs *stat)
{
	if (!stat) {
		return -EINVAL;
	}

	stat->f_bsize = 1;   // 块大小
	stat->f_frsize = 1;  // 分配单元大小
	stat->f_blocks = 8192; // 总块数
	stat->f_bfree = 8192;  // 空闲块数

	return 0;
}

static int hwmondev_stat(struct fs_mount_t *mountp, const char *path,
                         struct fs_dirent *entry)
{
	LOG_DBG("Stat path: %s", path);

	if (!mountp || !path || !entry) {
		return -EINVAL;
	}

	// 根目录
	if (strcmp(path, HWMON_MNT_POINT) == 0) {
		entry->type = FS_DIR_ENTRY_DIR;
		entry->size = 0;
		return 0;
	}

	// 设备目录和属性文件
	const size_t base_len = strlen(HWMON_MNT_POINT);
	if (strncmp(path, HWMON_MNT_POINT, base_len) == 0 && path[base_len] == '/') {
		const char *subpath = path + base_len + 1;
		unsigned int dev_index;
		char attr_name[32];

		// 匹配属性文件路径：hwmonX/<attribute>
		if (sscanf(subpath, "hwmon%u/%31s", &dev_index, attr_name) == 2) {
			if (dev_index < HWMON_NUM) {
				const struct hwmon_device_info *dev = &hwmon_devices[dev_index];
				const struct hwmon_ls_config *cfg = hwmon_ls_get_config(dev->dev);

				if (!cfg) {
					LOG_ERROR("Failed to get hwmon config for device %u", dev_index);
					return -ENODEV;
				}

				// 验证属性有效性
				switch (cfg->class) {
					case HWMON_FAN:	// TODO:
						break;

					case HWMON_TEMP: // TODO:
						break;

					case HWMON_ADC: {
						unsigned int sensor_num;
						char attr_type[16];
						if (sscanf(attr_name, "in%u_%15s", &sensor_num, attr_type) >= 1) {
							if (sensor_num > 0 && sensor_num <= cfg->in_num) {
								entry->type = FS_DIR_ENTRY_FILE;
								entry->size = sizeof(int);
								return 0;
							}
						}
						break;
					}
					case HWMON_SENSOR: // TODO:
						break;
				}

				// 检查name属性
				if (strcmp(attr_name, "name") == 0) {
					entry->type = FS_DIR_ENTRY_FILE;
					entry->size = sizeof(int);
					return 0;
				}
			}
		}
		// 匹配设备路径格式：hwmonX
		else if (sscanf(subpath, "hwmon%u", &dev_index) == 1) {
			if (dev_index < HWMON_NUM) {
				entry->type = FS_DIR_ENTRY_DIR;
				entry->size = 0;
				return 0;
			}
		}
	}

	LOG_ERROR("Invalid path for stat: %s", path);
	return -ENOENT;
}

static const struct fs_file_system_t hwmondev_fs = {
	.mount = hwmondev_mount,
	.unmount = hwmondev_unmount,
	.open = hwmondev_open,
	.close = hwmondev_close,
	.read = hwmondev_read,
	.write = NULL,
	.lseek = NULL,
	.ioctl = hwmondev_ioctl,
	.opendir = hwmondev_opendir,
	.readdir = hwmondev_readdir,
	.closedir = hwmondev_closedir,
	.stat = hwmondev_stat,
	.statvfs = hwmondev_statvfs,
};

static int hwmon_dev_init(void)
{
	LOG_DBG("Initializing hwmon devices");
	LOG_DBG("Number of hwmon devices: %d", HWMON_NUM);

	// 输出设备信息
	for (size_t i = 0; i < HWMON_NUM; i++) {
		if (hwmon_devices[i].dev && hwmon_devices[i].dev->name) {
			LOG_DBG("hwmon_devices[%zu].dev->name = %s", i, hwmon_devices[i].dev->name);
		}
		if (hwmon_devices[i].label) {
			LOG_DBG("hwmon_devices[%zu].label = %s", i, hwmon_devices[i].label);
		}
	}

	hwmonfs_mnt.mnt_point = HWMON_MNT_POINT;
	LOG_DBG("hwmonfs_mnt: type: %d, mnt_point: %s", hwmonfs_mnt.type, hwmonfs_mnt.mnt_point);

	int rc = fs_mount(&hwmonfs_mnt);
	if (rc) {
		LOG_ERROR("Failed to mount hwmonfs: %d", rc);
		hwmonfs_mnt.mnt_point = NULL;
		return rc;
	}

	return 0;
}

static int hwmon_init(void)
{
	LOG_DBG("hwmon filesystem initialization started");

	int rc = fs_register(FS_HWMON, &hwmondev_fs);
	if (rc < 0) {
		LOG_ERROR("Failed to register hwmon filesystem: %d", rc);
		return rc;
	}

	LOG_DBG("hwmon filesystem registered");

	rc = hwmon_dev_init();
	if (rc) {
		LOG_ERROR("hwmon device initialization failed: %d", rc);
		return rc;
	}

	LOG_DBG("hwmon filesystem initialization completed");
	return 0;
}

SYS_INIT(hwmon_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
