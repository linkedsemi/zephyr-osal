#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <stdio.h>

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/types.h>

#pragma once

#define HWMON_MNT_POINT "/sys/class/hwmon"

enum hwmon_class
{
	HWMON_FAN,
	HWMON_TEMP,
	HWMON_ADC,
	HWMON_SENSOR,
    HWMON_PMBUS,
    HWMON_MOC_FPGA,
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

// PMBUS设备默认值配置结构体定义
struct pmbus_default_values {
    const char *attr_name;      // 属性名称
    const char *default_value;  // 默认值字符串
};

// 在文件顶部添加枚举定义
enum pmbus_attrs {
    // PMBUS_CURR_ALARM,
    // PMBUS_CURR_INPUT,
    // PMBUS_CURR_LABEL,
    // PMBUS_CURR_MAX,
    // PMBUS_IN1_BEEP,
    // PMBUS_IN1_FAULT,
    PMBUS_IN1_INPUT,
    PMBUS_IN1_LABEL,
    // PMBUS_IN1_MAX,
    // PMBUS_IN1_RATED_MAX,
    // PMBUS_IN1_MAX_ALARM,
    // PMBUS_IN1_MIN,
    // PMBUS_IN1_RATED_MIN,
    // PMBUS_IN1_MIN_ALARM,
    // PMBUS_IN2_ALARM,
    // PMBUS_IN2_INPUT,
    // PMBUS_IN2_LABEL,
    // PMBUS_IN3_BEEP,
    // PMBUS_IN3_FAULT,
    // PMBUS_IN3_INPUT,
    // PMBUS_IN3_LABEL,
    // PMBUS_IN3_MAX,
    // PMBUS_IN3_RATED_MAX,
    // PMBUS_IN3_MAX_ALARM,
    // PMBUS_IN3_MIN,
    // PMBUS_IN3_RATED_MIN,
    // PMBUS_IN3_MIN_ALARM,
    // PMBUS_POWER_ALARM,
    PMBUS_POWER_INPUT,
    PMBUS_POWER_LABEL,
    // PMBUS_POWER_RATED_MIN,
    // PMBUS_POWER_RATED_MAX,
    // PMBUS_TEMP_CRIT,
    // PMBUS_TEMP_CRIT_ALARM,
    PMBUS_TEMP_INPUT,
    PMBUS_TEMP_LABEL,
    // PMBUS_TEMP_MAX,
    // PMBUS_TEMP_MAX_ALARM,
    // PMBUS_TEMP_RATED_MIN,
    // PMBUS_TEMP_RATED_MAX,
    PMBUS_NAME,
    PMBUS_ATTR_COUNT
};

// 完整的 PMBUS 属性名称结构体定义
static const char *const pmbus_attr_names[PMBUS_ATTR_COUNT] = {
    // [PMBUS_CURR_ALARM] = "curr1_alarm",
    // [PMBUS_CURR_INPUT] = "curr1_input",
    // [PMBUS_CURR_LABEL] = "curr1_label",
    // [PMBUS_CURR_MAX] = "curr1_max",
    // [PMBUS_IN1_BEEP] = "in1_beep",
    // [PMBUS_IN1_FAULT] = "in1_fault",
    [PMBUS_IN1_INPUT] = "in1_input",
    [PMBUS_IN1_LABEL] = "in1_label",
    // [PMBUS_IN1_MAX] = "in1_max",
    // [PMBUS_IN1_RATED_MAX] = "in1_rated_max",
    // [PMBUS_IN1_MAX_ALARM] = "in1_max_alarm",
    // [PMBUS_IN1_MIN] = "in1_min",
    // [PMBUS_IN1_RATED_MIN] = "in1_rated_min",
    // [PMBUS_IN1_MIN_ALARM] = "in1_min_alarm",
    // [PMBUS_IN2_ALARM] = "in2_alarm",
    // [PMBUS_IN2_INPUT] = "in2_input",
    // [PMBUS_IN2_LABEL] = "in2_label",
    // [PMBUS_IN3_BEEP] = "in3_beep",
    // [PMBUS_IN3_FAULT] = "in3_fault",
    // [PMBUS_IN3_INPUT] = "in3_input",
    // [PMBUS_IN3_LABEL] = "in3_label",
    // [PMBUS_IN3_MAX] = "in3_max",
    // [PMBUS_IN3_RATED_MAX] = "in3_rated_max",
    // [PMBUS_IN3_MAX_ALARM] = "in3_max_alarm",
    // [PMBUS_IN3_MIN] = "in3_min",
    // [PMBUS_IN3_RATED_MIN] = "in3_rated_min",
    // [PMBUS_IN3_MIN_ALARM] = "in3_min_alarm",
    // [PMBUS_POWER_ALARM] = "power1_alarm",
    // [PMBUS_POWER_RATED_MIN] = "power1_rated_min",
    // [PMBUS_POWER_RATED_MAX] = "power1_rated_max",
    [PMBUS_POWER_INPUT] = "power1_input",
    [PMBUS_POWER_LABEL] = "power1_label",
    // [PMBUS_TEMP_CRIT] = "temp1_crit",
    // [PMBUS_TEMP_CRIT_ALARM] = "temp1_crit_alarm",
    [PMBUS_TEMP_INPUT] = "temp1_input",
    [PMBUS_TEMP_LABEL] = "temp1_label",
    // [PMBUS_TEMP_MAX] = "temp1_max",
    // [PMBUS_TEMP_MAX_ALARM] = "temp1_max_alarm",
    // [PMBUS_TEMP_RATED_MIN] = "temp1_rated_min",
    // [PMBUS_TEMP_RATED_MAX] = "temp1_rated_max",
    [PMBUS_NAME] = "name"
};

/**
 * @brief 获取HWMON设备配置
 *
 * @param dev HWMON设备
 * @return 指向配置结构体的指针，如果失败则返回NULL
 */
const struct hwmon_ls_config *hwmon_ls_get_config(const struct device *dev);
