// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ali-moc-fpga.c - The i2c driver for MOC FPGA
 *
 * The source file is linux/drivers/misc
 *
 * Copyright (c) 2024, Alibaba Co. Ltd. All rights reserved.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/hwmon/hwmon_moc_fpga.h"

LOG_MODULE_REGISTER(moc_fpga, LOG_LEVEL_ERR);

K_MUTEX_DEFINE(moc_fpga_lock);

#define DT_DRV_COMPAT moc_fpga

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


struct moc_fpga_data
{
    const struct device* i2c_dev;
    struct k_mutex lock;
};

struct moc_fpga_config
{
    const char* name;
    uint16_t i2c_addr;
};



static const char* fpga_temp_labels[] = {
    "SDM Temp", "MAC0 Temp", "MAC1 Temp", "Max Temp", NULL,
};

static int moc_fpga_read_data(const struct device* dev, uint16_t dev_addr, uint32_t reg,
                              uint32_t* value)
{
    LOG_DBG("Entering moc_fpga_read_data function with dev_addr: 0x%x, reg: 0x%x", dev_addr, reg);

    int ret;
    uint32_t reg_tmp = reg;
    uint8_t buf;

    *value = 0;

    ret = i2c_reg_write_byte(dev, dev_addr, I2C_REG_ENABLE, 0x1);
    if (ret)
    {
        return ret;
    }

    ret = i2c_reg_write_byte(dev, dev_addr, I2C_REG_RW, 0x2);
    if (ret)
    {
        return ret;
    }

    for (int i = 0; i < 4; i++)
    {
        ret = i2c_reg_write_byte(dev, dev_addr, I2C_REG_ADDR0 + i, reg_tmp & 0xff);
        if (ret)
        {
            return ret;
        }
        reg_tmp >>= 8;
    }

    for (int i = 0; i < 4; i++)
    {
        ret = i2c_reg_read_byte(dev, dev_addr, I2C_REG_READY, &buf);
        if (ret)
        {
            return ret;
        }

        if (buf & 0x1)
            break;

        if (i == 3)
        {
            LOG_WRN("read reg: 0x%x timeout", reg);
            return -EIO;
        }

        k_sleep(K_MSEC(10));
    }

    for (int i = 0; i < 4; i++)
    {
        *value <<= 8;
        ret = i2c_reg_read_byte(dev, dev_addr, I2C_REG_RDATA3 - i, &buf);
        if (ret)
        {
            return ret;
        }
        *value |= buf;
    }

    return 0;
}

static int moc_fpga_write_data(const struct device* dev, uint16_t dev_addr, uint32_t reg,
                               uint32_t value)
{
    LOG_DBG("Entering moc_fpga_write_data function with dev_addr: 0x%x, reg: 0x%x, value: 0x%x", 
            dev_addr, reg, value);

    int ret;

    ret = i2c_reg_write_byte(dev, dev_addr, I2C_REG_ENABLE, 0x1);
    if (ret)
    {
        return ret;
    }

    ret = i2c_reg_write_byte(dev, dev_addr, I2C_REG_RW, 0x1);
    if (ret)
    {
        return ret;
    }

    for (int i = 0; i < 4; i++)
    {
        ret = i2c_reg_write_byte(dev, dev_addr, I2C_REG_ADDR0 + i, reg & 0xff);
        if (ret)
        {
            return ret;
        }
        reg >>= 8;
    }

    for (int i = 0; i < 4; i++)
    {
        ret = i2c_reg_write_byte(dev, dev_addr, I2C_REG_WDATA0 + i, value & 0xff);
        if (ret)
        {
            return ret;
        }
        value >>= 8;
    }

    return 0;
}

static int get_mac_temp(const struct device* dev, uint16_t dev_addr, enum fpga_temp_type type,
                        int* temp)
{
    LOG_DBG("Entering get_mac_temp function with dev_addr: 0x%x, type: %d", dev_addr, type);

    uint32_t value = 0;
    uint32_t reg_1, reg_2;
    int ret;

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

    k_mutex_lock(&moc_fpga_lock, K_FOREVER);

    ret = moc_fpga_read_data(dev, dev_addr, reg_2, &value);
    if (ret)
    {
        k_mutex_unlock(&moc_fpga_lock);
        return ret;
    }

    if (value & 0x1)
    {
        k_mutex_unlock(&moc_fpga_lock);
        return -EBUSY;
    }

    ret = moc_fpga_write_data(dev, dev_addr, reg_1, 0x40118);
    if (ret)
    {
        k_mutex_unlock(&moc_fpga_lock);
        return ret;
    }

    ret = moc_fpga_write_data(dev, dev_addr, reg_2, 0xF01);
    if (ret)
    {
        k_mutex_unlock(&moc_fpga_lock);
        return ret;
    }

    ret = moc_fpga_read_data(dev, dev_addr, fpga_temp_regs[type], &value);
    if (ret)
    {
        k_mutex_unlock(&moc_fpga_lock);
        return ret;
    }

    k_mutex_unlock(&moc_fpga_lock);

    /* bit8 is sign bit */
    int integer = value & 0x1FF;
    if (integer & 0x100)
        *temp = (integer - 0x200) * 1000;
    else
        *temp = integer * 1000;

    return 0;
}

static int get_sdm_temp(const struct device* dev, uint16_t dev_addr, enum fpga_temp_type type,
                        int* temp)
{
    LOG_DBG("Entering get_sdm_temp function with dev_addr: 0x%x, type: %d", dev_addr, type);

    uint32_t value = 0;
    int ret;

    k_mutex_lock(&moc_fpga_lock, K_FOREVER);
    ret = moc_fpga_read_data(dev, dev_addr, fpga_temp_regs[type], &value);
    if (ret)
    {
        k_mutex_unlock(&moc_fpga_lock);
        return ret;
    }
    k_mutex_unlock(&moc_fpga_lock);

    int integer = ((value & 0xff00) >> 8) * 1000;
    int decimal = (value & 0xff) > 99 ? 0 : (value & 0xff) * 10;

    *temp = integer + decimal;

    return 0;
}

static int get_temp(const struct device* dev, uint16_t dev_addr, enum fpga_temp_type type,
                    int* temp)
{
    LOG_DBG("Entering get_temp function with dev_addr: 0x%x, type: %d", dev_addr, type);

    switch (type)
    {
        case FPGA_TEMP_SDM:
            return get_sdm_temp(dev, dev_addr, FPGA_TEMP_SDM, temp);
        case FPGA_TEMP_MAC0:
        case FPGA_TEMP_MAC1:
            return get_mac_temp(dev, dev_addr, type, temp);
    }
    return -EINVAL;
}
ssize_t moc_fpga_get_temperature(const struct device* dev, uint16_t dev_addr,
                                        enum fpga_temp_type type, char* buf)
{
    LOG_DBG("Entering moc_fpga_get_temperature function with dev_addr: 0x%x, type: %d", dev_addr, type);

    int temp = -256;

    if (type > FPGA_TEMP_MAX)
    {
        return -EINVAL;
    }

    if (type == FPGA_TEMP_MAX)
    {
        bool got_temp = false;
        int tmp = -256;
        for (int i = 0; i < FPGA_TEMP_MAX; i++)
        {
            if (get_temp(dev, dev_addr, i, &tmp))
                continue;

            got_temp = true;
            if (tmp > temp)
            {
                temp = tmp;
            }
        }
        if (!got_temp)
            return -EIO;
    }
    else
    {
        if (get_temp(dev, dev_addr, type, &temp))
            return -EIO;
    }

    return snprintf(buf, MOC_FPGA_BUF_SIZE, "%d\n", temp);
}

static ssize_t moc_fpga_get_temp_label(enum fpga_temp_type type, char* buf)
{
    LOG_DBG("Entering moc_fpga_get_temp_label function with type: %d", type);

    if (type > FPGA_TEMP_MAX)
    {
        return -1;
    }
    return snprintf(buf, MOC_FPGA_BUF_SIZE, "%s\n", fpga_temp_labels[type]);
}

ssize_t moc_fpga_get_version(const struct device* dev, uint16_t dev_addr, char* buf)
{
    LOG_DBG("Entering moc_fpga_get_version function with dev_addr: 0x%x", dev_addr);

    uint32_t value = 0;
    int ret;

    k_mutex_lock(&moc_fpga_lock, K_FOREVER);
    ret = moc_fpga_read_data(dev, dev_addr, FPGA_REG_VERSION, &value);
    if (ret)
    {
        k_mutex_unlock(&moc_fpga_lock);
        return ret;
    }
    k_mutex_unlock(&moc_fpga_lock);

	int patch = value & 0xff;
	value >>= 8;
	int minor = value & 0xff;
	value >>= 8;
	int major = value & 0xff;

    return snprintf(buf, MOC_FPGA_BUF_SIZE, "%d.%d.%d\n", major, minor, patch);
}

ssize_t chip_id_show(struct device* dev, uint16_t dev_addr, char* buf)
{
    LOG_DBG("Entering chip_id_show function with dev_addr: 0x%x", dev_addr);

    u32 lsb = 0;
    u32 msb = 0;

    k_mutex_lock(&moc_fpga_lock, K_FOREVER);
    if (moc_fpga_read_data(dev, dev_addr, FPGA_REG_CHIP_ID_LSB_REG, &lsb))
    {
        k_mutex_unlock(&moc_fpga_lock);
        return -EIO;
    }
    if (moc_fpga_read_data(dev, dev_addr, FPGA_REG_CHIP_ID_MSB_REG, &msb))
    {
        k_mutex_unlock(&moc_fpga_lock);
        return -EIO;
    }
    k_mutex_unlock(&moc_fpga_lock);

    return snprintf(buf, MOC_FPGA_BUF_SIZE, "0x%08X%08X\n", msb, lsb);
}
