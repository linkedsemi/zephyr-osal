#include "driver/hwmon/hwmon.h"

#define MOC_FPGA_BUF_SIZE 32

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

struct moc_fpga_default_values { 
    const char *name;
    const char *default_value;
};

enum fpga_temp_type
{
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

enum moc_fpga_attrs {
    MOC_FPGA_TEMP_MAX_INPUT,
    MOC_FPGA_TEMP_MAX_LABEL,
    MOC_FPGA_TEMP_SDM_INPUT,
    MOC_FPGA_TEMP_SDM_LABEL,
    MOC_FPGA_TEMP_MAC0_INPUT,
    MOC_FPGA_TEMP_MAC0_LABEL,
    MOC_FPGA_TEMP_MAC1_INPUT,
    MOC_FPGA_TEMP_MAC1_LABEL,
    MOC_FPGA_VERSION,
    MOC_FPGA_CHIP_ID,
    MOC_FPGA_ATTR_COUNT
};

static const char *const moc_fpga_attr_names[MOC_FPGA_ATTR_COUNT] = {
    [MOC_FPGA_TEMP_MAX_INPUT] = "temp1_input",
    [MOC_FPGA_TEMP_MAX_LABEL] = "temp1_label",
    [MOC_FPGA_TEMP_SDM_INPUT] = "temp2_input",
    [MOC_FPGA_TEMP_SDM_LABEL] = "temp2_label",
    [MOC_FPGA_TEMP_MAC0_INPUT] = "temp3_input",
    [MOC_FPGA_TEMP_MAC0_LABEL] = "temp3_label",
    [MOC_FPGA_TEMP_MAC1_INPUT] = "temp4_input",
    [MOC_FPGA_TEMP_MAC1_LABEL] = "temp4_label",
    [MOC_FPGA_VERSION] = "version",
    [MOC_FPGA_CHIP_ID] = "chip_id"
};

ssize_t moc_fpga_get_temperature(const struct device* dev, uint16_t dev_addr,
                                        enum fpga_temp_type type, char* buf);
ssize_t moc_fpga_get_version(const struct device* dev, uint16_t dev_addr, char* buf);
ssize_t chip_id_show(struct device* dev, uint16_t dev_addr, char* buf);