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
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <driver/i2c/i2c_dev.h>
#include <zephyr/sys/fdtable.h>

LOG_MODULE_REGISTER(i2c_dev, LOG_LEVEL_ERR); // 注册日志模块

#define CONFIG_I2C_DEV_MAX_SIZE 8192

#define MAX_I2C_DEVICES 16 /* The maximum number of I2C devices */

#define DT_DRV_COMPAT linkedsemi_ls_i2c

#define I2C_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(I2C_NUM <= MAX_I2C_DEVICES,
			 "Exceeded maximum I2C device count");

#define I2C_DEVICE_PROCESS(inst)                            \
	{                                                       \
		.dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \
		.label = DT_INST_PROP(inst, label),                 \
		.addr = 0,                                          \
		.opened = ATOMIC_INIT(0)},

struct i2c_device_info i2c_devices[] = {
	DT_INST_FOREACH_STATUS_OKAY(I2C_DEVICE_PROCESS)};

// struct i2c_device_info i2c_devices[I2C_NUM];

static int i2c_index = 0; // 当前遍历的 I2C 设备索引

static struct fs_mount_t i2cfs_mnt = {
	.type = FS_I2C,
	.fs_data = &i2c_devices,
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

static int i2cdev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
	LOG_ERROR("i2cdev_open start----\n");
	struct i2c_device_info *client = NULL;

	// 遍历所有 I2C 设备实例，找到对应的设备
	for (size_t i = 0; i < I2C_NUM; i++)
	{
		if (strcmp(i2c_devices[i].label, getFileName(file_name)) == 0)
		{
			client = &i2c_devices[i];
			break;
		}
	}

	if (client == NULL)
	{
		LOG_ERROR("Error: I2C device with name '%s' not found\n", file_name);
		return -ENODEV;
	}

	if (!atomic_cas(&client->opened, 0, 1))
	{
		LOG_ERROR("device name is opened");
		return -EINVAL;
	}

	if (!device_is_ready(client->dev))
	{
		LOG_ERROR("Error: I2C device with name '%s' is not ready\n", file_name);
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
	zfp->mp = &i2cfs_mnt;

	LOG_ERROR("I2C device with name '%s' opened successfully\n", file_name);
	return 0;
}

static int i2cdev_close(struct fs_file_t *zfp)
{
	LOG_ERROR("i2cdev_close start----\n");
	if (zfp->filep)
	{
		struct i2c_device_info *client = zfp->filep;
		atomic_set(&client->opened, 0);
	}
	if (zfp->mp)
	{
		zfp->mp = NULL;
		LOG_ERROR("Closed i2cfs mount");
	}

	return 0;
}

static ssize_t i2cdev_read(struct fs_file_t *zfp, void *ptr, size_t size)
{
	LOG_ERROR("i2cdev_read start----\n");
	struct i2c_device_info *client = zfp->filep;

	if (client == NULL)
	{
		LOG_ERROR("No client context");
		return -EINVAL;
	}

	if (size > CONFIG_I2C_DEV_MAX_SIZE)
	{
		size = CONFIG_I2C_DEV_MAX_SIZE;
	}

	int ret = i2c_read(client->dev, (uint8_t *)ptr, size, client->addr);
	if (ret != 0)
	{
		LOG_ERROR("Read failed: %d (addr=0x%02X)", ret, client->addr);
		return ret;
	}

	LOG_ERROR("Read %zu bytes from I2C 0x%02X", size, client->addr);
	return size;
}

static ssize_t i2cdev_write(struct fs_file_t *zfp, const void *ptr, size_t size)
{
	LOG_ERROR("i2cdev_write start----\n");
	struct i2c_device_info *client = zfp->filep;
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

	if (size > CONFIG_I2C_DEV_MAX_SIZE)
	{
		size = CONFIG_I2C_DEV_MAX_SIZE;
		LOG_ERROR("Truncated write size to %d", CONFIG_I2C_DEV_MAX_SIZE);
	}

	int ret = i2c_write(client->dev, write_buf, size, client->addr);
	if (ret != 0)
	{
		LOG_ERROR("Write failed: %d (addr=0x%02X, len=%zu)",
				  ret, client->addr, size);
		return ret;
	}

	LOG_ERROR("I2C writing %zu bytes to device at address 0x%x", size, client->addr);
	return size;
}

static int i2cdev_ioctl_rdwr(const struct device *dev, unsigned nmsgs,
							 struct i2c_msg_linux *msgs)
{
	LOG_ERROR("i2cdev_ioctl_rdwr start----\n");
	int i, res, nmsgs_sent = 0;

	for (i = 0; i < nmsgs; i++)
	{
		struct i2c_msg_linux zephyr_msg = {
			.addr = msgs[i].addr,
			.flags = msgs[i].flags & I2C_MSG_READ ? I2C_MSG_READ : 0,
			.buf = msgs[i].buf,
			.len = msgs[i].len};
		if (zephyr_msg.flags & I2C_MSG_READ)
		{
			res = i2c_read(dev, zephyr_msg.buf, zephyr_msg.len, zephyr_msg.addr);
		}
		else
		{
			res = i2c_write(dev, zephyr_msg.buf, zephyr_msg.len, zephyr_msg.addr);
		}
		if (res < 0)
		{
			break;
		}
		nmsgs_sent++;
	}
	LOG_ERROR("i2cdev_ioctl_rdwr successful----\n");
	return nmsgs_sent;
}

static unsigned long i2c_get_functionality(const struct device *dev)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_BLOCK_DATA;
}

struct smbus_device_info {
	const char* i2c_name;
	const char* smbus_name;
};

static struct smbus_device_info smbus_devices[] = {
	{"i2c-1", "smbus1"},
	{"i2c-2", "smbus2"},
	{"i2c-3", "smbus3"},
	{"i2c-4", "smbus4"},
	{"i2c-5", "smbus5"},
	{"i2c-6", "smbus6"},
	{"i2c-7", "smbus7"},
	{"i2c-8", "smbus8"},
	{"i2c-9", "smbus9"},
	{"i2c-10", "smbus10"},
	{"i2c-11", "smbus11"},
	{"i2c-12", "smbus12"},
	{"i2c-13", "smbus13"},
	{"i2c-14", "smbus14"},
	{"i2c-15", "smbus15"},
	{"i2c-16", "smbus16"}
};

static const char* get_smbus_name(const struct device *dev){
	const char *i2c_name = dev->name;
	int count = ARRAY_SIZE(smbus_devices);

	for(int i = 0; i < count; i++) {
		if(strcmp(smbus_devices[i].i2c_name, i2c_name) == 0){
            return smbus_devices[i].smbus_name;
		}
	}

	LOG_ERROR("No mapping found for device: %s\n", i2c_name);
    return NULL;
}

static int i2cdev_ioctl_smbus(struct i2c_device_info *client, u8 read_write,
                              u8 command, u32 size,
                              union i2c_smbus_data *data)
{
	int res = 0;
    if ((size != I2C_SMBUS_BYTE) && (size != I2C_SMBUS_QUICK) &&
        (size != I2C_SMBUS_BYTE_DATA) && (size != I2C_SMBUS_WORD_DATA) &&
        (size != I2C_SMBUS_PROC_CALL) && (size != I2C_SMBUS_BLOCK_DATA) &&
        (size != I2C_SMBUS_I2C_BLOCK_BROKEN) &&
        (size != I2C_SMBUS_I2C_BLOCK_DATA) &&
        (size != I2C_SMBUS_BLOCK_PROC_CALL)) {
        LOG_ERROR("size out of range (%x) in ioctl I2C_SMBUS.\n", size);
        return -EINVAL;
    }
    if ((read_write != I2C_SMBUS_READ) && (read_write != I2C_SMBUS_WRITE)) {
        LOG_ERROR("read_write out of range (%x) in ioctl I2C_SMBUS.\n", read_write);
        return -EINVAL;
    }

	//获取smbus dev信息
	const char* smbusName = get_smbus_name(client->dev);
	if (smbusName == NULL)
	{
		return -EINVAL;
	}
	const struct device *smbus_dev = device_get_binding(smbusName);
	if (!smbus_dev) {
		LOG_ERROR("SMBus: Device %s not found", smbusName);
		return -ENODEV;
	}

    switch (size) {
    case I2C_SMBUS_QUICK:
        res = smbus_quick(smbus_dev, client->addr, read_write);
        break;
    case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_WRITE) {
            res = smbus_byte_write(smbus_dev, client->addr, command);
        } else {
            res = smbus_byte_read(smbus_dev, client->addr, &data->byte);
        }
        
		// LOG_ERROR("SMBus: data->byte = %d", data->byte);
        break;
    case I2C_SMBUS_BYTE_DATA:
        if (read_write == I2C_SMBUS_WRITE) {
            res = smbus_byte_data_write(smbus_dev, client->addr, command, data->byte);
        } else {
            res = smbus_byte_data_read(smbus_dev, client->addr, command, &data->byte);
        }
        break;
    case I2C_SMBUS_WORD_DATA:
        if (read_write == I2C_SMBUS_WRITE) {
            res = smbus_word_data_write(smbus_dev, client->addr, command, data->word);
        } else {
            res = smbus_word_data_read(smbus_dev, client->addr, command, &data->word);
        }
        break;
    // case I2C_SMBUS_PROC_CALL:
    //     res = smbus_pcall(smbus_dev, client->addr, command, data->word, &data->word);
    //     if (res >= 0) {
    //         data->word = (u16)res;
    //     }
    //     break;
    case I2C_SMBUS_BLOCK_DATA:
    case I2C_SMBUS_I2C_BLOCK_DATA:
		case_I2C_SMBUS_I2C_BLOCK_DATA:
		// 需要基于i2ctransfer实现
        if (read_write == I2C_SMBUS_WRITE) {
            res = smbus_block_write(smbus_dev, client->addr, command, data->block[0], &data->block[1]);
        } else {
            res = smbus_block_read(smbus_dev, client->addr, command, &data->block[0], &data->block[1]);
        }
        break;
    // case I2C_SMBUS_BLOCK_PROC_CALL:
    //     res = smbus_block_pcall(smbus_dev, client->addr, command, data->block[0], &data->block[1]);
    //     if (res >= 0) {
    //         data->block[0] = (u8)res;
    //     }
    //     break;
    case I2C_SMBUS_I2C_BLOCK_BROKEN:
        // Convert old I2C block commands to the new convention.
        size = I2C_SMBUS_I2C_BLOCK_DATA;
        if (read_write == I2C_SMBUS_READ) {
            data->block[0] = I2C_SMBUS_BLOCK_MAX;
        }
        goto case_I2C_SMBUS_I2C_BLOCK_DATA;
    default:
        LOG_ERROR("Unsupported SMBus size (%x) in ioctl I2C_SMBUS.\n", size);
        return -EINVAL;
    }

    return res;
}

static int i2cdev_ioctl(struct fs_file_t *zfp, unsigned long request, va_list args)
{
	// LOG_ERROR("i2cdev_ioctl start----\n");
	struct i2c_device_info *client = zfp->filep;

	if (!client)
		return -EINVAL;

	switch (request)
	{
	case I2C_SLAVE:
	case I2C_SLAVE_FORCE:
	
		client->addr = (uint16_t)va_arg(args, uint32_t);
		// LOG_ERROR("Set address to 0x%02X", client->addr);
		return 0;
	case ZFD_IOCTL_SET_LOCK:
		// LOG_ERROR("ioctl ZFD_IOCTL_SET_LOCK----\n");
		return 0;
	case I2C_PEC:
		/*
		 * Setting the PEC flag here won't affect kernel drivers,
		 * which will be using the i2c_client node registered with
		 * the driver model core.  Likewise, when that client has
		 * the PEC flag already set, the i2c-dev driver won't see
		 * (or use) this setting.
		 */
		int arg = va_arg(args, int);
		if (arg)
			client->flags |= I2C_CLIENT_PEC;
		else
			client->flags &= ~I2C_CLIENT_PEC;
		return 0;
	case I2C_FUNCS:
		 /* 获取第一个参数，类型为unsigned long* */
        unsigned long *funcs = va_arg(args, unsigned long *);
		/* 设置支持的功能 */
		*funcs = i2c_get_functionality(client->dev);
		return 0;
	case I2C_RDWR:
	{
		LOG_ERROR("ioctl I2C_RDWR----\n");
		
		// rdwr_arg = va_arg(args, struct i2c_rdwr_ioctl_data);
		struct i2c_rdwr_ioctl_data *rdwr_arg_ptr = va_arg(args, struct i2c_rdwr_ioctl_data *);
		if (!rdwr_arg_ptr) {
            return -EINVAL;
        }
        struct i2c_rdwr_ioctl_data rdwr_arg = *rdwr_arg_ptr;
		// memcpy(&rdwr_arg, (struct i2c_rdwr_ioctl_data*)args, sizeof(rdwr_arg));

		// LOG_ERROR("rdwr_arg.msgs = %s, rdwr_arg.nmsgs=%d----\n", rdwr_arg.msgs[0].buf, rdwr_arg.nmsgs);

		if (!rdwr_arg.msgs || rdwr_arg.nmsgs == 0)
			return -EINVAL;

		/*
		 * Put an arbitrary limit on the number of messages that can
		 * be sent at once
		 */
		if (rdwr_arg.nmsgs > I2C_RDWR_IOCTL_MAX_MSGS)
			return -EINVAL;

		size_t total_size = rdwr_arg.nmsgs * sizeof(struct i2c_msg_linux);
		struct i2c_msg_linux *rdwr_pa = malloc(total_size);
		if (rdwr_pa == NULL)
		{
			return -ENOMEM;
		}
		memcpy(rdwr_pa, rdwr_arg.msgs, total_size);

		int result = i2cdev_ioctl_rdwr(client->dev, rdwr_arg.nmsgs, rdwr_pa);

		free(rdwr_pa);

		return result;
	}
	case I2C_SMBUS: 
	{
		struct i2c_smbus_ioctl_data *data_arg_ptr = va_arg(args, struct i2c_smbus_ioctl_data *);
		if (!data_arg_ptr) {
            return -EINVAL;
        }
        struct i2c_smbus_ioctl_data data_arg = *data_arg_ptr;
		return i2cdev_ioctl_smbus(client, data_arg.read_write,
					  data_arg.command, data_arg.size,
					  data_arg.data);
	}
	default:
		return -ENOTTY;
	}
}

static int i2cdev_mount(struct fs_mount_t *mountp)
{
	// 检查挂载点是否为空
	if (mountp->mnt_point == NULL)
	{
		LOG_ERROR("Invalid mount point\n");
		return -EINVAL;
	}

	// 检查 I2C 设备是否就绪
	for (int i = 0; i < I2C_NUM; i++)
	{
		if (!device_is_ready(i2c_devices[i].dev))
		{
			LOG_ERROR("I2C device %d not ready\n", i);
			return -ENODEV;
		}
	}

	// LOG_ERROR("I2C device mounted successfully\n");
	return 0;
}
static int i2cdev_unmount(struct fs_mount_t *mountp)
{
	if (mountp->mnt_point == NULL)
	{
		LOG_ERROR("Invalid mount point\n");
		return -EINVAL;
	}

	mountp->mnt_point = NULL;
	LOG_ERROR("I2C device unmounted successfully\n");
	return 0;
}

static int i2cdev_opendir(struct fs_dir_t *dp, const char *path)
{
	if (strcmp(path, I2C_MNT_POINT) != 0)
	{
		LOG_ERROR("Invalid directory path\n");
		return -EINVAL;
	}

	dp->dirp = &i2c_index;

	// LOG_ERROR("I2C device opendir successfully\n");
	return 0;
}

static int i2cdev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
	int *index = dp->dirp;

	if (*index < I2C_NUM)
	{
		// 构造文件条目信息
		snprintf(entry->name, sizeof(entry->name), "%s", i2c_devices[*index].label);
		entry->type = FS_DIR_ENTRY_FILE;
		entry->size = 0; // I2C 设备没有文件大小
		(*index)++;
		return 0;
	}

	// 没有更多条目
	memset(entry, 0, sizeof(struct fs_dirent));
	// LOG_ERROR("No more I2C devices to list\n");
	return 0;
}

static int i2cdev_closedir(struct fs_dir_t *dp)
{
	if (dp->dirp == NULL)
	{
		LOG_ERROR("Error: NULL pointer passed to i2cdev_closedir\n");
		return -EINVAL;
	}

	i2c_index = 0;
	dp->dirp = NULL;

	LOG_ERROR("I2C device closedir successfully\n");
	return 0;
}

static int i2cdev_stat(struct fs_mount_t *mountp,
					   const char *path, struct fs_dirent *entry)
{
	// 检查路径是否为挂载点
	if (strcmp(path, I2C_MNT_POINT) == 0)
	{
		// 挂载点是目录
		entry->type = FS_DIR_ENTRY_DIR;
		entry->size = 0;
	}
	else
	{
		// 检查是否为 I2C 设备路径
		uint8_t minor;
		if (sscanf(path, "i2c-%hhu", &minor) == 1 && minor < I2C_NUM)
		{
			// 是 I2C 设备
			entry->type = FS_DIR_ENTRY_FILE;
			entry->size = 0;
		}
		else
		{
			LOG_ERROR("Invalid path\n");
			return -EINVAL;
		}
	}

	LOG_ERROR("I2C device stat successfully\n");
	return 0;
}

/* File system interface */
static const struct fs_file_system_t i2cdev_fs = {
	.mount = i2cdev_mount,
	.unmount = i2cdev_unmount,
	.open = i2cdev_open,
	.close = i2cdev_close,
	.read = i2cdev_read,   // 从 I2C 设备中读取数据
	.write = i2cdev_write, // 将用户空间的数据写入到 I2C 设备
	.lseek = NULL,
	.ioctl = i2cdev_ioctl,		 // 增加ioctl
	.opendir = i2cdev_opendir,	 // fs ls访问
	.readdir = i2cdev_readdir,	 // fs ls访问
	.closedir = i2cdev_closedir, // fs ls访问
	.stat = i2cdev_stat,		 // fs cd访问
};

static int i2c_dev_init(void)
{
	LOG_ERROR("i2c /dev entries driver\n");

	LOG_ERROR("i2c_num = %d\n", I2C_NUM);

	// 输出i2c设备name和label信息
	for (int i=0; i < I2C_NUM; i++){
		if (i2c_devices[i].dev->name != NULL){
			LOG_ERROR("i2c_devices[%d].dev->name = %s\n", i, i2c_devices[i].dev->name);
		}
		if (i2c_devices[i].label != NULL){
			LOG_ERROR("i2c_devices[%d].label = %s\n", i, i2c_devices[i].label);
		}
	}

	i2cfs_mnt.mnt_point = I2C_MNT_POINT;
	LOG_ERROR("i2cfs_mnt: type: %d, mnt_point: %s, fs_data: %p\n", i2cfs_mnt.type, i2cfs_mnt.mnt_point, i2cfs_mnt.fs_data);

	int rc = fs_mount(&i2cfs_mnt);
	if (rc != 0)
	{
		LOG_ERROR("Error mounting as i2cfs: %d", rc);
		i2cfs_mnt.mnt_point = NULL;
		return rc;
	}

	return 0;
}

static int i2c_init(void)
{
	LOG_ERROR("i2c_init----------start\n");

	int rc = fs_register(FS_I2C, &i2cdev_fs);
	if (rc < 0)
	{
		LOG_ERROR("i2cfs register error (%d)\n", rc);
	}
	else
	{
		LOG_ERROR("i2cfs fs registered\n");
	}

	rc = i2c_dev_init();
	if (rc != 0)
	{
		LOG_ERROR("i2cfs init failed\n");
		return rc;
	}

	LOG_ERROR("i2c_init----------end\n");
	return rc;
}

// void i2c_main()
// {
// 	i2c_dev_init();
// }

// 系统初始化
SYS_INIT(i2c_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
