#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <stdio.h>

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/types.h>

#define HWMON_MNT_POINT "/sys/class/hwmon"

enum hwmon_class
{
	HWMON_FAN,
	HWMON_TEMP,
	HWMON_ADC,
	HWMON_SENSOR,
};

struct hwmon_ref {
	const struct device *dev;
	uint8_t channel;
};

struct hwmon_ls_config {
	const struct hwmon_ref *in;
	const struct hwmon_ref *out;
	uint8_t in_num;
	uint8_t out_num;
	enum hwmon_class class;
};

/* hwmon device class */
struct hwmon_device_info {
  const struct device *dev; // sensor device handle
  const char *label;        // sensor labels in the device tree
};

struct hwmon_file_handle {
	struct hwmon_device_info *device;
	char attr_name[32];
	atomic_t opened;
    int nonblocking;          // 添加非阻塞标志
};

// 上下文结构定义
enum hwmon_dir_type {
    ROOT_DIR,    // 根目录：列出所有设备
    DEVICE_DIR   // 设备目录：列出设备属性
};

struct hwmon_dir_ctx {
    enum hwmon_dir_type type;
    
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

/**
 * @brief 获取HWMON设备配置
 * 
 * @param dev HWMON设备
 * @return 指向配置结构体的指针，如果失败则返回NULL
 */
const struct hwmon_ls_config *hwmon_ls_get_config(const struct device *dev);