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
	off_t offset;
	atomic_t opened; // 原子操作
};

#if defined(CONFIG_BOARD_ALIMOC30) || defined(CONFIG_BOARD_ALIMOC31)
#define EEPROM_MNT_POINT "/sys/bus/i2c/devices/2-0053"
#else defined(CONFIG_BOARD_ALISMC)
#define EEPROM_MNT_POINT "/sys/bus/i2c/devices/14-0050"
#endif
