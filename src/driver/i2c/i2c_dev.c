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
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>

#define I2C_SLAVE 0x0703 /* Use this slave address */

#define I2C_SLAVE_FORCE 0x0706 /* Use this slave address, even if it \
				  is already in use by a driver! */

#define I2C_RDWR 0x0707 /* Combined R/W transfer (one STOP only) */

#define CONFIG_I2C_DEV_MAX_SIZE 8192

#define MAX_I2C_DEVICES 14 /* The maximum number of I2C devices */

#define I2C_MNT_POINT "/dev"

#define DT_DRV_COMPAT linkedsemi_ls_i2c

#define I2C_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(I2C_NUM <= MAX_I2C_DEVICES,
			 "Exceeded maximum I2C device count");

struct i2c_device_info
{
	const struct device *dev;
	const char* label;
	uint16_t addr;
	uint8_t opened;  //原子操作
};

#define I2C_DEVICE_PROCESS(inst)                            \
    {                                                       \
        .dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \
        .label = DT_INST_PROP(inst, label),                       \
        .addr = 0,                                          \
        .opened = 0                                         \
    },

struct i2c_device_info i2c_devices[] = {
	DT_INST_FOREACH_STATUS_OKAY(I2C_DEVICE_PROCESS)};

// struct i2c_device_info i2c_devices[I2C_NUM];


static int i2c_index = 0; // 当前遍历的 I2C 设备索引

static struct fs_mount_t i2cfs_mnt = {
	.type = FS_I2C,
	.fs_data = &i2c_devices,
};

static const char* getFileName(const char* filePath) {
    const char* lastSlash = strrchr(filePath, '/');
    if (lastSlash != NULL) {
        return lastSlash + 1;
    }
    return filePath;
}

static int i2cdev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
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

	if (client == NULL) {
        printk("Error: I2C device with name '%s' not found\n", file_name);
        return -ENODEV;
    }

	if (client->opened) {
		printk("device name is opened");
		return -EINVAL;
	}

	client->opened = 1;

	if (client->dev || !device_is_ready(client->dev))
	{
		printk("Error: I2C device with name '%s' is not ready\n", file_name);
		client->opened = 0;
		return -ENODEV;
	}

	if (zfp->mp == NULL) {
        printk("Error: File system mount point is NULL\n");
        client->opened = 0;
        return -EINVAL;
    }

	zfp->filep = client;
	zfp->mp = &i2cfs_mnt;

	printk("I2C device with name '%s' opened successfully\n", file_name);
	return 0;
}

static int i2cdev_close(struct fs_file_t *zfp)
{
	if (zfp->mp)
	{
		zfp->mp = NULL;
		printk("Closed i2cfs mount");
	}

	return 0;
}

static ssize_t i2cdev_read(struct fs_file_t *zfp, void *ptr, size_t size)
{
	struct i2c_device_info *client = zfp->filep;

	if (client == NULL)
	{
		printk("No client context");
		return -EINVAL;
	}

	if (size > CONFIG_I2C_DEV_MAX_SIZE)
	{
		size = CONFIG_I2C_DEV_MAX_SIZE;
	}

	int ret = i2c_read(client->dev, (uint8_t *)ptr, size, client->addr);
	if (ret != 0)
	{
		printk("Read failed: %d (addr=0x%02X)", ret, client->addr);
		return ret;
	}

	printk("Read %zu bytes from I2C 0x%02X", size, client->addr);
	return size;
}

static ssize_t i2cdev_write(struct fs_file_t *zfp, const void *ptr, size_t size)
{
	struct i2c_device_info *client = zfp->filep;
	const uint8_t *write_buf = (const uint8_t *)ptr;

	if (client == NULL)
	{
		printk("No client context");
		return -EINVAL;
	}

	if (ptr == NULL)
	{
		printk("Null buffer pointer");
		return -EFAULT;
	}

	if (size > CONFIG_I2C_DEV_MAX_SIZE)
	{
		size = CONFIG_I2C_DEV_MAX_SIZE;
		printk("Truncated write size to %d", CONFIG_I2C_DEV_MAX_SIZE);
	}

	int ret = i2c_write(client->dev, write_buf, size, client->addr);
	if (ret != 0)
	{
		printk("Write failed: %d (addr=0x%02X, len=%zu)",
			   ret, client->addr, size);
		return ret;
	}

	printk("I2C writing %zu bytes to device at address 0x%x", size, client->addr);
	return size;
}
struct i2c_msg_linux
{
	uint16_t addr;
	uint8_t flags;
	uint32_t len;
	uint8_t *buf;
};
/* This is the structure as used in the I2C_RDWR ioctl call */
struct i2c_rdwr_ioctl_data
{
	struct i2c_msg_linux *msgs; /* pointers to i2c_msgs */
	uint32_t nmsgs;				/* number of i2c_msgs */
};

#define I2C_RDWR_IOCTL_MAX_MSGS 42

#define I2C_SMBUS_BLOCK_MAX 32 /* As specified in SMBus standard */

static int i2cdev_ioctl_rdwr(const struct device *dev, unsigned nmsgs,
							 struct i2c_msg_linux *msgs)
{
	int i, res;

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
	}

	return res;
}

static int i2cdev_ioctl(struct fs_file_t *zfp, unsigned long request, va_list args)
{
	struct i2c_device_info *client = zfp->filep;

	if (!client)
		return -EINVAL;

	switch (request)
	{
	case I2C_SLAVE:
	case I2C_SLAVE_FORCE:
		client->addr = (uint16_t)(*(uint32_t *)args);
		printf("Set address to 0x%02X", client->addr);
		return 0;
	case I2C_RDWR:
	{
		struct i2c_rdwr_ioctl_data rdwr_arg;
		struct i2c_msg_linux *rdwr_pa;

		memcpy(&rdwr_arg, args, sizeof(rdwr_arg));

		if (!rdwr_arg.msgs || rdwr_arg.nmsgs == 0)
			return -EINVAL;

		/*
		 * Put an arbitrary limit on the number of messages that can
		 * be sent at once
		 */
		if (rdwr_arg.nmsgs > I2C_RDWR_IOCTL_MAX_MSGS)
			return -EINVAL;

		size_t total_size = rdwr_arg.nmsgs * sizeof(struct i2c_msg_linux);
		rdwr_pa = malloc(total_size);
		if (rdwr_pa == NULL)
		{
			return -ENOMEM;
		}
		memcpy(rdwr_pa, rdwr_arg.msgs, total_size);

		int result = i2cdev_ioctl_rdwr(client->dev, rdwr_arg.nmsgs, rdwr_pa);

		free(rdwr_pa);

		return result;
	}
	default:
		return -ENOTTY;
	}
}

static int i2cdev_mount(struct fs_mount_t *mountp)
{
	// 检查挂载点是否为空
    if (mountp->mnt_point == NULL) {
        printk("Invalid mount point\n");
        return -EINVAL;
    }

    // 检查 I2C 设备是否就绪
    // for (int i = 0; i < I2C_NUM; i++) {
    //     if (!i2c_devices[i].dev || !device_is_ready(i2c_devices[i].dev)) {
    //         printk("I2C device %d not ready\n", i);
    //         return -ENODEV;
    //     }
    // }

	printk("I2C device mounted successfully\n");
	return 0;
}
static int i2cdev_unmount(struct fs_mount_t *mountp)
{
    if (mountp->mnt_point == NULL) {
        printk("Invalid mount point\n");
        return -EINVAL;
    }

	mountp->mnt_point = NULL;
	printk("I2C device unmounted successfully\n");
	return 0;
}

static int i2cdev_opendir(struct fs_dir_t *dp, const char *path)
{
    if (strcmp(path, I2C_MNT_POINT) != 0) {
        printk("Invalid directory path\n");
        return -EINVAL;
    }

    i2c_index = 0;
    dp->dirp = (void *)(intptr_t)i2c_index;

	printk("I2C device opendir successfully\n");
	return 0;
}

static int i2cdev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
	i2c_index = (int)(intptr_t)dp->dirp;

    if (i2c_index < I2C_NUM) {
        // 构造文件条目信息
        snprintf(entry->name, sizeof(entry->name), "%s", i2c_devices[i2c_index].label);
        entry->type = FS_DIR_ENTRY_FILE;
        entry->size = 0; // I2C 设备没有文件大小
        i2c_index++;
		dp->dirp = (void *)(intptr_t)i2c_index;
        return 0;
    }

    // 没有更多条目
    memset(entry, 0, sizeof(struct fs_dirent));
	printk("No more I2C devices to list\n");
    return 0;
}

static int i2cdev_closedir(struct fs_dir_t *dp)
{
	if (dp->dirp == NULL) {
        printk("Error: NULL pointer passed to i2cdev_closedir\n");
        return -EINVAL;
    }

	i2c_index = 0;
	dp->dirp = NULL;

	printk("I2C device closedir successfully\n");
	return 0;
}

static int i2cdev_stat(struct fs_mount_t *mountp,
	const char *path, struct fs_dirent *entry)
{
	// 检查路径是否为挂载点
    if (strcmp(path, I2C_MNT_POINT) == 0) {
        // 挂载点是目录
        entry->type = FS_DIR_ENTRY_DIR;
        entry->size = 0;
    } else {
        // 检查是否为 I2C 设备路径
        uint8_t minor;
        if (sscanf(path, "i2c-%hhu", &minor) == 1 && minor < I2C_NUM) {
            // 是 I2C 设备
            entry->type = FS_DIR_ENTRY_FILE;
            entry->size = 0;
        } else {
            printk("Invalid path\n");
            return -EINVAL;
        }
    }

	printk("I2C device stat successfully\n");
	return 0;
}

/* File system interface */
static const struct fs_file_system_t i2cdev_fs = {
	.mount = i2cdev_mount,
	.unmount = i2cdev_unmount,
	.open = i2cdev_open,
	.close = i2cdev_close,
	.read = i2cdev_read,   // 从 I2C 设备中读取数据并返回给用户空间
	.write = i2cdev_write, // 将用户空间的数据写入到 I2C 设备
	.lseek = NULL,
	.ioctl = i2cdev_ioctl, // 增加ioctl
	.opendir = i2cdev_opendir, // fs ls访问
	.readdir = i2cdev_readdir, // fs ls访问
	.closedir = i2cdev_closedir, // fs ls访问
	.stat = i2cdev_stat, // fs cd访问
};

static int i2c_dev_init(void)
{
	printk("i2c /dev entries driver\n");

	printk("i2c_num = %d\n", I2C_NUM);

	printk("i2c_devices[0].dev->name = %s\n", i2c_devices[0].dev->name);

	printk("i2c_devices[0].label = %s\n", i2c_devices[0].label);

	i2cfs_mnt.mnt_point = I2C_MNT_POINT;
	printk("i2cfs_mnt: type: %d, mnt_point: %s, fs_data: %p\n", i2cfs_mnt.type, i2cfs_mnt.mnt_point, i2cfs_mnt.fs_data);

	int rc = fs_mount(&i2cfs_mnt);
	if (rc != 0)
	{
		printk("Error mounting as i2cfs: %d", rc);
		i2cfs_mnt.mnt_point = NULL;
		return rc;
	}

	return 0;
}

static int i2c_init(void)
{
	printk("i2c_init----------start\n");

	int rc = fs_register(FS_I2C, &i2cdev_fs);
	if (rc < 0)
	{
		printk("i2cfs register error (%d)\n", rc);
	}
	else
	{
		printk("i2cfs fs registered\n");
	}

	rc = i2c_dev_init();
	if (rc != 0)
	{
		printk("i2cfs init failed\n");
		return rc;
	}

	printk("i2c_init----------end\n");
	return rc;
}

void i2c_main()
{
	i2c_dev_init();
}

// 系统初始化
SYS_INIT(i2c_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
