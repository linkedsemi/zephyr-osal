#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <stdio.h>

/* form linux/i2c-dev.h */
#define I2C_RETRIES 0x0701 /* number of times a device address should \
				  be polled when not acknowledging */
#define I2C_TIMEOUT 0x0702 /* set timeout in units of 10 ms */

/* NOTE: Slave address is 7 or 10 bits, but 10-bit addresses
 * are NOT supported! (due to code brokenness)
 */
#define I2C_SLAVE 0x0703	   /* Use this slave address */
#define I2C_SLAVE_FORCE 0x0706 /* Use this slave address, even if it \
				  is already in use by a driver! */
#define I2C_TENBIT 0x0704	   /* 0 for 7 bit addrs, != 0 for 10 bit */

#define I2C_FUNCS 0x0705 /* Get the adapter functionality mask */

#define I2C_RDWR 0x0707 /* Combined R/W transfer (one STOP only) */

#define I2C_PEC 0x0708	 /* != 0 to use PEC with SMBus */
#define I2C_SMBUS 0x0720 /* SMBus transfer */

#define I2C_RDWR_IOCTL_MAX_MSGS 42
/* Originally defined with a typo, keep it for compatibility */
#define I2C_RDRW_IOCTL_MAX_MSGS I2C_RDWR_IOCTL_MAX_MSGS

// #define CONFIG_I2C_DEV_MAX_SIZE 8192

// #define MAX_I2C_DEVICES 14 /* The maximum number of I2C devices */

// #define I2C_MNT_PONINT "/dev"

// #define DT_DRV_COMPAT linkedsemi_ls_i2c

// #define I2C_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

struct i2c_device_info
{
	const struct device *dev;
	const char *label;
	uint16_t addr;
	atomic_t opened; // 原子操作
	uint8_t flags;
};

#define I2C_CLIENT_PEC 0x04			/* Use Packet Error Checking */
#define I2C_CLIENT_TEN 0x10			/* we have a ten bit chip address */
									/* Must equal I2C_M_TEN below */
#define I2C_CLIENT_SLAVE 0x20		/* we are the slave */
#define I2C_CLIENT_HOST_NOTIFY 0x40 /* We want to use I2C host notify */
#define I2C_CLIENT_WAKE 0x80		/* for board_info; true iff can wake */
#define I2C_CLIENT_SCCB 0x9000		/* Use Omnivision SCCB protocol */

#define I2C_RDWR_IOCTL_MAX_MSGS 42

#define I2C_SMBUS_BLOCK_MAX 32 /* As specified in SMBus standard */

/* Originally defined with a typo, keep it for compatibility */
#define I2C_RDRW_IOCTL_MAX_MSGS I2C_RDWR_IOCTL_MAX_MSGS

/* from linux/i2c.h*/
struct i2c_msg_linux
{
	uint16_t addr;
	uint8_t flags;
#define I2C_M_RD 0x0001			  /* guaranteed to be 0x0001! */
#define I2C_M_TEN 0x0010		  /* use only if I2C_FUNC_10BIT_ADDR */
#define I2C_M_DMA_SAFE 0x0200	  /* use only in kernel space */
#define I2C_M_RECV_LEN 0x0400	  /* use only if I2C_FUNC_SMBUS_READ_BLOCK_DATA */
#define I2C_M_NO_RD_ACK 0x0800	  /* use only if I2C_FUNC_PROTOCOL_MANGLING */
#define I2C_M_IGNORE_NAK 0x1000	  /* use only if I2C_FUNC_PROTOCOL_MANGLING */
#define I2C_M_REV_DIR_ADDR 0x2000 /* use only if I2C_FUNC_PROTOCOL_MANGLING */
#define I2C_M_NOSTART 0x4000	  /* use only if I2C_FUNC_NOSTART */
#define I2C_M_STOP 0x8000		  /* use only if I2C_FUNC_PROTOCOL_MANGLING */
	uint32_t len;
	uint8_t *buf;
};

/* To determine what functionality is present */
#define I2C_FUNC_I2C 0x00000001
#define I2C_FUNC_10BIT_ADDR 0x00000002		  /* required for I2C_M_TEN */
#define I2C_FUNC_PROTOCOL_MANGLING 0x00000004 /* required for I2C_M_IGNORE_NAK etc. */
#define I2C_FUNC_SMBUS_PEC 0x00000008
#define I2C_FUNC_NOSTART 0x00000010 /* required for I2C_M_NOSTART */
#define I2C_FUNC_SLAVE 0x00000020
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL 0x00008000 /* SMBus 2.0 or later */
#define I2C_FUNC_SMBUS_QUICK 0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE 0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE 0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA 0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA 0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA 0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA 0x00400000
#define I2C_FUNC_SMBUS_PROC_CALL 0x00800000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA 0x01000000 /* required for I2C_M_RECV_LEN */
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK 0x04000000  /* I2C-like block xfer  */
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK 0x08000000 /* w/ 1-byte reg. addr. */
#define I2C_FUNC_SMBUS_HOST_NOTIFY 0x10000000	  /* SMBus 2.0 or later */

#define I2C_FUNC_SMBUS_BYTE (I2C_FUNC_SMBUS_READ_BYTE | \
							 I2C_FUNC_SMBUS_WRITE_BYTE)
#define I2C_FUNC_SMBUS_BYTE_DATA (I2C_FUNC_SMBUS_READ_BYTE_DATA | \
								  I2C_FUNC_SMBUS_WRITE_BYTE_DATA)
#define I2C_FUNC_SMBUS_WORD_DATA (I2C_FUNC_SMBUS_READ_WORD_DATA | \
								  I2C_FUNC_SMBUS_WRITE_WORD_DATA)
#define I2C_FUNC_SMBUS_BLOCK_DATA (I2C_FUNC_SMBUS_READ_BLOCK_DATA | \
								   I2C_FUNC_SMBUS_WRITE_BLOCK_DATA)
#define I2C_FUNC_SMBUS_I2C_BLOCK (I2C_FUNC_SMBUS_READ_I2C_BLOCK | \
								  I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)

#define I2C_FUNC_SMBUS_EMUL (I2C_FUNC_SMBUS_QUICK |            \
							 I2C_FUNC_SMBUS_BYTE |             \
							 I2C_FUNC_SMBUS_BYTE_DATA |        \
							 I2C_FUNC_SMBUS_WORD_DATA |        \
							 I2C_FUNC_SMBUS_PROC_CALL |        \
							 I2C_FUNC_SMBUS_WRITE_BLOCK_DATA | \
							 I2C_FUNC_SMBUS_I2C_BLOCK |        \
							 I2C_FUNC_SMBUS_PEC)

/* if I2C_M_RECV_LEN is also supported */
#define I2C_FUNC_SMBUS_EMUL_ALL (I2C_FUNC_SMBUS_EMUL |            \
								 I2C_FUNC_SMBUS_READ_BLOCK_DATA | \
								 I2C_FUNC_SMBUS_BLOCK_PROC_CALL)

/*
 * Data for SMBus Messages
 */
#define I2C_SMBUS_BLOCK_MAX 32 /* As specified in SMBus standard */
union i2c_smbus_data
{
	uint8_t byte;
	uint16_t word;
	uint8_t block[I2C_SMBUS_BLOCK_MAX + 2]; /* block[0] is used for length */
											/* and one more for user-space compatibility */
};

/* i2c_smbus_xfer read or write markers */
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_WRITE 0

/* SMBus transaction types (size parameter in the above functions)
   Note: these no longer correspond to the (arbitrary) PIIX4 internal codes! */
#define I2C_SMBUS_QUICK 0
#define I2C_SMBUS_BYTE 1
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3
#define I2C_SMBUS_PROC_CALL 4
#define I2C_SMBUS_BLOCK_DATA 5
#define I2C_SMBUS_I2C_BLOCK_BROKEN 6
#define I2C_SMBUS_BLOCK_PROC_CALL 7 /* SMBus 2.0 */
#define I2C_SMBUS_I2C_BLOCK_DATA 8

/* This is the structure as used in the I2C_SMBUS ioctl call */
struct i2c_smbus_ioctl_data
{
	uint8_t read_write;
	uint32_t command;
	uint32_t size;
	union i2c_smbus_data *data;
};

/* This is the structure as used in the I2C_RDWR ioctl call */
struct i2c_rdwr_ioctl_data
{
	struct i2c_msg_linux *msgs; /* pointers to i2c_msgs */
	uint32_t nmsgs;				/* number of i2c_msgs */
};