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
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(gpio_dev, LOG_LEVEL_ERR); // 注册日志模块

#define CONFIG_GPIO_DEV_MAX_SIZE 8192
#define CONFIG_GPIO_MAX_PINS_PER_DEVICE 16

#define MAX_GPIO_DEVICES 14 /* The maximum number of GPIO devices */

#define GPIO_MNT_POINT "/dev"

typedef struct gpio_device_info {
    const char *label;        // 设备名称 (例如 "gpiochip0")
    const struct device *dev; // Zephyr GPIO 设备指针
    __u16 pin_count;       // GPIO 引脚数量
    time_t timestamp;         // 挂载时间戳
    atomic_t opened;          // 原子操作
    __u16 pin_mask;
} gpio_device_info;

struct linehandle_state {
    const struct gpio_device_info *gdev;
    __u32 lineoffsets[GPIOHANDLES_MAX];
    __u8 default_values[GPIOHANDLES_MAX];
    __u32 flags;
    __u32 lines;
};

struct lineevent_state {
    const struct gpio_device_info *gdev;
    const char *label;
    // struct gpio_desc *desc;
};

typedef union {
    struct gpiochip_info chip;
    struct linehandle_state linehandle;
    struct lineevent_state lineevent;
} MyUnion;

typedef struct gpio_dev{
    int type;
    MyUnion gpio_union;
} gpio_dev;

struct gpio_dev gpiodevs[] = {
    {
         .type = GPIOCHIP
    }
};

struct gpio_dev linefd_dev[LINE_MAX] ={
    {
        .type = LINEHANDLE
    }
};

struct gpio_dev lineevtfd_dev[LINEEVENT_MAX] = {
    {
        .type = LINEEVT
    }
};

#define DT_DRV_COMPAT linkedsemi_ls_gpio

#define GPIOCHIP_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(GPIOCHIP_NUM <= MAX_GPIO_DEVICES,
             "Exceeded maximum GPIO device count");


// 自动生成 GPIO 设备信息
#define GPIO_DEVICE_PROCESS(inst)                \
    {                                            \
        .dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \ 
        .label = DT_INST_PROP(inst, label),                 \     
        .pin_count = DT_INST_PROP(inst, ngpios), \
        .pin_mask = 0,                           \
        .timestamp = 0,                          \
        .opened = ATOMIC_INIT(0)},

struct gpio_device_info gpio_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(GPIO_DEVICE_PROCESS)};

// struct gpio_device_info gpio_devices[GPIOCHIP_NUM];

static int gpio_index = 0; // 当前遍历的 GPIO 设备索引

static struct fs_mount_t gpiofs_mnt = {
    .type = FS_GPIO,
    .mnt_point = GPIO_MNT_POINT, // 使用宏定义的挂载点路径
    .fs_data = &gpio_devices,
};

int malloc_state(struct gpio_dev *client, struct gpio_dev *dev)
{
    if (!client) {
        printf("Device pointer is NULL. Allocation failed.\n");
        return -1;
    }

    if (!dev) {
        printf("Device pointer is NULL. Allocation failed.\n");
        return -1;
    }
    if (dev->type = LINEHANDLE) {
        for (int i = 0; i < LINE_MAX; i++) {
            if (!dev->gpio_union.linehandle.gdev) {
                dev->gpio_union.linehandle.gdev = client->gpio_union.linehandle.gdev;
                printf("Allocated state %d for device type: %d\n", i, dev->type);
                return i;
            }
        }
    } else if (dev->type = LINEEVT) {
        for (int i = 0; i < LINEEVENT_MAX; i++) {
            if (!dev->gpio_union.lineevent.gdev) {
                dev->gpio_union.lineevent.gdev = client->gpio_union.lineevent.gdev;
                printf("Allocated state %d for device type: %d\n", i, dev->type);
                return i;
            }
        }        
    } else {
        printf("Allocated state %d for device type: %d\n", dev->type);
        return -1; 
    }

    printf("No available states in the pool.\n");
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

static int gpiodev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
    LOG_ERROR("gpiodev_open start----\n"); 

    struct gpio_dev *client = (struct gpio_dev *)zfp->filep;

    // todo 感觉这里还没有type信息呢，测试看看，不行的话就从filename中获取
    int type = client->type;
    if (type == GPIOCHIP) {
        struct  gpio_device_info *client = NULL;

        // 遍历所有 GPIO 设备实例，找到对应的设备
        for (size_t i = 0; i < GPIOCHIP_NUM; i++) {
            if (strcmp(gpio_devices[i].label, getFileName(file_name)) == 0) {
                client = &gpio_devices[i];
                break;
            }
        }

        if (client == NULL) {
            LOG_ERROR("Error: gpio device with name '%s' not found\n", file_name);
            return -ENODEV;
        }

        if (!atomic_cas(&client->opened, 0, 1)) {
            LOG_ERROR("device name is opened");
            return -EINVAL;
        }

        if (!device_is_ready(client->dev)) {
            LOG_ERROR("Error: GPIO device with name '%s' is not ready\n", file_name);
            atomic_set(&client->opened, 0);
            return -ENODEV;
        }

        if (zfp->mp == NULL) {
            LOG_ERROR("Error: File system mount point is NULL\n");
            atomic_set(&client->opened, 0);
            return -EINVAL;
        }

        zfp->filep = client;

    } else if (type == LINEHANDLE) {
        int idx = -1;
        if (strncmp(file_name, "/dev/gpio-linehandle", 20) == 0) {            
            if (sscanf(file_name, "/dev/gpio-linehandle%d", &idx) == 1 && idx >= 0) {
                printf("Detected GPIO line handle device with index: %d\n", idx);
            } else {
                return -1;
            }
        }
        // todo 需要测试一下看是否需要这个if条件
        if(mode & O_CREAT) {
            zfp->filep = &linefd_dev[idx];
        }
    } else if (type == LINEEVT) {
        int idx = -1;
        if (strncmp(file_name, "/dev/gpio-lineevent", 19) == 0) {            
            if (sscanf(file_name, "/dev/gpio-lineevent%d", &idx) == 1 && idx >= 0) {
                printf("Detected GPIO line event device with index: %d\n", idx);
            } else {
                return -1;
            }
        }
        // todo 需要测试一下看是否需要这个if条件
        if(mode & O_CREAT) {
            zfp->filep = &lineevtfd_dev[idx];
        }
    } else {
        LOG_ERROR("Unknown file name format: %s\n", file_name);
        return -EINVAL; // 格式错误        
    }

    LOG_ERROR("GPIO device with name '%s' opened successfully\n", file_name);
    return 0;
}

static int gpiodev_close(struct fs_file_t *zfp)
{
    LOG_ERROR("gpiodev_close start----\n");

    if (!zfp || !zfp->filep) {
        printf("No valid device to close.\n");
        return -EINVAL;
    }

    struct gpio_dev *client = (gpio_dev *)zfp->filep;

    if (client->type == GPIOCHIP) {
        if (zfp->filep)
        {
            struct gpio_device_info *client = zfp->filep;
            atomic_set(&client->opened, 0);
        }
        if (zfp->mp)
        {
            zfp->mp = NULL;
            LOG_ERROR("Closed gpiofs mount");
        }
    } else if (client->type == LINEHANDLE) {
        if (zfp->filep)
        {
            zfp->filep = NULL;
            client->gpio_union.linehandle.gdev = NULL;
        }
    } else if (client->type == LINEEVT) {
        if (zfp->filep)
        {
            zfp->filep = NULL;
            client->gpio_union.lineevent.gdev = NULL;
        }
    } else {
        return -EINVAL;
    }

    return 0;
}

static int gpiodev_ioctl(struct fs_file_t *zfp, unsigned long request, va_list args)
{
    LOG_ERROR("gpiodev_ioctl start----\n");

    switch (request)
    {
        case GPIO_GET_CHIPINFO_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != GPIOCHIP) {
                return -EINVAL;
            }

            struct gpiochip_info *chipinfo = va_arg(args, struct gpiochip_info *);     
            memset(chipinfo, 0, sizeof(*chipinfo));
            // 填充芯片信息
            strncpy(chipinfo->name, client->gpio_union.chip.gdev->dev->name, sizeof(chipinfo->name) - 1);
            chipinfo->lines = client->gpio_union.chip.gdev->pin_count;

            return 0;
        }
    
        case GPIO_GET_LINEINFO_IOCTL: {// 需要修改，可能我进行挂载的时候要产生line的虚拟文件，按照pin的数量，产生lines
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != GPIOCHIP) {
                return -EINVAL;
            }

            struct gpioline_info *line_info = va_arg(args, struct gpioline_info *);
            if (!line_info) {
                return -EINVAL;
            }

            // 填充线路信息
            memset(line_info, 0, sizeof(*line_info));
            line_info->line_offset = 0; // 示例：假设只支持一个 GPIO 线路
            strncpy(line_info->name, client->gpio_union.chip.gdev->dev->name, sizeof(line_info->name) - 1);
            // todo 确认一下是否使用consumer
            strncpy(line_info->consumer, "zephyr-gpio", sizeof(line_info->consumer) - 1);
            line_info->flags = GPIO_V2_LINE_FLAG_INPUT; // 示例：默认为输入模式
            return 0;
        }

        case GPIO_GET_LINEHANDLE_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != GPIOCHIP) {
                return -EINVAL;
            }
           
            int idx = malloc_state(client, linefd_dev);
            if (idx < 0) {
                return -EINVAL;
            }
            char path[64];
            snprintf(path, sizeof(path), "/dev/gpio-linehandle%d", idx);
            linefd_dev[idx].gpio_union.linehandle.gdev = client->gpio_union.chip.gdev;

            struct gpiohandle_request *req = va_arg(args, struct gpiohandle_request *);
            memcpy(linefd_dev[idx].gpio_union.linehandle.lineoffsets, req->lineoffsets, sizeof(req->lineoffsets));
            memcpy(linefd_dev[idx].gpio_union.linehandle.default_values, req->default_values, sizeof(req->default_values));
            linefd_dev[idx].gpio_union.linehandle.flags = req->flags;
            linefd_dev[idx].gpio_union.linehandle.lines = req->lines;

            req->fd = zvfs_open(path, O_CREAT, O_RDWR);
        }

        case GPIOHANDLE_GET_LINE_VALUES_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != LINEHANDLE) {
                return -EINVAL;
            }

            struct gpiohandle_data *data = va_arg(args, struct gpiohandle_data *);           
            __u32 port_value;
            int ret = gpio_port_get_raw(client->gpio_union.linehandle.gdev->dev, &port_value);
            if (ret < 0) {
                return ret;
            }

            uint32_t lines = client->gpio_union.linehandle.lines;
            for (int i = 0; i < lines; i++) {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE) {
                    return -EINVAL;
                }       
                // 提取第 offset 个引脚的状态
                data->values[i] = (port_value >> offset) & 0x1;
            }
        }

        case GPIOHANDLE_SET_LINE_VALUES_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != LINEHANDLE) {
                return -EINVAL;
            }

            struct gpiohandle_data *data = va_arg(args, struct gpiohandle_data *);
            uint32_t mask = 0;
            uint32_t value = 0;
            uint32_t lines = client->gpio_union.linehandle.lines;

            for (int i = 0; i < lines; i++) {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
        
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE) {
                    return -EINVAL;
                }
                mask |= (1 << offset);       
                // 设置值
                if (data->values[i]) {
                    value |= (1 << offset); // 设置为高电平
                } else {
                    value &= ~(1 << offset); // 设置为低电平
                }
            }

            int ret = gpio_port_set_masked_raw(client->gpio_union.linehandle.gdev->dev, mask, value);
            if (ret < 0) {
                return ret;
            }
        }
        case GPIOHANDLE_SET_CONFIG_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != LINEHANDLE) {
                return -EINVAL;
            }

            struct gpiohandle_config *config = va_arg(args, struct gpiohandle_config *);
         
            for (int i = 0; i < (client->gpio_union.linehandle.lines); i++) {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
        
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE) {
                    return -EINVAL;
                }
        
                int ret = gpio_pin_configure(client->gpio_union.linehandle.gdev->dev, offset, config->flags);
                if (ret < 0) {
                    return ret;
                }
            }
        }
        case GPIO_GET_LINEEVENT_IOCTL: {
            struct gpio_dev *client = (struct gpio_dev *)zfp->filep;
            if (client && client->type != LINEEVT) {
                return -EINVAL;
            }
           
            // int idx = malloc_state(linefd_dev);
            // char path[64];
            // snprintf(path, sizeof(path), "/dev/gpio-linehandle%d", idx);
            // linefd_dev[idx].gpio_union.linehandle.gdev = client->gpio_union.chip.gdev;

        }
    default:
        LOG_ERROR("Unsupported ioctl request: %lu", request);
        return -EINVAL;
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
        if (!device_is_ready(gpio_devices[i].dev))
        {
            LOG_ERROR("GPIO device %d not ready\n", i);
            return -ENODEV;
        }
    }

    LOG_ERROR("GPIO device mounted successfully\n");
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
    LOG_ERROR("GPIO device unmounted successfully\n");
    return 0;
}

static int gpiodev_opendir(struct fs_dir_t *dp, const char *path)
{
    if (strcmp(path, "/dev") != 0)
    {
        LOG_ERROR("Invalid directory path\n");
        return -EINVAL;
    }

    dp->dirp = &gpio_index;

    LOG_ERROR("GPIO device opendir successfully\n");
    return 0;
}

static int gpiodev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
    int *index = dp->dirp;

    if (*index < GPIOCHIP_NUM)
    {
        // 构造文件条目信息
        snprintf(entry->name, sizeof(entry->name), "%s", gpio_devices[*index].label);
        entry->type = FS_DIR_ENTRY_FILE;
        entry->size = 0; // GPIO 设备没有文件大小
        (*index)++;
        return 0;
    }

    // 没有更多条目
    memset(entry, 0, sizeof(struct fs_dirent));
    LOG_ERROR("No more GPIO devices to list\n");
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

    LOG_ERROR("GPIO device closedir successfully\n");
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

    LOG_ERROR("GPIO device stat successfully\n");
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
    LOG_ERROR("gpio /dev entries driver\n");

    LOG_ERROR("gpiochip_num = %d\n", GPIOCHIP_NUM);

    LOG_ERROR("gpio_devices[0].dev->name = %s\n", gpio_devices[0].dev->name);

    LOG_ERROR("gpio_devices[0].label = %s\n", gpio_devices[0].label);

    gpiofs_mnt.mnt_point = "/dev";
    LOG_ERROR("gpiofs_mnt: type: %d, mnt_point: %s, fs_data: %p\n", gpiofs_mnt.type, gpiofs_mnt.mnt_point, gpiofs_mnt.fs_data);

    int rc = fs_mount(&gpiofs_mnt);
    if (rc != 0)
    {
        LOG_ERROR("Error mounting as gpiofs: %d", rc);
        gpiofs_mnt.mnt_point = NULL;
        return rc;
    }

    return 0;
}

static int gpio_init(void)
{
    LOG_ERROR("gpio_init----------start\n");

    int rc = fs_register(FS_GPIO, &gpiodev_fs);
    if (rc < 0)
    {
        LOG_ERROR("gpiofs register error (%d)\n", rc);
    }
    else
    {
        LOG_ERROR("gpiofs fs registered\n");
    }

    rc = gpio_dev_init();
    if (rc != 0)
    {
        LOG_ERROR("gpiofs init failed\n");
        return rc;
    }

    LOG_ERROR("gpio_init----------end\n");
    return rc;
}

int gpio_main()
{
    gpio_init();
}

// // 系统初始化
// SYS_INIT(gpio_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
