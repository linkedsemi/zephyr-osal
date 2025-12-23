// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ali-moc-fpga.c - The i2c driver for MOC FPGA
 *
 * The source file is linux/drivers/misc
 *
 * Copyright (c) 2024, Alibaba Co. Ltd. All rights reserved.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>

#define I2C_REG_TEST 0x0
#define I2C_REG_ENABLE 0x1
#define I2C_REG_RW 0x2
#define I2C_REG_ADDR0 0x3
#define I2C_REG_ADDR1 0x4
#define I2C_REG_ADDR2 0x5
#define I2C_REG_ADDR3 0x6
#define I2C_REG_WDATA0 0x7
#define I2C_REG_WDATA1 0x8
#define I2C_REG_WDATA2 0x9
#define I2C_REG_WDATA3 0xA
#define I2C_REG_READY 0x10
#define I2C_REG_RDATA0 0x11
#define I2C_REG_RDATA1 0x12
#define I2C_REG_RDATA2 0x13
#define I2C_REG_RDATA3 0x14

#define FPGA_REG(reg) ((reg) << 2)
#define FPGA_REG_VERSION FPGA_REG(0x2B10FFE)
#define FPGA_REG_MAX_TEMP 0xFFFFFFFF
#define FPGA_REG_SDM_TEMP FPGA_REG(0x80)
#define FPGA_REG_MAC0_TEMP FPGA_REG(0x120001C)
#define FPGA_REG_MAC0_REG1 FPGA_REG(0x1210013)
#define FPGA_REG_MAC0_REG2 FPGA_REG(0x1210016)
#define FPGA_REG_MAC1_TEMP FPGA_REG(0x120001E)
#define FPGA_REG_MAC1_REG1 FPGA_REG(0x1210017)
#define FPGA_REG_MAC1_REG2 FPGA_REG(0x121001A)
#define FPGA_REG_CHIP_ID_LSB_REG FPGA_REG(0x00000090)
#define FPGA_REG_CHIP_ID_MSB_REG FPGA_REG(0x00000091)

/* Each client has this additional data */
struct moc_fpga_data {
	struct i2c_client *client;
	struct mutex mutex_lock;
};

enum {
	FPGA_TEMP_SDM,
	FPGA_TEMP_MAC0,
	FPGA_TEMP_MAC1,
	FPGA_TEMP_MAX,
};

static const u32 fpga_temp_regs[] = {
	FPGA_REG_SDM_TEMP,
	FPGA_REG_MAC0_TEMP,
	FPGA_REG_MAC1_TEMP,
	FPGA_REG_MAX_TEMP,
};

static const char* fpga_temp_labels[] = {
	"SDM Temp",
	"MAC0 Temp",
	"MAC1 Temp",
	"Max Temp",
	NULL,
};

static int moc_fpga_read_data(struct i2c_client *client, u32 reg, u32 *value)
{
	u32 reg_tmp = reg;
	*value = 0;

	if (i2c_smbus_write_byte_data(client, I2C_REG_ENABLE, 0x1))
		return -EIO;

	if (i2c_smbus_write_byte_data(client, I2C_REG_RW, 0x2))
		return -EIO;

	for (int i = 0; i < 4; i++) {
		if (i2c_smbus_write_byte_data(client, I2C_REG_ADDR0 + i,
					      reg_tmp & 0xff))
			return -EIO;
		reg_tmp >>= 8;
	}

	for (int i = 0; i < 4; i++) {
		s32 buf = i2c_smbus_read_byte_data(client, I2C_REG_READY);

		if (buf < 0)
			return -EIO;

		if (buf & 0x1)
			break;

		if (i == 3) {
			dev_info(&client->dev, "read reg: 0x%x timeout\n", reg);
			return -EIO;
		}

		msleep(10);
	}

	for (int i = 0; i < 4; i++) {
		*value <<= 8;
		s32 buf = i2c_smbus_read_byte_data(client, I2C_REG_RDATA3 - i);

		if (buf < 0)
			return -EIO;

		*value |= buf;
	}

	return 0;
}

static int moc_fpga_write_data(struct i2c_client *client, u32 reg, u32 value)
{
	if (i2c_smbus_write_byte_data(client, I2C_REG_ENABLE, 0x1))
		return -EIO;

	if (i2c_smbus_write_byte_data(client, I2C_REG_RW, 0x1))
		return -EIO;

	for (int i = 0; i < 4; i++) {
		if (i2c_smbus_write_byte_data(client, I2C_REG_ADDR0 + i,
					      reg & 0xff))
			return -EIO;
		reg >>= 8;
	}

	for (int i = 0; i < 4; i++) {
		if (i2c_smbus_write_byte_data(client, I2C_REG_WDATA0 + i,
					      value & 0xff))
			return -EIO;
		value >>= 8;
	}

	return 0;
}

static int get_mac_temp(struct device *dev, int type, int *temp)
{
	struct moc_fpga_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	u32 value = 0;
	u32 reg_1, reg_2;

	switch (type)
	{
	case FPGA_TEMP_MAC0:
		reg_1 = FPGA_REG_MAC0_REG1;
		reg_2 = FPGA_REG_MAC0_REG2;
		break;
	case FPGA_TEMP_MAC1:
		reg_1 = FPGA_REG_MAC1_REG1;
		reg_2 = FPGA_REG_MAC1_REG2;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&data->mutex_lock);

	if (moc_fpga_read_data(client, reg_2, &value)) {
		mutex_unlock(&data->mutex_lock);
		return -EIO;
	}
	if (value & 0x1) {
		mutex_unlock(&data->mutex_lock);
		return -EBUSY;
	}

	if (moc_fpga_write_data(client, reg_1, 0x40118)) {
		mutex_unlock(&data->mutex_lock);
		return -EIO;
	}

	if (moc_fpga_write_data(client, reg_2, 0xF01)) {
		mutex_unlock(&data->mutex_lock);
		return -EIO;
	}

	if (moc_fpga_read_data(client, fpga_temp_regs[type], &value)) {
		mutex_unlock(&data->mutex_lock);
		return -EIO;
	}
	mutex_unlock(&data->mutex_lock);

	/* bit8 is sign bit */
	int integer = value & 0x1FF;
	if (integer & 0x100)
		*temp = (integer - 0x200) * 1000;
	else
		*temp = integer * 1000;

	return 0;
}

static int get_sdm_temp(struct device *dev, int type, int* temp)
{
	struct moc_fpga_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	u32 value = 0;

	mutex_lock(&data->mutex_lock);
	if (moc_fpga_read_data(client, fpga_temp_regs[type], &value)) {
		mutex_unlock(&data->mutex_lock);
		return -EIO;
	}
	mutex_unlock(&data->mutex_lock);

	int integer = ((value & 0xff00) >> 8) * 1000;
	int decimal = (value & 0xff) > 99 ? 0 : (value & 0xff) * 10;

	*temp = integer + decimal;

	return 0;
}

static int get_temp(struct device *dev, int type, int* temp)
{
	switch (type)
	{
	case FPGA_TEMP_SDM:
		return get_sdm_temp(dev, FPGA_TEMP_SDM, temp);
	case FPGA_TEMP_MAC0:
	case FPGA_TEMP_MAC1:
		return get_mac_temp(dev, type, temp);
	}
	return -EINVAL;
}

static ssize_t temp_show(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);

	int temp = -256;

	if (attr->index == FPGA_TEMP_MAX) {
		bool got_temp = false;
		int tmp = -256;
		for (int i = 0; i < FPGA_TEMP_MAX; i++) {
			if (get_temp(dev, i, &tmp))
				continue;

			got_temp = true;
			if (tmp > temp) {
				temp = tmp;
			}
		}
		if (!got_temp)
			return -EIO;
	}
	else {
		if (get_temp(dev, attr->index, &temp))
			return -EIO;
	}

	return sprintf(buf, "%d\n", temp);
}

static ssize_t temp_label_show(struct device *dev, struct device_attribute *da,
			       char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	return sprintf(buf, "%s\n", fpga_temp_labels[attr->index]);
}

static ssize_t version_show(struct device *dev, struct device_attribute *da,
			    char *buf)
{
	struct moc_fpga_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	u32 value = 0;

	mutex_lock(&data->mutex_lock);
	if (moc_fpga_read_data(client, FPGA_REG_VERSION, &value)) {
		mutex_unlock(&data->mutex_lock);
		return -EIO;
	}
	mutex_unlock(&data->mutex_lock);

	int patch = value & 0xff;
	value >>= 8;
	int minor = value & 0xff;
	value >>= 8;
	int major = value & 0xff;

	return sprintf(buf, "%d.%d.%d\n", major, minor, patch);
}

static ssize_t chip_id_show(struct device *dev, struct device_attribute *da,
			    char *buf)
{
	struct moc_fpga_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	u32 lsb = 0;
	u32 msb = 0;

	mutex_lock(&data->mutex_lock);
	if (moc_fpga_read_data(client, FPGA_REG_CHIP_ID_LSB_REG, &lsb)) {
		mutex_unlock(&data->mutex_lock);
		return -EIO;
	}
	if (moc_fpga_read_data(client, FPGA_REG_CHIP_ID_MSB_REG, &msb)) {
		mutex_unlock(&data->mutex_lock);
		return -EIO;
	}
	mutex_unlock(&data->mutex_lock);

	return sprintf(buf, "0x%08X%08X\n", msb, lsb);
}

static SENSOR_DEVICE_ATTR_RO(temp1_input, temp, FPGA_TEMP_MAX);
static SENSOR_DEVICE_ATTR_RO(temp2_input, temp, FPGA_TEMP_SDM);
static SENSOR_DEVICE_ATTR_RO(temp3_input, temp, FPGA_TEMP_MAC0);
static SENSOR_DEVICE_ATTR_RO(temp4_input, temp, FPGA_TEMP_MAC1);
static SENSOR_DEVICE_ATTR_RO(temp1_label, temp_label, FPGA_TEMP_MAX);
static SENSOR_DEVICE_ATTR_RO(temp2_label, temp_label, FPGA_TEMP_SDM);
static SENSOR_DEVICE_ATTR_RO(temp3_label, temp_label, FPGA_TEMP_MAC0);
static SENSOR_DEVICE_ATTR_RO(temp4_label, temp_label, FPGA_TEMP_MAC1);

static SENSOR_DEVICE_ATTR_RO(version, version, 0);

static SENSOR_DEVICE_ATTR_RO(chip_id, chip_id, 0);

static struct attribute *moc_fpga_attrs[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_temp3_input.dev_attr.attr,
	&sensor_dev_attr_temp4_input.dev_attr.attr,
	&sensor_dev_attr_temp1_label.dev_attr.attr,
	&sensor_dev_attr_temp2_label.dev_attr.attr,
	&sensor_dev_attr_temp3_label.dev_attr.attr,
	&sensor_dev_attr_temp4_label.dev_attr.attr,
	&sensor_dev_attr_version.dev_attr.attr,
	&sensor_dev_attr_chip_id.dev_attr.attr,
	NULL
};

ATTRIBUTE_GROUPS(moc_fpga);

static int moc_fpga_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device *hwmon_dev;
	struct moc_fpga_data *data;

	data = devm_kzalloc(dev, sizeof(struct moc_fpga_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->mutex_lock);

	hwmon_dev = devm_hwmon_device_register_with_groups(
		dev, client->name, data, moc_fpga_groups);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id moc_fpga_id[] = { { "moc_fpga", 0 }, {} };

MODULE_DEVICE_TABLE(i2c, moc_fpga_id);

/* This is the driver that will be inserted */
static struct i2c_driver moc_fpga_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		   .name = "moc_fpga",
	},
	.probe_new = moc_fpga_probe,
	.id_table = moc_fpga_id,
};

module_i2c_driver(moc_fpga_driver);

MODULE_AUTHOR("Tianhui Xu <tianhui.xth@alibaba-inc.com>");
MODULE_DESCRIPTION("Alibaba MOC FPGA Driver");
MODULE_LICENSE("GPL");
