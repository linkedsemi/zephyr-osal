#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <stdio.h>

#define I2C_SLAVE 0x0703 /* Use this slave address */

#define I2C_SLAVE_FORCE 0x0706 /* Use this slave address, even if it \
				  is already in use by a driver! */

#define I2C_RDWR 0x0707 /* Combined R/W transfer (one STOP only) */

#define CONFIG_I2C_DEV_MAX_SIZE 8192

#define MAX_I2C_DEVICES 14 /* The maximum number of I2C devices */

#define I2C_MNT_PONINT "/dev"

#define DT_DRV_COMPAT linkedsemi_ls_i2c

#define I2C_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

struct i2c_device_info
{
	uint16_t addr;
	const struct device *dev;
};

struct i2c_msg_linux
{
	uint16_t addr;
	uint8_t flags;
	uint32_t len;
	uint8_t *buf;
};
/* This is the structure as used in the I2C_RDWR ioctl call */
struct i2c_rdwr_ioctl_data
{
	struct i2c_msg_linux *msgs; /* pointers to i2c_msgs */
	uint32_t nmsgs;				/* number of i2c_msgs */
};

#define I2C_RDWR_IOCTL_MAX_MSGS 42

#define I2C_SMBUS_BLOCK_MAX 32 /* As specified in SMBus standard */