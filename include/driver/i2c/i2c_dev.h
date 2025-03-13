#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <stdio.h>

// 定义存储 I2C 设备信息的结构体
typedef struct
{
	uint16_t addr;			  // 从设备地址
	const struct device *dev; // 设备指针
} i2c_device_info;

/* NOTE: Slave address is 7 or 10 bits, but 10-bit addresses
 * are NOT supported! (due to code brokenness)
 */
#define I2C_SLAVE 0x0703 /* Use this slave address */

#define MAX_I2C_DEVICES 14

#define CONFIG_I2C_DEV_MAX_SIZE 8192