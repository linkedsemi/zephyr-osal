/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <driver/hwmon/hwmon.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/drivers/adc.h>
#include <ls_hal_adcv2.h>
#include <zephyr/drivers/sensor.h>
#include "driver/hwmon/hwmon_moc_fpga.h"
#include "driver/hwmon/hwmon_pmbus.h"

LOG_MODULE_REGISTER(hwmon_dev, LOG_LEVEL_ERR);

#define CONFIG_HWMON_DEV_MAX_SIZE 8192
#define MAX_HWMON_DEVICES 32
#define MAX_ATTR_NAME_LEN 32

#define DT_DRV_COMPAT linkedsemi_hwmon

#define HWMON_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(HWMON_NUM <= MAX_HWMON_DEVICES,
             "Exceeded maximum HWMON device count");

#define HWMON_DEVICE_PROCESS(inst)                            \
	{                                                       \
		.dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \
		.label = DT_INST_PROP(inst, label)},

static struct hwmon_device_info hwmon_devices[] = {
	DT_INST_FOREACH_STATUS_OKAY(HWMON_DEVICE_PROCESS)
};

static struct fs_mount_t hwmonfs_mnt = {
	.type = FS_HWMON,
	.fs_data = &hwmon_devices,
};

#if defined(CONFIG_BOARD_ALIMOC30) || defined(CONFIG_BOARD_ALIMOC31)
// PMBUS设备的默认初始值配置
static const struct pmbus_default_values pmbus_default_values[] = {
    // 电流相关属性
    // {"curr1_input", "6500\n"},        // 6.5A输入电流 (mA)
    // {"curr1_label", "iout1\n"},       // 输出电流标签
    // // {"curr1_max", "50000\n"},         // 最大电流50A (mA)
    // // {"curr1_alarm", "0\n"},           // 电流告警状态

    // 输入电压相关属性 (in1)
    {"in1_input", "0\n"},         // 12.406V输入电压 (mV)
    {"in1_label", "vin\n"},           // 输入电压标签
    // {"in1_max", "17000\n"},           // 最大输入电压17V (mV)
    // {"in1_rated_max", "17000\n"},     // 额定最大值
    // {"in1_max_alarm", "0\n"},         // 输入电压最大值告警
    // {"in1_min", "3312\n"},            // 最小输入电压3.312V (mV)
    // {"in1_rated_min", "3312\n"},     // 额定最小值
    // {"in1_min_alarm", "0\n"},         // 输入电压最小值告警
    // {"in1_beep", "0\n"},              // 输入电压蜂鸣器
    // {"in1_fault", "0\n"},             // 输入电压故障

    // // 输入电压相关属性 (in2)
    // {"in2_input", "12375\n"},         // 12.375V输入电压 (mV)
    // {"in2_label", "vout1\n"},         // 输出电压标签
    // {"in2_alarm", "0\n"},             // 输出电压告警

    // // 输入电压相关属性 (in3)
    // {"in3_input", "12406\n"},         // 12.406V输入电压 (mV)
    // {"in3_label", "vin1\n"},          // 输出电压标签
    // {"in3_max", "17000\n"},           // 最大输出电压17V (mV)
    // {"in3_rated_max", "17000\n"},     // 额定最大值
    // {"in3_max_alarm", "0\n"},         // 输出电压最大值告警
    // {"in3_min", "3312\n"},            // 最小输出电压3.312V (mV)
    // {"in3_min_alarm", "0\n"},         // 输出电压最小值告警
    // {"in3_rated_min", "3312\n"},     // 额定最小值
    // {"in3_beep", "0\n"},              // 输出电压蜂鸣器
    // {"in3_fault", "0\n"},             // 输出电压故障

    // 功率相关属性
    {"power1_input", "0\n"},   // 80W功耗 (uW)
    {"power1_label", "pin\n"},        // 输入功耗标签
    // {"power1_alarm", "0\n"},          // 功耗告警状态
    // {"power1_rated_min", "0\n"},          // 功耗告警状态
    // {"power1_rated_max", "200000000\n"},          // 功耗告警状态

    // 温度相关属性
    {"temp1_input", "0\n"},        // 2.5°C温度 (milli-degree Celsius)
    {"temp1_label", "temp1\n"},       // 温度标签
    // {"temp1_max", "130000\n"},        // 最大温度130°C (milli-degree Celsius)
    // {"temp1_max_alarm", "0\n"},       // 温度最大值告警
    // {"temp1_crit", "213000\n"},       // 临界温度213°C (milli-degree Celsius)
    // {"temp1_crit_alarm", "0\n"},      // 温度临界值告警
    // {"temp1_rated_min", "0\n"},      // 温度临界值告警
    // {"temp1_rated_max", "130000\n"},      // 温度临界值告警

    // 设备名称
    {"name", "mp5023\n"},            // 设备名称
};
#elif defined(CONFIG_BOARD_ALISMC)

/* Show and Store Functions */
static int zephyr_psu_word_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    uint16_t word;
    int ret = ali_psu_word_show(dev, reg, &word);
    if (ret == 0 && buf != NULL && len != NULL) {
        buf[0] = word & 0xFF;
        buf[1] = (word >> 8) & 0xFF;
        *len = 2;
    }
    return ret;
}

static int zephyr_psu_word_store(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    if (buf == NULL || len == NULL || *len < 2) {
        return -EINVAL;
    }
    uint16_t word = buf[0] | (buf[1] << 8);
    return ali_psu_word_store(dev, reg, word);
}

static int zephyr_psu_byte_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    uint8_t byte;
    int ret = ali_psu_byte_show(dev, reg, &byte);
    if (ret == 0 && buf != NULL && len != NULL) {
        buf[0] = byte;
        *len = 1;
    }
    return ret;
}

static int zephyr_psu_byte_store(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    if (buf == NULL) {
        return -EINVAL;
    }
    return ali_psu_byte_store(dev, reg, buf[0]);
}

static int zephyr_psu_sensor_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    int64_t val;
    int ret = ali_psu_sensor_show(dev, reg, &val);
    if (ret == 0 && buf != NULL && len != NULL) {
        // 将64位值存储到缓冲区中（根据实际需求调整）
        for (int i = 0; i < sizeof(val) && i < *len; i++) {
            buf[i] = (val >> (i * 8)) & 0xFF;
        }
        *len = sizeof(val);
    }
    return ret;
}

static int zephyr_psu_mfr_page_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    uint8_t page;
    int ret = ali_psu_mfr_page_show(dev, &page);
    if (ret == 0 && buf != NULL && len != NULL) {
        buf[0] = page;
        *len = 1;
    }
    return ret;
}

static int zephyr_psu_mfr_page_store(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    if (buf == NULL) {
        return -EINVAL;
    }
    return ali_psu_mfr_page_store(dev, buf[0]);
}

static int zephyr_psu_ac_cycle_store(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    return ali_psu_ac_cycle_store(dev);
}

static int zephyr_psu_block_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    return ali_psu_block_show(dev, reg, buf, len);
}

static int zephyr_psu_fw_version_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    return ali_psu_fw_version_show(dev, reg, buf, len);
}

static int zephyr_psu_bootloader_str_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    return ali_psu_bootloader_str_show(dev, reg, buf, len);
}

static int zephyr_psu_bootloader_str_store(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    if (buf == NULL || len == NULL) {
        return -EINVAL;
    }
    return ali_psu_bootloader_str_store(dev, reg, buf, *len);
}

static int zephyr_psu_bootloader_hex_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    return ali_psu_bootloader_hex_show(dev, reg, buf, len);
}

static int zephyr_psu_bootloader_hex_store(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    if (buf == NULL || len == NULL) {
        return -EINVAL;
    }
    return ali_psu_bootloader_hex_store(dev, reg, buf, *len);
}

static int zephyr_psu_word_his_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    if (buf == NULL) {
        return -EINVAL;
    }
    return ali_psu_word_his_show(dev, reg, buf);
}

static int zephyr_psu_byte_his_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    if (buf == NULL) {
        return -EINVAL;
    }
    return ali_psu_byte_his_show(dev, reg, buf);
}

static int zephyr_psu_sensor_his_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    int64_t val;
    int ret = ali_psu_sensor_his_show(dev, reg, &val);
    if (ret == 0 && buf != NULL && len != NULL) {
        // 将64位值存储到缓冲区中（根据实际需求调整）
        for (int i = 0; i < sizeof(val) && i < *len; i++) {
            buf[i] = (val >> (i * 8)) & 0xFF;
        }
        *len = sizeof(val);
    }
    return ret;
}

static int zephyr_psu_block_hex_his_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    return ali_psu_block_hex_his_show(dev, reg, buf, len);
}

static int zephyr_powerbrick_block_hex_his_show(const struct device *dev, uint8_t reg, uint8_t *buf, uint8_t *len)
{
    return ali_powerbrick_block_hex_his_show(dev, reg, buf, len);
}

static const struct pmbus_default_values pmbus_default_values[] = {
    // 电源控制相关属性
    {"ac_cycle", "0\n", NULL, zephyr_psu_ac_cycle_store, 0},                           // 电源AC周期控制
    {"auto_turn_on_delay", "0\n", zephyr_psu_word_show, zephyr_psu_word_store, auto_turn_on_delay},                 // 自动开机延时

    // 风扇控制相关属性
    {"fan_config_1_2", "0\n", zephyr_psu_byte_show, zephyr_psu_byte_store, fan_config_1_2},                     // 风扇配置
    {"fan_command_1", "0\n", zephyr_psu_byte_show, zephyr_psu_byte_store, fan_command_1},                      // 风扇命令1
    {"fan_command_2", "0\n", zephyr_psu_byte_show, zephyr_psu_byte_store, fan_command_2},                      // 风扇命令2

    // 历史状态相关属性
    {"his_status_word", "0\n", zephyr_psu_word_his_show, NULL, status_word},                    // 历史状态字
    {"his_status_vout", "0\n", zephyr_psu_byte_his_show, NULL, status_vout},                    // 历史输出电压状态
    {"his_status_iout", "0\n", zephyr_psu_byte_his_show, NULL, status_iout},                    // 历史输出电流状态
    {"his_status_input", "0\n", zephyr_psu_byte_his_show, NULL, status_input},                   // 历史输入状态
    {"his_status_temp", "0\n", zephyr_psu_byte_his_show, NULL, status_temp},                    // 历史温度状态
    {"his_status_cml", "0\n", zephyr_psu_byte_his_show, NULL, status_cml},                     // 历史通信/控制状态
    {"his_status_mfr_spec", "0\n", zephyr_psu_byte_his_show, NULL, status_mfr_spec},                // 历史厂商特定状态
    {"his_status_fans12", "0\n", zephyr_psu_byte_his_show, NULL, status_fans12},                  // 历史风扇状态

    // 历史读取相关属性
    {"his_read_vin", "0\n", zephyr_psu_sensor_his_show, NULL, read_vin},                       // 历史输入电压读数
    {"his_read_iin", "0\n", zephyr_psu_sensor_his_show, NULL, read_iin},                       // 历史输入电流读数
    {"his_read_vcap", "0\n", zephyr_psu_word_his_show, NULL, read_vcap},                      // 历史电容电压读数
    {"his_read_vout", "0\n", zephyr_psu_sensor_his_show, NULL, read_vout},                      // 历史输出电压读数
    {"his_read_vin1", "0\n", zephyr_psu_sensor_his_show, NULL, read_vin1},                      // 历史输入电压1读数
    {"his_read_iout", "0\n", zephyr_psu_sensor_his_show, NULL, read_iout},                      // 历史输出电流读数
    {"his_read_temp1", "0\n", zephyr_psu_sensor_his_show, NULL, read_temp1},                     // 历史温度1读数
    {"his_read_temp2", "0\n", zephyr_psu_sensor_his_show, NULL, read_temp2},                     // 历史温度2读数
    {"his_read_temp3", "0\n", zephyr_psu_sensor_his_show, NULL, read_temp3},                     // 历史温度3读数
    {"his_read_fan_speed1", "0\n", zephyr_psu_sensor_his_show, NULL, read_fan_speed1},                // 历史风扇速度1读数
    {"his_read_pout", "0\n", zephyr_psu_sensor_his_show, NULL, read_pout},                      // 历史输出功率读数
    {"his_read_pin", "0\n", zephyr_psu_sensor_his_show, NULL, read_pin},                       // 历史输入功率读数
    {"his_mfr_pos_total", "0\n", zephyr_psu_block_hex_his_show, NULL, mfr_pos_total},                  // 历史厂商位置总数
    {"his_mfr_pos_last", "0\n", zephyr_psu_block_hex_his_show, NULL, mfr_pos_last},                   // 历史厂商最后位置

    // 事件索引和读取相关属性
    {"delta_event_index", "0\n", zephyr_psu_word_show, zephyr_psu_word_store, delta_event_index},                  // Delta事件索引
    {"his_delta_read_event", "0\n", zephyr_powerbrick_block_hex_his_show, NULL, delta_read_event},               // Delta历史事件读取
    {"acep_event_index", "0\n", zephyr_psu_byte_show, zephyr_psu_byte_store, acep_event_index},                   // ACEP事件索引
    {"his_acep_read_event", "0\n", zephyr_powerbrick_block_hex_his_show, NULL, acep_read_event},                // ACEP历史事件读取

    // 制造商信息相关属性
    {"mfr_id", "AliPSU\n", zephyr_psu_block_show, NULL, mfr_id},                        // 制造商ID
    {"mfr_model", "ALI-PSU\n", zephyr_psu_block_show, NULL, mfr_model},                    // 制造商型号
    {"mfr_location", "China\n", zephyr_psu_block_show, NULL, mfr_location},                   // 制造商位置
    {"mfr_date", "20240101\n", zephyr_psu_block_show, NULL, mfr_date},                    // 制造日期
    {"mfr_sn", "ALI00000000\n", zephyr_psu_block_show, NULL, mfr_sn},                   // 制造商序列号
    {"mfr_psu_defined", "0\n", zephyr_psu_byte_show, NULL, mfr_psu_defined},                    // 制造商定义字段

    // 版本和状态相关属性
    {"hw_version", "1.0\n", zephyr_psu_block_show, NULL, mfr_hw_rev},                       // 硬件版本
    {"fw_version", "1.0\n", zephyr_psu_fw_version_show, NULL, fw_rev},                       // 固件版本
    {"status0_other", "0\n", zephyr_psu_byte_show, NULL, status_other},                      // 其他状态
    {"line_status", "0\n", zephyr_psu_byte_show, NULL, line_status},                        // 线路状态
    {"match_status", "0\n", zephyr_psu_byte_show, NULL, match_status},                       // 匹配状态
    {"status0_input_b", "0\n", zephyr_psu_byte_show, NULL, status_input_b},                    // 输入B状态
    {"input_channel_status", "0\n", zephyr_psu_byte_show, NULL, input_channel_status},               // 输入通道状态
    {"power_supply_mode", "0\n", zephyr_psu_byte_show, zephyr_psu_byte_store, power_supply_mode},                  // 电源模式

    // 页和引导加载程序相关属性
    {"mfr_page", "0\n", zephyr_psu_mfr_page_show, zephyr_psu_mfr_page_store, mfr_page},                           // 制造商页
    {"mfr_pos_total", "0\n", zephyr_psu_bootloader_str_show, NULL, mfr_pos_total},                      // 制造商位置总数
    {"mfr_pos_last", "0\n", zephyr_psu_bootloader_str_show, NULL, mfr_pos_last},                       // 制造商最后位置
    {"bootloader_key", "0\n", zephyr_psu_bootloader_str_show, zephyr_psu_bootloader_str_store, bootloader_key},                     // 引导加载程序密钥
    {"bootloader_product_key", "0\n", zephyr_psu_bootloader_str_show, zephyr_psu_bootloader_str_store, bootloader_product_key},             // 引导加载程序产品密钥
    {"bootloader_status", "0\n", zephyr_psu_bootloader_hex_show, zephyr_psu_bootloader_hex_store, bootloader_status},                  // 引导加载程序状态
    {"bootloader_memory16", "0\n", zephyr_psu_bootloader_hex_show, zephyr_psu_bootloader_str_store, bootloader_memory16},                // 引导加载程序内存16
    {"bootloader_memory32", "0\n", zephyr_psu_bootloader_hex_show, zephyr_psu_bootloader_str_store, bootloader_memory32},                // 引导加载程序内存32

    // 电流共享相关属性
    {"curr_share_adjust_en", "0\n", zephyr_psu_byte_show, zephyr_psu_byte_store, curr_share_adjust_en},               // 电流共享调整使能
    {"curr_share_adjust", "0\n", zephyr_psu_byte_show, zephyr_psu_byte_store, curr_share_adjust},                  // 电流共享调整
    {"curr_share_compensation_read", "0\n", zephyr_psu_word_show, zephyr_psu_word_store, curr_share_compensation_read},       // 电流共享补偿读取
    {"curr_share_compensation_write", "0\n", zephyr_psu_word_show, zephyr_psu_word_store, curr_share_compensation_write},      // 电流共享补偿写入
    {"curr_iout_filter", "0\n", zephyr_psu_sensor_show, NULL, curr_iout_filter},                   // 输出电流滤波

    // 设备名称
    {"name", "alipsu\n", NULL, NULL, 0x00},                          // 设备名称
};
#endif

#if defined(CONFIG_BOARD_ALIMOC30) || defined(CONFIG_BOARD_ALIMOC31)
static const char *adc_file[] = {
	"in1_input",
	"in2_input",
	"in3_input",
	"in4_input",
	"in5_input",
	"in6_input",
	"in7_input",
	"in8_input",
	"in9_input",
	"in10_input",
	"in11_input",
	"in12_input",
	"in13_input",
	"name"
};
#elif defined(CONFIG_BOARD_ALISMC)
static const char *adc_file[] = {
	"in1_input",
	"in2_input",
	"in3_input",
	"in4_input",
	"in5_input",
	"in6_input",
	"in7_input",
	"in8_input",
	"in9_input",
	"in10_input",
	"in11_input",
	"in12_input",
	"in13_input",
	"in14_input",
	"in15_input",
	"name"
};
#endif

// 查找属性默认值
static const char *find_pmbus_default_value(const char *attr_name)
{
    for (size_t i = 0; i < ARRAY_SIZE(pmbus_default_values); i++) {
        if (strcmp(pmbus_default_values[i].attr_name, attr_name) == 0) {
            return pmbus_default_values[i].default_value;
        }
    }
    return NULL;
}

// 检查属性是否有效
static bool is_valid_pmbus_attr(const char *attr_name)
{
    return find_pmbus_default_value(attr_name) != NULL;
}

// 检查属性是否为有效的ADC属性
static bool is_valid_adc_attr(const char *attr_name)
{
    for (size_t i = 0; i < ARRAY_SIZE(adc_file); i++) {
        if (strcmp(adc_file[i], attr_name) == 0) {
            return true;
        }
    }
    return false;
}

static const struct moc_fpga_default_values moc_fpga_default_values[] = {
    {"temp1_input", "0\n"},
    {"temp1_label", "Max Temp\n"},
	{"temp2_input", "0\n"},
	{"temp2_label", "SDM Temp\n"},
	{"temp3_input", "0\n"},
	{"temp3_label", "MAC0 Temp\n"},
	{"temp4_input", "0\n"},
	{"temp4_label", "MAC1 Temp\n"},
	{"version", "0\n"},
	{"chip_id", "0\n"},
};

static const char *find_moc_fpga_default_value(const char *attr_name)
{
    for (size_t i = 0; i < ARRAY_SIZE(moc_fpga_default_values); i++) {
        if (strcmp(moc_fpga_default_values[i].name, attr_name) == 0) {
            return moc_fpga_default_values[i].default_value;
        }
    }
    return NULL;
}

static bool is_valid_moc_fpga_attr(const char *attr_name)
{
    return find_moc_fpga_default_value(attr_name) != NULL;
}

// poll信号用于支持异步I/O
static struct k_poll_signal hwmon_poll_signal = K_POLL_SIGNAL_INITIALIZER(hwmon_poll_signal);

/**
 * 从路径中提取hwmon设备名
 */
static int get_hwmon_device(const char *path, char *output, size_t output_size)
{
	const size_t base_len = strlen(HWMON_MNT_POINT);

	if (strncmp(path, HWMON_MNT_POINT, base_len) != 0) {
		return -1;
	}

	const char *hwmon_start = path + base_len + 1;
	const char *current = hwmon_start;
	size_t length = 0;

	while (*current && *current != '/' && length < output_size - 1) {
		output[length++] = *current++;
	}

	output[length] = '\0';
	return (length > 0) ? 0 : -1;
}

/**
 * 从路径中提取属性名
 */
static int get_hwmon_attr(const char *path, char *result, size_t max_len)
{
	if (!path || !result || max_len == 0) {
		return -1;
	}

	const char *last_slash = strrchr(path, '/');
	if (!last_slash) {
		return -1;
	}

	const char *input_start = last_slash + 1;
	size_t len = strlen(input_start);

	if (len >= max_len) {
		return -1;
	}

	strncpy(result, input_start, len);
	result[len] = '\0';
	return 0;
}

static int hwmondev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
	LOG_DBG("Opening hwmon device: %s", file_name);

	struct hwmon_device_info *client = NULL;
	char device_name[20];
	char attr_name[20];

	if (get_hwmon_device(file_name, device_name, sizeof(device_name)) != 0) {
		LOG_ERROR("HWMON device not found in path: %s", file_name);
		return -ENODEV;
	}

	if (get_hwmon_attr(file_name, attr_name, sizeof(attr_name)) != 0) {
		LOG_ERROR("Failed to extract attribute from path: %s", file_name);
		return -ENODEV;
	}

	// 查找对应的设备
	for (size_t i = 0; i < HWMON_NUM; i++) {
		char hwmon_dev_name[32];
		snprintf(hwmon_dev_name, sizeof(hwmon_dev_name), "hwmon%zu", i);

		if (strcmp(device_name, hwmon_dev_name) == 0) {
			client = &hwmon_devices[i];
			break;
		}
	}

	if (!client) {
		LOG_ERROR("HWMON device '%s' not found", device_name);
		return -ENODEV;
	}


	const struct hwmon_ls_config *cfg = hwmon_ls_get_config(client->dev);
	if (!cfg) {
		LOG_ERROR("Failed to get hwmon configuration");
		return -ENODEV;
	}

	switch (cfg->class) {
	case HWMON_ADC:
		if (!is_valid_adc_attr(attr_name)) {
			LOG_ERROR("adc attr '%s' is not found", attr_name);
			return -EINVAL;
		}
		break;
	case HWMON_FAN:
	case HWMON_TEMP:
	case HWMON_SENSOR:
		break;
	case HWMON_PMBUS:
		if (!is_valid_pmbus_attr(attr_name)) {
			LOG_ERROR("pmbus attr '%s' is not found", attr_name);
			return -EINVAL;
		}
		break;
	case HWMON_MOC_FPGA:
		if (!is_valid_moc_fpga_attr(attr_name)) {
			LOG_ERROR("moc_fpga attr '%s' is not found", attr_name);
			return -EINVAL;
		}
		break;
	default:
		LOG_ERROR("Unsupported hwmon class: %d", cfg->class);
		return -EINVAL;
	}

	// 分配文件句柄
	struct hwmon_file_handle *handle = k_malloc(sizeof(struct hwmon_file_handle));  //最好使用全局数组实现文件存储
	if (!handle) {
		LOG_ERROR("Failed to allocate file handle");
		return -ENOMEM;
	}

	handle->device = client;
	strncpy(handle->attr_name, attr_name, sizeof(handle->attr_name) - 1);
	handle->attr_name[sizeof(handle->attr_name) - 1] = '\0';
	atomic_set(&handle->opened, 0);
	handle->nonblocking = 0;

	// 检查文件是否已被打开
	if (!atomic_cas(&handle->opened, 0, 1)) {
		k_free(handle);
		LOG_ERROR("Attribute file '%s' is already opened", attr_name);
		return -EINVAL;
	}

	if (!device_is_ready(client->dev)) {
		atomic_set(&handle->opened, 0);
		k_free(handle);
		LOG_ERROR("HWMON device '%s' is not ready", device_name);
		return -ENODEV;
	}

	if (!zfp->mp) {
		atomic_set(&handle->opened, 0);
		k_free(handle);
		LOG_ERROR("Invalid mount point");
		return -EINVAL;
	}

	zfp->filep = handle;
	zfp->mp = &hwmonfs_mnt;

	LOG_DBG("HWMON device '%s' opened successfully", file_name);
	return 0;
}

static int hwmondev_close(struct fs_file_t *zfp)
{
	LOG_DBG("Closing hwmon device");

	if (zfp->filep) {
		struct hwmon_file_handle *handle = zfp->filep;
		atomic_set(&handle->opened, 0);
		k_free(handle);
		zfp->filep = NULL;
	}

	if (zfp->mp) {
		zfp->mp = NULL;
		LOG_DBG("HWMON mount closed");
	}

	return 0;
}

static uint8_t ch1[] = {0, 1, 10, 11, 12};          // ADC1通道
#ifdef CONFIG_BOARD_ALIMOC31
static uint8_t ch2[] = {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};  // ADC2通道
#else
static uint8_t ch2[] = {0, 1, 2, 8, 9, 10, 11, 12};  // ADC2通道
#endif

/**
 * 设置ADC通道配置
 */
static enum adc_reference ADC_Channel_setCfg(const struct device* adc, uint8_t channel_id)
{
    struct adc_channel_cfg channel_config = {
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_SAMPLETIME_15CYCLES,
        .channel_id = channel_id,
    };
    adc_channel_setup(adc, &channel_config);
    return channel_config.reference;
}

/**
 * 配置ADC通道
 */
static int configure_adc_channels(const struct device* adc, const uint8_t* channels, size_t num_channels)
{
    for (size_t i = 0; i < num_channels; i++) {
        ADC_Channel_setCfg(adc, channels[i]);
    }
    return 0;
}

/**
 * 读取ADC值
 */
static ssize_t hwmondev_read_adc(const struct device* adc, unsigned int channel_id, uint16_t* result)
{
    if (!adc || !result) {
        return -EINVAL;
    }

    // 定义缓冲区
    uint16_t buffer[14]; // 栈分配，避免动态内存分配
    struct adc_sequence sequence = {
        .buffer = buffer,
        .buffer_size = sizeof(buffer),
        .options = NULL,
    };

    // 根据设备名称配置通道
    if (strcmp(adc->name, "adc@4008b000") == 0) {
        configure_adc_channels(adc, ch1, ARRAY_SIZE(ch1));
    } else if (strcmp(adc->name, "adc@4008b400") == 0) {
        configure_adc_channels(adc, ch2, ARRAY_SIZE(ch2));
    } else {
        LOG_ERROR("Unsupported ADC device: %s", adc->name);
        return -ENODEV;
    }

    // 执行ADC读取
    int ret = adc_read(adc, &sequence);
    if (ret) {
        LOG_ERROR("ADC read failed: %d", ret);
        return ret;
    }

    // 查找对应通道的ADC值
    uint16_t adc_value = 0;
    const uint8_t* channels = strcmp(adc->name, "adc@4008b000") == 0 ? ch1 : ch2;
    size_t num_channels = strcmp(adc->name, "adc@4008b000") == 0 ? ARRAY_SIZE(ch1) : ARRAY_SIZE(ch2);

    for (size_t i = 0; i < num_channels; i++) {
        if (channel_id == channels[i]) {
            adc_value = buffer[i];
            break;
        }
    }

    // 计算结果并返回
    *result = (adc_value * 1400) / 4095;

    // 日志输出
    LOG_DBG("ADC Device: %s, Channel ID: %u, Value: %u", adc->name, channel_id, adc_value);

    return 0;
}

static ssize_t hwmondev_read(struct fs_file_t *zfp, void *ptr, size_t size)
{
	int ret = 0;

	LOG_DBG("Reading from hwmon device");

	if (!zfp || !ptr) {
		LOG_ERROR("Invalid parameters");
		return -EINVAL;
	}

	struct hwmon_file_handle *handle = zfp->filep;
	if (!handle) {
		LOG_ERROR("No client context");
		return -EINVAL;
	}

	struct hwmon_device_info *client = handle->device;
	const char *attr_name = handle->attr_name;

	if (size > CONFIG_HWMON_DEV_MAX_SIZE) {
		size = CONFIG_HWMON_DEV_MAX_SIZE;
	}

	// 检查是否已经读取完成
	if (zfp->flags & FS_FLAG_FILE_READ_END) {
		if (handle->nonblocking) {
			return -EAGAIN;
		}
		return 0;
	}

	const struct hwmon_ls_config *cfg = hwmon_ls_get_config(client->dev);
	if (!cfg) {
		LOG_ERROR("Failed to get hwmon configuration");
		return -ENODEV;
	}

	ssize_t len = 0;

	switch (cfg->class) {
	case HWMON_FAN:  // TODO:
		break;

	case HWMON_ADC:
		if (strlen(attr_name) > 0) {
			unsigned int channel_index;
			char attr_type[16];

			if (sscanf(attr_name, "in%u_%15s", &channel_index, attr_type) >= 1) {
				if (channel_index > 0 && (channel_index-1) < cfg->in_num) {
					if (strcmp(attr_type, "input") == 0) {
						uint16_t adc_value = 0;
						int ret = hwmondev_read_adc(cfg->in[channel_index-1].dev,
						                           cfg->in[channel_index-1].channel, &adc_value);
						if (ret == 0) {
							len = snprintf((char *)ptr, size, "%u\n", adc_value);
							zfp->flags |= FS_FLAG_FILE_READ_END;
							return len;
						} else {
							return ret;
						}
					}
				}
			} else if (strcmp(attr_name, "name") == 0) {
				len = snprintf((char *)ptr, size, "iio_hwmon\n");
				zfp->flags |= FS_FLAG_FILE_READ_END;
				return len;
			}
		}
		break;

	case HWMON_TEMP: // TODO:
		break;

	case HWMON_SENSOR:	// TODO:
		break;
	case HWMON_PMBUS:
		if (strlen(attr_name) > 0) {
#if defined(CONFIG_BOARD_ALISMC)
			// 遍历pmbus_default_values数组查找对应的属性
			for (int i = 0; i < ARRAY_SIZE(pmbus_default_values); i++) {
				if (strcmp(pmbus_default_values[i].attr_name, attr_name) == 0) {
					// 如果有特定的show函数，则调用它
					if (pmbus_default_values[i].read_func) {
						// 调用特定的show函数来获取值
						char temp_buf[64];
						uint8_t temp_len = sizeof(temp_buf);
						ret = pmbus_default_values[i].read_func(cfg->in[0].dev,
																ali_psu_regs[pmbus_default_values[i].index],
																temp_buf,
																&temp_len);
						if (ret >= 0) {
							len = snprintf((char *)ptr, size, "%s", temp_buf);
							zfp->flags |= FS_FLAG_FILE_READ_END;
							return len;
						} else {
							// 如果特定函数失败，返回默认值
							LOG_ERROR("Show function failed for attribute %s, using default", attr_name);
						}
					}
					// 如果没有特定的show函数或函数失败，返回默认值
					len = snprintf((char *)ptr, size, "%s", pmbus_default_values[i].default_value);
					zfp->flags |= FS_FLAG_FILE_READ_END;
					return len;
				}
			}
			// 如果没找到属性，返回错误
			LOG_ERROR("Attribute %s not found in pmbus_default_values", attr_name);
			return -ENOENT;
#else
			unsigned int channel_index;
			char attr_type[16];

			// 处理电压输入 (inX_input)
			if (sscanf(attr_name, "in%u_%15s", &channel_index, attr_type) >= 1) {
				if (channel_index > 0 && (channel_index-1) < cfg->in_num) {
					if (strcmp(attr_type, "input") == 0) {
                        struct sensor_value voltage = {0};
						ret = sensor_sample_fetch(cfg->in[channel_index-1].dev); /* general sensor api */
						ret |= sensor_channel_get(cfg->in[channel_index-1].dev, SENSOR_CHAN_VOLTAGE, &voltage);
						if (ret == 0) {
							len = snprintf((char *)ptr, size, "%d\n", voltage.val1);
							zfp->flags |= FS_FLAG_FILE_READ_END;
							return len;
						}
					}
				}
			}
			// 处理电流 (currX_input)
			// else if (sscanf(attr_name, "curr%u_%15s", &channel_index, attr_type) >= 1) {
			// 	if (channel_index > 0 && (channel_index-1) < cfg->in_num) {
			// 		if (strcmp(attr_type, "input") == 0) {
			// 			ret = sensor_sample_fetch(cfg->in[channel_index-1].dev); /* general sensor api */
			// 			ret |= sensor_channel_get(cfg->in[channel_index-1].dev, SENSOR_CHAN_CURRENT, &current);
			// 			if (ret == 0) {
			// 				len = snprintf((char *)ptr, size, "%d\n", current.val1);
			// 				zfp->flags |= FS_FLAG_FILE_READ_END;
			// 				return len;
			// 			}
			// 		}
			// 	}
			// }
			// 处理功率 (powerX_input)
			else if (sscanf(attr_name, "power%u_%15s", &channel_index, attr_type) >= 1) {
				if (channel_index > 0 && (channel_index-1) < cfg->in_num) {
					if (strcmp(attr_type, "input") == 0) {
						struct sensor_value power = {0};
						ret = sensor_sample_fetch(cfg->in[channel_index-1].dev); /* general sensor api */
						ret |= sensor_channel_get(cfg->in[channel_index-1].dev, SENSOR_CHAN_POWER, &power);
						if (ret == 0) {
							len = snprintf((char *)ptr, size, "%d\n", power.val1);
							zfp->flags |= FS_FLAG_FILE_READ_END;
							return len;
						}
					}
				}
			}
			// 处理温度 (tempX_input)
			else if (sscanf(attr_name, "temp%u_%15s", &channel_index, attr_type) >= 1) {
				if (channel_index > 0 && (channel_index-1) < cfg->in_num) {
					if (strcmp(attr_type, "input") == 0) {
						struct sensor_value temp = {0};
						ret = sensor_sample_fetch(cfg->in[channel_index-1].dev); /* general sensor api */
						ret |= sensor_channel_get(cfg->in[channel_index-1].dev, SENSOR_CHAN_GAUGE_TEMP, &temp);
						if (ret == 0) {
							len = snprintf((char *)ptr, size, "%d\n", temp.val1);
							zfp->flags |= FS_FLAG_FILE_READ_END;
							return len;
						}
					}
				}
			}

			const char *default_val = find_pmbus_default_value(attr_name);
			if (!default_val) {
				LOG_ERROR("Default value not found for attribute: %s", attr_name);
				return -ENOENT; // 返回标准错误码，表示路径不存在
			}
			len = snprintf((char *)ptr, size, "%s", default_val);
			zfp->flags |= FS_FLAG_FILE_READ_END;
			return len;
#endif
		}
		break;

	case HWMON_MOC_FPGA:
        if (strlen(attr_name) > 0)
        {
            unsigned int channel_index;
            char attr_type[16];
            unsigned int type;

            // 处理温度 (tempX_input)
            if (sscanf(attr_name, "temp%u_%15s", &channel_index, attr_type) >=
                1)
            {
                switch (channel_index)
                {
                    case 1:
                        type = FPGA_TEMP_MAX;
                        break;
                    case 2:
                        type = FPGA_TEMP_SDM;
                        break;
                    case 3:
                        type = FPGA_TEMP_MAC0;
                        break;
                    case 4:
                        type = FPGA_TEMP_MAC1;
                        break;
					default:
						type = FPGA_TEMP_MAX;
                }
                if (strcmp(attr_type, "input") == 0)
                {
                    char buf[MOC_FPGA_BUF_SIZE];
                    int ret = moc_fpga_get_temperature(
                        cfg->in[0].dev, cfg->in[0].channel, type, buf);
                    if (ret > 0)
                    {
                        len = snprintf((char*)ptr, size, "%s", buf);
                        zfp->flags |= FS_FLAG_FILE_READ_END;
                        return len;
                    }
                    else
                    {
                        LOG_ERROR("Failed to get moc-fpga temperature(%d).",
                                  ret);
                    }
                }
            }
            else if (strcmp(attr_name, "version") == 0)
            {
                char buf[MOC_FPGA_BUF_SIZE];
                int ret = moc_fpga_get_version(cfg->in[0].dev,
                                               cfg->in[0].channel, buf);
                if (ret > 0)
                {
                    len = snprintf((char*)ptr, size, "%s", buf);
                    zfp->flags |= FS_FLAG_FILE_READ_END;
                    return len;
                }
                else
                {
                    LOG_ERROR("Failed to get moc-fpga version(%d).", ret);
                }
            }
            else if (strcmp(attr_name, "chip_id") == 0)
            {
                char buf[MOC_FPGA_BUF_SIZE];
                int ret = chip_id_show(cfg->in[0].dev, cfg->in[0].channel, buf);
                if (ret > 0)
                {
                    len = snprintf((char*)ptr, size, "%s", buf);
                    zfp->flags |= FS_FLAG_FILE_READ_END;
                    return len;
                }
                else
                {
                    LOG_ERROR("Failed to get moc-fpga chip id(%d).", ret);
                }
            }

            const char* default_val = find_moc_fpga_default_value(attr_name);
            if (!default_val)
            {
                LOG_ERROR("Default value not found for attribute: %s",
                          attr_name);
                return -ENOENT; // 返回标准错误码，表示路径不存在
            }
            len = snprintf((char*)ptr, size, "%s", default_val);
            zfp->flags |= FS_FLAG_FILE_READ_END;
            return len;
        }
        break;
    default:
		LOG_ERROR("Unsupported device class: %d", cfg->class);
		return -ENOTTY;
	}

	zfp->flags |= FS_FLAG_FILE_READ_END;
	return len > 0 ? len : 0;
}

static ssize_t hwmondev_write(struct fs_file_t *zfp, const void *ptr, size_t size)
{
	int ret = 0;

	LOG_DBG("Reading from hwmon device");

	if (!zfp || !ptr) {
		LOG_ERROR("Invalid parameters");
		return -EINVAL;
	}

	struct hwmon_file_handle *handle = zfp->filep;
	if (!handle) {
		LOG_ERROR("No client context");
		return -EINVAL;
	}

	struct hwmon_device_info *client = handle->device;
	const char *attr_name = handle->attr_name;

	if (size > CONFIG_HWMON_DEV_MAX_SIZE) {
		size = CONFIG_HWMON_DEV_MAX_SIZE;
	}

	// 检查是否已经读取完成
	if (zfp->flags & FS_FLAG_FILE_READ_END) {
		if (handle->nonblocking) {
			return -EAGAIN;
		}
		return 0;
	}

	const struct hwmon_ls_config *cfg = hwmon_ls_get_config(client->dev);
	if (!cfg) {
		LOG_ERROR("Failed to get hwmon configuration");
		return -ENODEV;
	}

	switch (cfg->class) {
	case HWMON_FAN:  // TODO:
		break;
	case HWMON_ADC:
		break;
	case HWMON_TEMP: // TODO:
		break;
	case HWMON_SENSOR:	// TODO:
		break;
	case HWMON_PMBUS:
#if defined(CONFIG_BOARD_ALISMC)
		uint8_t old_page = 0;
		ret = ali_psu_mfr_page_show(cfg->in[0].dev, &old_page);
		if (ret < 0) {
			LOG_ERROR("Failed to read current page: %d", old_page);
			return -EIO;
		}

		ret = ali_psu_mfr_page_store(cfg->in[0].dev, old_page+1);
		if (ret < 0) {
			LOG_ERROR("Failed to store new page: %d", old_page+1);
			return -EIO;
		}

		ret = ali_psu_mfr_page_show(cfg->in[0].dev, &old_page);
		if (ret < 0) {
			LOG_ERROR("Read current updated page: %d", old_page);
			return -EIO;
		}
		for (int i = 0; i < ARRAY_SIZE(pmbus_default_values); i++) {
			if (strcmp(pmbus_default_values[i].attr_name, attr_name) == 0) {
				// 检查是否有写入函数
				if (pmbus_default_values[i].write_func) {
					// 调用特定的store函数来处理写入
					uint8_t write_len = size;
					ret = pmbus_default_values[i].write_func(cfg->in[0].dev,
															ali_psu_regs[pmbus_default_values[i].index],
															(uint8_t *)ptr,
															&write_len);
					if (ret >= 0) {
						// 成功写入后，更新默认值（如果需要）
						return size; // 返回写入的字节数
					} else {
						LOG_ERROR("write function failed for attribute %s", attr_name);
						return ret;
					}
				} else {
					// 如果没有写入函数，返回错误
					LOG_ERROR("Write operation not supported for attribute %s", attr_name);
					return -EPERM; // 操作不允许
				}
			}
		}
		// 如果没找到属性，返回错误
		LOG_ERROR("Attribute %s not found in pmbus_default_values", attr_name);
		return -ENOENT;
#endif
		break;
	case HWMON_MOC_FPGA:
        break;
    default:
		LOG_ERROR("Unsupported device class: %d", cfg->class);
		return -ENOTTY;
	}

	zfp->flags |= FS_FLAG_FILE_READ_END;
	return size > 0 ? size : 0;
}

static int hwmon_poll_prepare_ctx(struct hwmon_file_handle *handle,
                                  struct zvfs_pollfd *pfd,
                                  struct k_poll_event **pev,
                                  struct k_poll_event *pev_end)
{
	if (pfd->events & ZVFS_POLLIN) {
		if (*pev == pev_end) {
			return -ENOMEM;
		}

		(*pev)->obj = &hwmon_poll_signal;
		(*pev)->type = K_POLL_TYPE_SIGNAL;
		(*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
		(*pev)->state = K_POLL_STATE_NOT_READY;
		(*pev)++;

		// 立即触发信号，表示数据可读
		k_poll_signal_raise(&hwmon_poll_signal, 0);
	}

	return 0;
}

static int hwmon_poll_update_ctx(struct hwmon_file_handle *handle,
                                 struct zvfs_pollfd *pfd,
                                 struct k_poll_event **pev)
{
	if (pfd->events & ZVFS_POLLIN) {
		if ((*pev)->state != K_POLL_STATE_NOT_READY) {
			pfd->revents |= ZVFS_POLLIN;
		}
		(*pev)++;
	}

	return 0;
}

static int hwmondev_ioctl(struct fs_file_t *zfp, unsigned long request, va_list args)
{
	LOG_DBG("hwmon ioctl: 0x%lx", request);

	struct hwmon_file_handle *handle = zfp->filep;
	if (!handle) {
		LOG_ERROR("No client context");
		return -EINVAL;
	}

	switch (request) {
	case F_GETFL: // TODO:
		return O_RDWR;

	case F_SETFL: {
		int flags = va_arg(args, int);
		handle->nonblocking = (flags & O_NONBLOCK) != 0;
		LOG_DBG("Set nonblocking mode to %d", handle->nonblocking);
		return 0;
	}

	case ZFD_IOCTL_POLL_PREPARE: {
		struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
		struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
		struct k_poll_event *pev_end = va_arg(args, struct k_poll_event *);
		return hwmon_poll_prepare_ctx(handle, pfd, pev, pev_end);
	}

	case ZFD_IOCTL_POLL_UPDATE: {
		struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
		struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
		return hwmon_poll_update_ctx(handle, pfd, pev);
	}

	case ZFD_IOCTL_SET_LOCK:
		return 0;

	default:
		LOG_ERROR("Unsupported ioctl request: 0x%lx", request);
		return -ENOTTY;
	}

	return 0;
}

static int hwmondev_mount(struct fs_mount_t *mountp)
{
	LOG_DBG("Mounting hwmon filesystem");

	if (!mountp || !mountp->mnt_point) {
		LOG_ERROR("Invalid mount point");
		return -EINVAL;
	}

	for (size_t i = 0; i < HWMON_NUM; i++) {
		if (!device_is_ready(hwmon_devices[i].dev)) {
			LOG_ERROR("HWMON device %zu not ready", i);
			return -ENODEV;
		}
	}

	LOG_DBG("HWMON filesystem mounted successfully");
	return 0;
}

static int hwmondev_unmount(struct fs_mount_t *mountp)
{
	LOG_DBG("Unmounting hwmon filesystem");

	if (!mountp || !mountp->mnt_point) {
		LOG_ERROR("Invalid mount point");
		return -EINVAL;
	}

	mountp->mnt_point = NULL;
	LOG_DBG("HWMON filesystem unmounted");
	return 0;
}

static int hwmondev_opendir(struct fs_dir_t *dp, const char *path)
{
	LOG_DBG("Opening directory: %s", path);

	if (!dp || !path) {
		return -EINVAL;
	}

	// 根目录处理
	if (strcmp(path, HWMON_MNT_POINT) == 0) {
		struct hwmon_dir_ctx *ctx = k_malloc(sizeof(struct hwmon_dir_ctx));
		if (!ctx) {
			return -ENOMEM;
		}

		ctx->type = ROOT_DIR;
		ctx->index = 0;
		dp->dirp = ctx;
		return 0;
	}

	// 设备目录处理
	const size_t base_len = strlen(HWMON_MNT_POINT);
	if (strncmp(path, HWMON_MNT_POINT, base_len) == 0 && path[base_len] == '/') {
		const char *dev_part = path + base_len + 1;
		unsigned int device_index;

		if (sscanf(dev_part, "hwmon%u", &device_index) == 1) {
			if (device_index < HWMON_NUM) {
				struct hwmon_dir_ctx *ctx = k_malloc(sizeof(struct hwmon_dir_ctx));
				if (!ctx) {
					return -ENOMEM;
				}

				ctx->type = DEVICE_DIR;
				ctx->dev.devid = device_index;
				ctx->dev.attr_idx = 0;
				dp->dirp = ctx;
				return 0;
			}
		}
	}

	LOG_ERROR("Invalid hwmon path: %s", path);
	return -ENOENT;
}

static int hwmondev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
	LOG_DBG("Reading directory entry");

	if (!dp || !dp->dirp || !entry) {
		LOG_ERROR("Invalid parameters");
		return -EINVAL;
	}

	struct hwmon_dir_ctx *ctx = dp->dirp;

	switch (ctx->type) {
	case ROOT_DIR:
		if (ctx->index < HWMON_NUM) {
			snprintf(entry->name, sizeof(entry->name), "hwmon%zu", ctx->index);
			entry->type = FS_DIR_ENTRY_DIR;
			entry->size = 0;
			ctx->index++;
			return 0;
		}
		break;

	case DEVICE_DIR:
		if (ctx->dev.devid >= HWMON_NUM) {
			LOG_ERROR("Invalid device ID: %zu", ctx->dev.devid);
			return -EINVAL;
		}

		const struct hwmon_device_info *dev = &hwmon_devices[ctx->dev.devid];
		const struct hwmon_ls_config *cfg = hwmon_ls_get_config(dev->dev);

		if (!cfg) {
			LOG_ERROR("Failed to get hwmon config for device %zu", ctx->dev.devid);
			return -ENODEV;
		}

		switch (cfg->class) {
			case HWMON_FAN:	// TODO:
				break;
			case HWMON_TEMP:	// TODO:
				break;
			case HWMON_ADC:
				char attr_name[MAX_ATTR_NAME_LEN];
				if (ctx->dev.attr_idx < cfg->in_num) {
					snprintf(attr_name, sizeof(attr_name), "in%u_input", ctx->dev.attr_idx + 1);
					strncpy(entry->name, attr_name, sizeof(entry->name) - 1);
					entry->name[sizeof(entry->name) - 1] = '\0';
					entry->type = FS_DIR_ENTRY_FILE;
					entry->size = sizeof(int);
					ctx->dev.attr_idx++;
					return 0;
				}
				else if (ctx->dev.attr_idx == cfg->in_num) {
					// 添加name属性
					strncpy(entry->name, "name", sizeof(entry->name) - 1);
					entry->name[sizeof(entry->name) - 1] = '\0';
					entry->type = FS_DIR_ENTRY_FILE;
					entry->size = sizeof(int);
					ctx->dev.attr_idx++;
					return 0;
				}
				break;
			case HWMON_SENSOR:	// TODO:
				break;
			case HWMON_PMBUS:	// TODO:
				if (ctx->dev.attr_idx < PMBUS_ATTR_COUNT) {
					strncpy(entry->name, pmbus_attr_names[ctx->dev.attr_idx], sizeof(entry->name) - 1);
					entry->name[sizeof(entry->name) - 1] = '\0';
					entry->type = FS_DIR_ENTRY_FILE;
					entry->size = sizeof(int);
					ctx->dev.attr_idx++;
					return 0;
				}
				break;
			case HWMON_MOC_FPGA:
				if (ctx->dev.attr_idx < MOC_FPGA_ATTR_COUNT) {
					strncpy(entry->name, moc_fpga_attr_names[ctx->dev.attr_idx], sizeof(entry->name) - 1);
					entry->name[sizeof(entry->name) - 1] = '\0';
					entry->type = FS_DIR_ENTRY_FILE;
					entry->size = sizeof(int);
					ctx->dev.attr_idx++;
					return 0;
				}
				break;
			default:
				LOG_ERROR("Unsupported device class: %d", cfg->class);
				return -ENOTTY;
		}
		break;
	default:
		LOG_ERROR("Unknown directory type: %d", ctx->type);
		return -EINVAL;
	}
	// 结束遍历
	memset(entry, 0, sizeof(*entry));
	return 0;
}

static int hwmondev_closedir(struct fs_dir_t *dp)
{
	LOG_DBG("Closing directory");

	if (!dp) {
		return -EINVAL;
	}

	if (dp->dirp) {
		k_free(dp->dirp);
		dp->dirp = NULL;
	}

	return 0;
}

/*
hwmondev_statvfs 函数实现文件系统的统计信息查询功能，用于返回文件系统的基本空间和容量信息。
hwmon不占用实际存储空间，只是提供传感器数据访问接口,这些统计信息对于hwmon这类虚拟文件系统没有实际意义.
初始化stat是为了填充 fs_statvfs 结构体的所有相关字段,避免上层调用报错.
*/
static int hwmondev_statvfs(struct fs_mount_t *mountp, const char *path,
                            struct fs_statvfs *stat)
{
	if (!stat) {
		return -EINVAL;
	}

	stat->f_bsize = 1;   // 块大小
	stat->f_frsize = 1;  // 分配单元大小
	stat->f_blocks = 1; // 总块数
	stat->f_bfree = 1;  // 空闲块数

	return 0;
}

static int hwmondev_stat(struct fs_mount_t *mountp, const char *path,
                         struct fs_dirent *entry)
{
	LOG_DBG("Stat path: %s", path);

	if (!mountp || !path || !entry) {
		return -EINVAL;
	}

	// 根目录
	if (strcmp(path, HWMON_MNT_POINT) == 0) {
		entry->type = FS_DIR_ENTRY_DIR;
		entry->size = 0;
		return 0;
	}

	// 设备目录和属性文件
	const size_t base_len = strlen(HWMON_MNT_POINT);
	if (strncmp(path, HWMON_MNT_POINT, base_len) == 0 && path[base_len] == '/') {
		const char *subpath = path + base_len + 1;
		unsigned int dev_index;
		char attr_name[32];

		// 匹配属性文件路径：hwmonX/<attribute>
		if (sscanf(subpath, "hwmon%u/%31s", &dev_index, attr_name) == 2) {
			if (dev_index < HWMON_NUM) {
				const struct hwmon_device_info *dev = &hwmon_devices[dev_index];
				const struct hwmon_ls_config *cfg = hwmon_ls_get_config(dev->dev);

				if (!cfg) {
					LOG_ERROR("Failed to get hwmon config for device %u", dev_index);
					return -ENODEV;
				}

				// 验证属性有效性
				switch (cfg->class) {
					case HWMON_FAN:	// TODO:
						break;

					case HWMON_TEMP: // TODO:
						break;

					case HWMON_ADC: {
						unsigned int sensor_num;
						char attr_type[16];
						if (sscanf(attr_name, "in%u_%15s", &sensor_num, attr_type) >= 1) {
							if (sensor_num > 0 && sensor_num <= cfg->in_num) {
								entry->type = FS_DIR_ENTRY_FILE;
								entry->size = sizeof(int);
								return 0;
							}
						}
						break;
					}
					case HWMON_SENSOR: // TODO:
						break;
					case HWMON_PMBUS:	// TODO:
						// 使用辅助函数检查属性是否有效
						if (is_valid_pmbus_attr(attr_name)) {
							entry->type = FS_DIR_ENTRY_FILE;
							entry->size = sizeof(int);
							return 0;
						}
						break;

					case HWMON_MOC_FPGA:
						if (is_valid_moc_fpga_attr(attr_name)) {
							entry->type = FS_DIR_ENTRY_FILE;
							entry->size = sizeof(int);
							return 0;
						}
						break;
				}

				// 检查name属性
				if (strcmp(attr_name, "name") == 0) {
					entry->type = FS_DIR_ENTRY_FILE;
					entry->size = sizeof(int);
					return 0;
				}
			}
		}
		// 匹配设备路径格式：hwmonX
		else if (sscanf(subpath, "hwmon%u", &dev_index) == 1) {
			if (dev_index < HWMON_NUM) {
				entry->type = FS_DIR_ENTRY_DIR;
				entry->size = 0;
				return 0;
			}
		}
	}

	LOG_ERROR("Invalid path for stat: %s", path);
	return -ENOENT;
}

static const struct fs_file_system_t hwmondev_fs = {
	.mount = hwmondev_mount,
	.unmount = hwmondev_unmount,
	.open = hwmondev_open,
	.close = hwmondev_close,
	.read = hwmondev_read,
	.write = hwmondev_write,
	.lseek = NULL,
	.ioctl = hwmondev_ioctl,
	.opendir = hwmondev_opendir,
	.readdir = hwmondev_readdir,
	.closedir = hwmondev_closedir,
	.stat = hwmondev_stat,
	.statvfs = hwmondev_statvfs,
};

static int hwmon_dev_init(void)
{
	LOG_DBG("Initializing hwmon devices");
	LOG_DBG("Number of hwmon devices: %d", HWMON_NUM);

	// 输出设备信息
	for (size_t i = 0; i < HWMON_NUM; i++) {
		if (hwmon_devices[i].dev && hwmon_devices[i].dev->name) {
			LOG_DBG("hwmon_devices[%zu].dev->name = %s", i, hwmon_devices[i].dev->name);
		}
		if (hwmon_devices[i].label) {
			LOG_DBG("hwmon_devices[%zu].label = %s", i, hwmon_devices[i].label);
		}
	}

	hwmonfs_mnt.mnt_point = HWMON_MNT_POINT;
	LOG_DBG("hwmonfs_mnt: type: %d, mnt_point: %s", hwmonfs_mnt.type, hwmonfs_mnt.mnt_point);

	int rc = fs_mount(&hwmonfs_mnt);
	if (rc) {
		LOG_ERROR("Failed to mount hwmonfs: %d", rc);
		hwmonfs_mnt.mnt_point = NULL;
		return rc;
	}

	return 0;
}

static int hwmon_init(void)
{
	LOG_DBG("hwmon filesystem initialization started");

	int rc = fs_register(FS_HWMON, &hwmondev_fs);
	if (rc < 0) {
		LOG_ERROR("Failed to register hwmon filesystem: %d", rc);
		return rc;
	}

	LOG_DBG("hwmon filesystem registered");

	rc = hwmon_dev_init();
	if (rc) {
		LOG_ERROR("hwmon device initialization failed: %d", rc);
		return rc;
	}

	LOG_DBG("hwmon filesystem initialization completed");
	return 0;
}

SYS_INIT(hwmon_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
