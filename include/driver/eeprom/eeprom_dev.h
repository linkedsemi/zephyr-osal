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
	const char *label; // 存储设备名称，如"2-0053"
};

#define EEPROM_MNT_POINT "/sys/bus/i2c/devices"

// 上下文结构定义
enum eeprom_dir_type {
    ROOT_DIR,    // 根目录：列出所有设备
    DEVICE_DIR   // 设备目录：列出设备属性
};

struct eeprom_dir_ctx {
    enum eeprom_dir_type type;

    union {
        // 根目录上下文
        struct {
            size_t index;  // 当前设备索引
        };

        // 设备目录上下文
        struct {
            size_t devid;    // 设备ID
            size_t attr_idx; // 当前属性索引
        } dev;
    };
};
