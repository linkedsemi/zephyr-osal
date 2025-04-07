#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <stdio.h>

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/types.h>

struct i2c_device_info
{
	const struct device *dev;
	const char *label;
	uint16_t addr;
	atomic_t opened; // 原子操作
	uint8_t flags;
};

#define I2C_MNT_POINT "/dev"