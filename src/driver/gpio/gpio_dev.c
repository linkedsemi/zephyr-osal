/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <driver/gpio/gpio_dev.h>
#include <zephyr/posix/poll.h>

LOG_MODULE_REGISTER(gpio_dev, LOG_LEVEL_ERR); // 注册日志模块

#define CONFIG_GPIO_MAX_PINS_PER_DEVICE 16
#define MAX_GPIO_DEVICES 15 /* The maximum number of GPIO devices */
#define GPIO_MNT_POINT "/dev/gpio"


#define DT_DRV_COMPAT linkedsemi_lsqsh_gpio

typedef struct gpio_device_info {
    const char *label;
    const struct device *dev;
    __u16 pin_count;
    time_t timestamp;
    atomic_t opened;
    const char **line_names;
} gpio_device_info;

struct linehandle_state {
    const struct gpio_device_info *gdev;
    __u32 lineoffsets[GPIOHANDLES_MAX];
    __u8 default_values[GPIOHANDLES_MAX];
    __u8 open_mode;
    __u32 flags;
    __u32 lines;
};

struct lineevent_state {
    const struct gpio_device_info *gdev;
    const char *label;
    // struct gpio_desc *desc;
};

typedef union {
    struct gpio_device_info chip;
    struct linehandle_state linehandle;
    struct lineevent_state lineevent;
} MyUnion;

typedef struct gpio_dev{
    int type;
    MyUnion gpio_union;
} gpio_dev;

__attribute__((section("PSRAM")))
struct gpio_dev linefd_dev[LINE_MAX];

__attribute__((section("PSRAM")))
struct gpio_dev lineevtfd_dev[LINEEVENT_MAX];

#define INIT_GPIO_LINE_NAMES_ARRAY(inst)                                        \
    static const char *gpio_line_names_##inst[] =  \
        DT_INST_PROP_OR(inst, gpio_line_names, {NULL});                           \

DT_INST_FOREACH_STATUS_OKAY(INIT_GPIO_LINE_NAMES_ARRAY)

static int init_gpio_line_names(void)
{
#define INIT_GPIO_LINE_NAMES_CHECK(inst)               \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, gpio_line_names) <= CONFIG_GPIO_MAX_PINS_PER_DEVICE, \
                 "gpio_line_names too long for instance " #inst); \
    LOG_DBG("Init_gpio_line_names gpio_line_names_" #inst " = %p", gpio_line_names_##inst); \
    LOG_DBG("Init_gpio_line_names gpio_line_names_" #inst "[0] = %s", gpio_line_names_##inst[0] ? gpio_line_names_##inst[0] : "(empty)");

    DT_INST_FOREACH_STATUS_OKAY(INIT_GPIO_LINE_NAMES_CHECK);

    return 0;
}

SYS_INIT(init_gpio_line_names, POST_KERNEL, 70);

#define GPIO_DEVICE_PROCESS(inst)                      \
{                                                      \
    .type = GPIOCHIP,                               \
    .gpio_union = {                                    \
        .chip = {                                       \
            .dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \
            .label = DT_INST_PROP(inst, label),         \
            .pin_count = DT_INST_PROP(inst, ngpios),    \
            .timestamp = 0,                             \
            .opened = ATOMIC_INIT(0),                   \
            .line_names = gpio_line_names_##inst,       \
        }                                               \
    }                                                   \
},

const struct gpio_dev gpio_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(GPIO_DEVICE_PROCESS)};

#define GPIOCHIP_NUM ARRAY_SIZE(gpio_devices)
BUILD_ASSERT(GPIOCHIP_NUM <= MAX_GPIO_DEVICES, "Exceeded maximum GPIO device count");

static int gpio_index = 0;

static struct fs_mount_t gpiofs_mnt = {
    .type = FS_GPIO,
    .mnt_point = GPIO_MNT_POINT,
    .fs_data = &gpio_devices,
};

void print_gpio_devices_info(void)
{
    for (int i = 0; i < GPIOCHIP_NUM; i++) {
        const struct gpio_device_info *chip = &gpio_devices[i].gpio_union.chip;

        LOG_DBG("print_gpio_devices_info gpio_devices[%d].label = %s, gpio_devices[%d].pin_count = %d, gpio_devices[%d].line_names = %p", i, chip->label, i, chip->pin_count, i, chip->line_names);

        if (chip->line_names) {
            for (int j = 0; j < chip->pin_count && j < 5; j++) {
                const char *name = chip->line_names[j];
                LOG_DBG("gpio_devices[%d].line_names[%d] = %s", i, j, name ? name : "(empty)");
            }
        } else {
            LOG_DBG("gpio_devices[%d].line_names is NULL", i);
        }
    }
}

int malloc_state(struct gpio_dev *client, struct gpio_dev *dev_array)
{
    if (!client || !dev_array) {
        LOG_ERROR("malloc_state Invalid input pointers.\n");
        return -1;
    }

    if (dev_array->type == LINEHANDLE) {
        for (int i = 0; i < LINE_MAX; i++) {
            if (dev_array[i].gpio_union.linehandle.gdev == NULL) {
                dev_array[i].gpio_union.linehandle.gdev = &client->gpio_union.chip;
                dev_array[i].type = client->type;

                LOG_DBG("Allocated state %d for device type: %d\n", i, dev_array[i].type);
                return i;
            }
        }
    } else if (dev_array->type == LINEEVT) {
        for (int i = 0; i < LINEEVENT_MAX; i++) {
            if (dev_array[i].gpio_union.lineevent.gdev == NULL) {
                dev_array[i].gpio_union.lineevent.gdev = &client->gpio_union.chip;
                dev_array[i].type = client->type;

                LOG_DBG("Allocated state %d for device type: %d\n", i, dev_array[i].type);
                return i;
            }
        }        
    } else {
        LOG_ERROR("Else device type: %d\n", dev_array->type);
        return -1; 
    }

    LOG_ERROR("No available states in the pool.\n");
    return -1;
}

// 提取文件名的函数
static const char *getFileName(const char *filePath)
{
    if (!filePath || strlen(filePath) == 0) {
        return NULL;
    }
    const char *lastSlash = strrchr(filePath, '/');
    if (lastSlash != NULL) {
        return lastSlash + 1;
    }
    return filePath;
}

static int gpiodev_filename_check(const char *file_name, uint8_t *type, uint8_t *index) {
    if (!file_name || strlen(file_name) == 0) {
        LOG_ERROR("Invalid file name\n");
        return -1;
    }
    if (strncmp(file_name, "/dev/gpio/gpiochip", 18) == 0) {
        int idx = -1;
        if (sscanf(file_name, "/dev/gpio/gpiochip%d", &idx) == 1 && idx >= 0) {
            *type = GPIOCHIP;
            *index = idx;
            LOG_DBG("Detected gpiochip device with index: %d\n", idx);
            return 0;
        } else {
            LOG_DBG("Invalid gpiochip format: %s\n", file_name);
            return -1;
        }
    }

    if (strncmp(file_name, "/dev/gpio/gpio-linehandle", 25) == 0) {
        int idx = -1;
        if (sscanf(file_name, "/dev/gpio/gpio-linehandle%d", &idx) == 1 && idx >= 0) {
            *type = LINEHANDLE;
            *index = idx;
            LOG_DBG("Detected GPIO line handle device with index: %d\n", idx);
            return 0;
        } else {
            LOG_ERROR("Invalid GPIO line handle format: %s\n", file_name);
            return -1;
        }
    }

    if (strncmp(file_name, "/dev/gpio/gpio-lineevent", 24) == 0) {
        int idx = -1;
        if (sscanf(file_name, "/dev/gpio/gpio-lineevent%d", &idx) == 1 && idx >= 0) {
            *type = LINEEVT;
            *index = idx;
            LOG_DBG("Detected GPIO lineevent device with index: %d\n", idx);
            return 0;
        } else {
            LOG_ERROR("Invalid GPIO lineevent format: %s\n", file_name);
            return -1;
        }
    }

    LOG_ERROR("Unknown file name format: %s\n", file_name);
    return -1;
}

static int gpiodev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
    LOG_DBG("Mode value: %x\n", mode);
    uint8_t type = 0;
    uint8_t idx = 0;

    // 判断 file_name 类型
    if (gpiodev_filename_check(file_name, &type, &idx) == 0) {
        LOG_DBG("Type: %u, Index: %u\n", type, idx);
    }

    if (type == GPIOCHIP) {
        LOG_DBG("GPIOCHIP Mode value: %x\n", mode);
        struct gpio_dev *client = NULL;
        // 遍历所有 GPIO 设备实例，找到对应的设备
        for (size_t i = 0; i < GPIOCHIP_NUM; i++) {
            const char *dev_name = gpio_devices[i].gpio_union.chip.label;
            const char *file_name_from_path = getFileName(file_name);
            LOG_DBG("Comparing: GPIOCHIP i=[%d], dev_name=%s, file_name_from_path=%s", i,
            dev_name, file_name_from_path);

            if (strcmp(gpio_devices[i].gpio_union.chip.label, getFileName(file_name)) == 0) {
                client = &gpio_devices[i];
                break;
            }
        }

        if (client == NULL) {
            LOG_ERROR("Error: gpio device with name '%s' not found\n", file_name);
            return -ENODEV;
        } 
        
        if (!atomic_cas(&client->gpio_union.chip.opened, 0, 1)) {
            LOG_ERROR("device name is opened");
            return -EINVAL;
        }

        if (!device_is_ready(client->gpio_union.chip.dev)) {
            LOG_ERROR("Error: GPIO device with name '%s' is not ready\n", file_name);
            atomic_set(&client->gpio_union.chip.opened, 0);
            return -ENODEV;
        }

        if (zfp->mp == NULL) {
            LOG_ERROR("Error: File system mount point is NULL\n");
            atomic_set(&client->gpio_union.chip.opened, 0);
            return -EINVAL;
        }

        zfp->filep = client;
    } else if (type == LINEHANDLE) {
        LOG_ERROR("LINEHANDLE Mode value: %x\n", mode);
        int idx = -1;
        if (strncmp(file_name, "/dev/gpio/gpio-linehandle", 25) == 0) {         
            if (sscanf(file_name, "/dev/gpio/gpio-linehandle%d", &idx) == 1 && idx >= 0) {
                LOG_DBG("Detected GPIO line handle device with index: %d\n", idx);
            } else {
                return -1;
            }
        }

        linefd_dev[idx].gpio_union.linehandle.open_mode = mode;
        zfp->filep = &linefd_dev[idx];
        if (linefd_dev[idx].gpio_union.linehandle.gdev == NULL) {
            LOG_ERROR("linefd_dev[idx].gpio_union.linehandle.gdev is a NULL pointer.\n");
            return -1;
        } 
        if (linefd_dev[idx].gpio_union.linehandle.gdev->dev == NULL) {
            LOG_ERROR("linefd_dev[idx].gpio_union.linehandle.gdev.dev is a NULL pointer.\n");
            return -1;
        } 
        LOG_DBG("linefd_dev[%d].gpio_union.linehandle.open_mode=[%d]!\n", idx, linefd_dev[idx].gpio_union.linehandle.open_mode);
    } else if (type == LINEEVT) {
        int idx = -1;
        if (strncmp(file_name, "/dev/gpio/gpio-lineevent", 24) == 0) {            
            if (sscanf(file_name, "/dev/gpio/gpio-lineevent%d", &idx) == 1 && idx >= 0) {
                LOG_DBG("Detected GPIO line event device with index: %d\n", idx);
            } else {
                return -1;
            }
        }

        zfp->filep = &lineevtfd_dev[idx];
    } else {
        LOG_ERROR("Unknown file name format: %s\n", file_name);
        return -EINVAL;
    }
    return 0;
}

static int gpiodev_close(struct fs_file_t *zfp)
{
    if (!zfp || !zfp->filep) {
        LOG_ERROR("No valid device to close.\n");
        return -EINVAL;
    }

    struct gpio_dev *client = zfp->filep;
    if (client->type == GPIOCHIP) {
        if (zfp->filep)
        {
            atomic_set(&client->gpio_union.chip.opened, 0);
            LOG_DBG("GPIOCHIP zfp->filep");
        }
        if (zfp->mp)
        {
            zfp->mp = NULL;
            LOG_DBG("GPIOCHIP Closed gpiofs mount");
        }
    } else if (client->type == LINEHANDLE) {
        LOG_DBG("client->gpio_union.linehandle.open_mode=[%d]!\n", client->gpio_union.linehandle.open_mode);
        if (client->gpio_union.linehandle.open_mode == 18) {
            LOG_DBG("LINEHANDLE zfp->filep is not set NULL!");
        } else {
            zfp->filep = NULL;
            client->gpio_union.linehandle.gdev = NULL;
            LOG_DBG("LINEHANDLE zfp->filep is set NULL!\n ");
        }
    } else if (client->type == LINEEVT) {
        zfp->filep = NULL;
        client->gpio_union.lineevent.gdev = NULL;
        LOG_DBG("LINEEVT zfp->filep");
    } else {
        return -EINVAL;
    }

    return 0;
}

static int gpiodev_ioctl(struct fs_file_t *zfp, unsigned long request, va_list args)
{
    int ret = -1;

    if (zfp == NULL) {
        LOG_ERROR("GPIO_GET_CHIPINFO_IOCTL [zfp == NULL]\n");
        return -EINVAL;
    }
    
    switch (request)
    {
        case GPIO_GET_CHIPINFO_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != GPIOCHIP) {
                LOG_ERROR("GPIO_GET_CHIPINFO_IOCTL [client == NULL] or [client->type != GPIOCHIP]\n");
                return -EINVAL;
            }

            struct gpiochip_info *chipinfo = va_arg(args, struct gpiochip_info *); 
            memset(chipinfo, 0, sizeof(*chipinfo));
            // 填充芯片信息
            strncpy(chipinfo->name, client->gpio_union.chip.dev->name, sizeof(chipinfo->name) - 1);
            strncpy(chipinfo->label, client->gpio_union.chip.label, sizeof(chipinfo->label) - 1);
            chipinfo->lines = client->gpio_union.chip.pin_count;
            return 0;
        }
    
        case GPIO_GET_LINEINFO_IOCTL: {// 需要修改，可能我进行挂载的时候要产生line的虚拟文件，按照pin的数量，产生lines
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != GPIOCHIP) {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL [client == NULL] or [client->type != GPIOCHIP]\n");
                return -EINVAL;
            }

            LOG_DBG(
                "GPIO_GET_LINEINFO_IOCTL Handling ioctl for chip label: %s, Current line_names array address: %p, Pin count for this chip: %d ",
                client->gpio_union.chip.label,
                client->gpio_union.chip.line_names,
                client->gpio_union.chip.pin_count);
            
            struct gpioline_info *line_info = va_arg(args, struct gpioline_info *);
            if (line_info == NULL) {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL [line_info == NULL]\n");
                return -EINVAL;
            }
            
            __u32 offset = line_info->line_offset;           
            LOG_DBG("GPIO_GET_LINEINFO_IOCTL Offset %d , pin count %d", offset, client->gpio_union.chip.pin_count);
            if (offset >= client->gpio_union.chip.pin_count) {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL Offset %d exceeds pin count %d", offset, client->gpio_union.chip.pin_count);
                return -EINVAL;
            }

            const char *name = client->gpio_union.chip.line_names[offset];
            if (name == NULL || name[0] == '\0') {
                line_info->name[0] = '\0';
            } else {
                strncpy(line_info->name, name, sizeof(line_info->name) - 1);
                line_info->name[sizeof(line_info->name) - 1] = '\0';
            }

            strncpy(line_info->consumer, "zephyr-gpio", sizeof(line_info->consumer) - 1);
            line_info->consumer[sizeof(line_info->consumer) - 1] = '\0';
            line_info->flags = GPIO_V2_LINE_FLAG_INPUT;

            LOG_DBG("Line %d name: %s", offset, line_info->name);
            return 0;
        }

        case GPIO_GET_LINEHANDLE_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != GPIOCHIP) {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL [client == NULL] or [client->type != GPIOCHIP]\n");
                return -EINVAL;
            }
           
            int idx = malloc_state(client, linefd_dev);
            if (idx < 0) {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL [idx < 0]\n");
                return -EINVAL;
            }

            char path[64];
            memset(path, 0x00, sizeof(path));
            snprintf(path, sizeof(path), "/dev/gpio/gpio-linehandle%d", idx);
            linefd_dev[idx].gpio_union.linehandle.gdev = &client->gpio_union.chip;
            if (linefd_dev[idx].gpio_union.linehandle.gdev == NULL) {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL gdev is a NULL pointer.\n");
                return -1;
            }
   
            if (linefd_dev[idx].gpio_union.linehandle.gdev->dev == NULL) {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL gdev.dev is a NULL pointer.\n");
                return -1;
            }

            linefd_dev[idx].type = LINEHANDLE;

            struct gpiohandle_request *req = va_arg(args, struct gpiohandle_request *);
            memcpy(linefd_dev[idx].gpio_union.linehandle.lineoffsets, req->lineoffsets, sizeof(req->lineoffsets));
            memcpy(linefd_dev[idx].gpio_union.linehandle.default_values, req->default_values, sizeof(req->default_values));
            linefd_dev[idx].gpio_union.linehandle.flags = req->flags;
            linefd_dev[idx].gpio_union.linehandle.lines = req->lines;

            req->fd = zvfs_open(path, O_CREAT, O_RDWR);
            if (req->fd < 0) {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL Failed to open file: %s, error code: %d\n", path, req->fd);
                return -1;
            }
            return 0;
        }

        case GPIOHANDLE_GET_LINE_VALUES_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;

            if (client && client->type != LINEHANDLE) {
                LOG_ERROR("GPIOHANDLE_GET_LINE_VALUES_IOCTL [client == NULL] or [client->type != LINEHANDLE]\n");
                return -EINVAL;
            }

            struct gpiohandle_data *data = va_arg(args, struct gpiohandle_data *);           
            __u32 port_value;
            ret = gpio_port_get(client->gpio_union.linehandle.gdev->dev, &port_value);
            if (ret < 0) {
                LOG_ERROR("GPIOHANDLE_GET_LINE_VALUES_IOCTL gpio_port_get fail, ret=[%d]\n", ret);
                return ret;
            }

            __u32 lines = client->gpio_union.linehandle.lines;
            for (int i = 0; i < lines; i++) {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE) {
                    return -EINVAL;
                }       
                // 提取第 offset 个引脚的状态
                data->values[i] = (port_value >> offset) & 0x1;
                LOG_DBG("GPIOHANDLE_GET_LINE_VALUES_IOCTL i =[%d], offset =[%u],data->values[i]=[%u], port_value=[%u]\n", i, offset, data->values[i], port_value);
            }
            return 0;
        }

        case GPIOHANDLE_SET_LINE_VALUES_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;           
            if (client && client->type != LINEHANDLE) {
                LOG_ERROR("GPIOHANDLE_SET_LINE_VALUES_IOCTL [client == NULL] or [client->type != LINEHANDLE]\n");
                return -EINVAL;
            }
            // client->gpio_union.linehandle.open_mode = 11;
            if (client->gpio_union.linehandle.gdev == NULL) {
                LOG_ERROR("GPIOHANDLE_SET_LINE_VALUES_IOCTL gdev is a NULL pointer.\n");
                return -1;
            }

            if (client->gpio_union.linehandle.gdev->dev == NULL) {
                LOG_ERROR("GPIOHANDLE_SET_LINE_VALUES_IOCTL gdev.dev is a NULL pointer.\n");
                return -1;
            }

            struct gpiohandle_data *data = va_arg(args, struct gpiohandle_data *);
            __u32 mask = 0;
            __u32 value = 0;
            __u32 lines = client->gpio_union.linehandle.lines;
            LOG_DBG("GPIOHANDLE_SET_LINE_VALUES_IOCTL lines = [%d]", lines);

            for (int i = 0; i < lines; i++) {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
                LOG_DBG("GPIOHANDLE_SET_LINE_VALUES_IOCTL i =[%d], offset =[%u]\n", i, offset);

                printk("%u ", data->values[i]);       
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE) {
                    LOG_ERROR("GPIOHANDLE_SET_LINE_VALUES_IOCTL Offset %u exceeds max pins per device (%d)", offset, CONFIG_GPIO_MAX_PINS_PER_DEVICE);
                    return -EINVAL;
                }
                
                mask |= BIT(offset); 
                // 设置值
                if (data->values[i]) {
                    value |= BIT(offset);
                    LOG_DBG("GPIOHANDLE_SET_LINE_VALUES_IOCTL Pin = %u & value = %u set to HIGH", offset,  value);
                } else {
                    value &= ~BIT(offset);
                    LOG_DBG("GPIOHANDLE_SET_LINE_VALUES_IOCTL Pin = %u & value = %u set to LOW", offset, value);
                }
            }

            LOG_DBG("GPIOHANDLE_SET_LINE_VALUES_IOCTL mask = 0x%x, value = 0x%x\n", mask, value);
            if (client->gpio_union.linehandle.gdev->dev != NULL) {
                ret = gpio_port_set_masked_raw(client->gpio_union.linehandle.gdev->dev, mask, value);
                if (ret < 0) {
                    LOG_ERROR("GPIOHANDLE_SET_LINE_VALUES_IOCTL gpio_port_set_masked_raw fail ret =[%d]\n", ret);
                    return ret;
                }
            } else {
                LOG_ERROR("GPIOHANDLE_SET_LINE_VALUES_IOCTL Error:gdev->dev is a NULL pointer.\n");
                return -1;
            }
            LOG_DBG("GPIOHANDLE_SET_LINE_VALUES_IOCTL mask = 0x%08x (%u), value = 0x%08x (%u)", mask, mask, value, value);
            return 0;
        }
        case GPIOHANDLE_SET_CONFIG_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != LINEHANDLE) {
                LOG_ERROR("GPIOHANDLE_SET_CONFIG_IOCTL [client == NULL] or [client->type != LINEHANDLE]\n");
                return -EINVAL;
            }

            struct gpiohandle_config *config = va_arg(args, struct gpiohandle_config *);
            __u32 lines = client->gpio_union.linehandle.lines;
            LOG_DBG("GPIOHANDLE_SET_CONFIG_IOCTL lines=[%d]\n", lines);

            for (int i = 0; i < lines; i++) {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
                LOG_DBG("GPIOHANDLE_SET_CONFIG_IOCTL i=[%d], offset=[%d], config->flags=[%d] ----\n", i, offset, config->flags);
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE) {
                    return -EINVAL;
                }
                ret = -1;
                ret = gpio_pin_configure(client->gpio_union.linehandle.gdev->dev, offset, config->flags);            
                if (ret < 0) {
                    LOG_ERROR("GPIOHANDLE_SET_CONFIG_IOCTL ret=[%d]\n", ret);
                    return ret;
                }
            }
            return 0;
        }
        case GPIO_GET_LINEEVENT_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != LINEEVT) {
                return -EINVAL;
            }
           
            // int idx = malloc_state(linefd_dev);
            // char path[64];
            // snprintf(path, sizeof(path), "/dev/gpio/gpio-linehandle%d", idx);
            // linefd_dev[idx].gpio_union.linehandle.gdev = client->gpio_union.chip.gdev;
            return 0;

        }
    default:
        LOG_ERROR("Unsupported ioctl request: %lu", request);
        return -ENOTTY;
    }
    return 0;
}

static int gpiodev_mount(struct fs_mount_t *mountp)
{
    // 检查挂载点是否为空
    if (mountp->mnt_point == NULL)
    {
        LOG_ERROR("Invalid mount point\n");
        return -EINVAL;
    }

    // 检查 GPIO 设备是否就绪
    for (int i = 0; i < GPIOCHIP_NUM; i++)
    {
        if (!device_is_ready(gpio_devices[i].gpio_union.chip.dev))
        {
            LOG_ERROR("GPIO device %d not ready\n", i);
            return -ENODEV;
        }
    }
    return 0;
}

static int gpiodev_unmount(struct fs_mount_t *mountp)
{
    if (mountp->mnt_point == NULL)
    {
        LOG_ERROR("Invalid mount point\n");
        return -EINVAL;
    }

    mountp->mnt_point = NULL;
    return 0;
}

static int gpiodev_opendir(struct fs_dir_t *dp, const char *path)
{
    if (strcmp(path, "/dev/gpio") != 0)
    {
        LOG_ERROR("Invalid directory path\n");
        return -EINVAL;
    }

    dp->dirp = &gpio_index;
    return 0;
}

static int gpiodev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
    int *index = dp->dirp;

    if (*index < GPIOCHIP_NUM)
    {
        // 构造文件条目信息
        snprintf(entry->name, sizeof(entry->name), "%s", gpio_devices[*index].gpio_union.chip.label);
        entry->type = FS_DIR_ENTRY_FILE;
        entry->size = 0; // GPIO 设备没有文件大小
        (*index)++;
        return 0;
    }

    // 没有更多条目
    memset(entry, 0, sizeof(struct fs_dirent));
    return 0;
}

static int gpiodev_closedir(struct fs_dir_t *dp)
{
    if (dp->dirp == NULL)
    {
        LOG_ERROR("Error: NULL pointer passed to gpiodev_closedir\n");
        return -EINVAL;
    }

    gpio_index = 0;
    dp->dirp = NULL;
    return 0;
}

static int gpiodev_stat(struct fs_mount_t *mountp,
                        const char *path, struct fs_dirent *entry)
{
    // 检查路径是否为挂载点
    if (strcmp(path, mountp->mnt_point) == 0)
    {
        // 挂载点是目录
        entry->type = FS_DIR_ENTRY_DIR;
        entry->size = 0;
    }
    else
    {
        // 检查是否为 GPIO 设备路径
        __u8 minor;
        if (sscanf(path, "gpiochip-%hhu", &minor) == 1 && minor < GPIOCHIP_NUM)
        {
            // 是 GPIO 设备
            entry->type = FS_DIR_ENTRY_FILE;
            entry->size = 0;
        }
        else
        {
            LOG_ERROR("Invalid path\n");
            return -EINVAL;
        }
    }

    LOG_DBG("GPIO device stat successfully\n");
    return 0;
}

/* File system interface */
static const struct fs_file_system_t gpiodev_fs = {
    .mount = gpiodev_mount,
    .unmount = gpiodev_unmount,
    .open = gpiodev_open,
    .close = gpiodev_close,
    .lseek = NULL,
    .ioctl = gpiodev_ioctl,       // 增加ioctl
    .opendir = gpiodev_opendir,   // fs ls访问
    .readdir = gpiodev_readdir,   // fs ls访问
    .closedir = gpiodev_closedir, // fs ls访问
    .stat = gpiodev_stat,         // fs cd访问
};


static int gpio_dev_init()
{

    LOG_DBG("gpiochip_num = %d\n", GPIOCHIP_NUM);

    for (int i=0; i < LINE_MAX; i++){
        linefd_dev[i].type = LINEHANDLE;
    };

    for (int i=0; i < LINEEVENT_MAX; i++){
        lineevtfd_dev[i].type = LINEEVT;
    };

	// 输出gpio设备name和label信息
	for (int i=0; i < GPIOCHIP_NUM; i++){
		if (gpio_devices[i].gpio_union.chip.dev->name != NULL){
			LOG_DBG("gpio_devices[%d].dev->name = %s\n", i, gpio_devices[i].gpio_union.chip.dev->name);
		}
		if (gpio_devices[i].gpio_union.chip.label != NULL){
			LOG_DBG("gpio_devices[%d].label = %s\n", i, gpio_devices[i].gpio_union.chip.label);
		}
	}

    gpiofs_mnt.mnt_point = "/dev/gpio";
    LOG_DBG("gpiofs_mnt: type: %d, mnt_point: %s, fs_data: %p\n", gpiofs_mnt.type, gpiofs_mnt.mnt_point, gpiofs_mnt.fs_data);

    int rc = fs_mount(&gpiofs_mnt);
    if (rc != 0)
    {
        LOG_ERROR("Error mounting as gpiofs: %d\n", rc);
        gpiofs_mnt.mnt_point = NULL;
        return rc;
    }

    return 0;
}

static int gpio_init(void)
{
    LOG_DBG("gpio_init start\n");
    print_gpio_devices_info();

    int rc = fs_register(FS_GPIO, &gpiodev_fs);
    if (rc < 0) {
        LOG_ERROR("gpiofs register error (%d)\n", rc);
    }

    rc = gpio_dev_init();
    if (rc != 0) {
        LOG_ERROR("gpiofs init failed\n");
        return rc;
    }

    LOG_DBG("gpio_init end\n");
    return rc;
}

// int gpio_main()
// {
//     gpio_init();
//     return 0;
// }

// 系统初始化
SYS_INIT(gpio_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
