#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <stdio.h>

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/types.h>

struct eeprom_device_info
{
	const struct device *dev;
	atomic_t opened; // 原子操作
};

#define EEPROM_MNT_POINT "/sys/bus/i2c/device/2-0055"