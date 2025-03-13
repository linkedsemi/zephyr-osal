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

#define I2C_MNT_PONINT "/dev"

#define DT_DRV_COMPAT linkedsemi_ls_i2c

#define I2C_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(I2C_NUM <= MAX_I2C_DEVICES,
			 "Exceeded maximum I2C device count");

struct i2c_device_info
{
	uint16_t addr;
	const struct device *dev;
};

static struct fs_mount_t i2cfs_mnt[I2C_NUM];

#define I2C_DEVICE_PROCESS(inst)                            \
	{                                                       \
		.dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \
		.addr = 0},

struct i2c_device_info i2c_devices[] = {
	DT_INST_FOREACH_STATUS_OKAY(I2C_DEVICE_PROCESS)};

// struct i2c_device_info i2c_devices[I2C_NUM];

static int i2cdev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
	struct fs_mount_t *i2cfs;
	uint8_t minor;

	if (sscanf(file_name, "i2c-%hhu", &minor) != 1)
	{
		printk("Invalid device name format");
		return -EINVAL;
	}

	if (minor >= I2C_NUM)
	{
		printk("Invalid device name format");
		return -EINVAL;
	}
	i2cfs = &i2cfs_mnt[minor];

	if (!i2c_devices[minor].dev || !device_is_ready(i2c_devices[minor].dev))
	{
		printk("I2C device not ready");
		return -ENODEV;
	}

	if (zfp->mp != NULL)
	{
		i2cfs->fs_data = &i2c_devices[minor];
		zfp->mp = i2cfs;
	}
	else
	{
		printk("File system mount point is NULL");
		return -EINVAL;
	}

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
	struct i2c_device_info *client = zfp->mp->fs_data;

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
	struct i2c_device_info *client = zfp->mp->fs_data;
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
	return ret;
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
	struct i2c_device_info *client = zfp->mp->fs_data;

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

/* File system interface */
static const struct fs_file_system_t i2cdev_fs = {
	.open = i2cdev_open,
	.close = i2cdev_close,
	.read = i2cdev_read,   // 从 I2C 设备中读取数据并返回给用户空间
	.write = i2cdev_write, // 将用户空间的数据写入到 I2C 设备
	.lseek = NULL,
	.ioctl = i2cdev_ioctl, // 增加ioctl
};

static int i2c_dev_init(void)
{
	printk("i2c /dev entries driver\n");

	// const struct device *dev = device_get_binding("i2c-1");
	printk("i2c_num = %d\n", I2C_NUM);
	// printk("i2c_name = %s\n", dev->name);

	for (int i = 0; i < I2C_NUM; i++)
	{
		i2cfs_mnt[i].type = FS_I2C;
		i2cfs_mnt[i].mnt_point = I2C_MNT_PONINT;
		i2cfs_mnt[i].fs_data = &i2c_devices[i];

		printk("i2cfs_mnt[%zu]: type: %d, mnt_point: %s, fs_data: %p\n", i, i2cfs_mnt[i].type, i2cfs_mnt[i].mnt_point, i2cfs_mnt[i].fs_data);

		int rc = fs_mount(&i2cfs_mnt[i]);
		if (rc != 0)
		{
			printk("Error mounting as i2cfs: %d", rc);
			k_free((void *)i2cfs_mnt[i].mnt_point);
			i2cfs_mnt[i].mnt_point = NULL;
			return rc;
		}
	}

	return 0;
}

static int i2c_init(void)
{
	printk("i2c_init----------start\n");

	int rc = fs_register(FS_I2C, &i2cdev_fs);
	if (rc < 0) {
		printk("i2cfs register error (%d)\n", rc);
	} else {
		printk("i2cfs fs registered\n");
	}
	
	rc = i2c_dev_init();
	if (rc != 0)
	{
		printk("i2cfs init failed\n");
		return rc;
	}

	// 将i2c文件系统接口注册到 Zephyr 文件系统中
	printk("i2c_init----------end\n");
	return rc;
}

void i2c_main()
{
	i2c_dev_init();
}

// 系统初始化
SYS_INIT(i2c_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
