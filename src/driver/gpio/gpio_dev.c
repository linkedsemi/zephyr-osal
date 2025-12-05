/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <driver/gpio/gpio_dev.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/posix/poll.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(gpio_dev, LOG_LEVEL_ERR); // 注册日志模块

#define CONFIG_GPIO_MAX_PINS_PER_DEVICE 16
#define MAX_GPIO_DEVICES 15 /* The maximum number of GPIO devices */
#define GPIO_MNT_POINT "/dev/gpio"

#define DT_DRV_COMPAT linkedsemi_lsqsh_gpio

typedef struct gpio_device_info
{
    const char* label;
    const struct device* dev;
    __u16 pin_count;
    time_t timestamp;
    atomic_t opened;
    const char** line_names;
} gpio_device_info;

struct linehandle_state
{
    const struct gpio_device_info* gdev;
    __u32 lineoffsets[GPIOHANDLES_MAX];
    __u8 default_values[GPIOHANDLES_MAX];
    __u8 open_mode;
    __u32 flags;
    __u32 lines;
};

struct lineevent_state
{
    const struct gpio_device_info* gdev; // 关联的 GPIO 芯片信息
    __u32 lineoffset[GPIOHANDLES_MAX];   // 监控的 line 偏移量（引脚号）
    __u32 lines;                         // 监控的 line 数量
    __u32 flags;                         // 事件触发标志（上升/下降/双边）
    struct gpio_callback gpio_cb;        // GPIO 中断回调结构体
    struct ring_buf event_buf;           // 事件缓存（避免丢失）
    uint8_t event_buf_data[256];         // 缓存数据区（可调整大小）最多16条
    struct k_poll_signal poll_signal;    // POLL 信号（唤醒上层 poll/epoll)
    atomic_t is_enabled;                 // 是否启用事件监控
    __u8 open_mode;
};

typedef union
{
    struct gpio_device_info chip;
    struct linehandle_state linehandle;
    struct lineevent_state lineevent;
} MyUnion;

typedef struct gpio_dev
{
    int type;
    MyUnion gpio_union;
} gpio_dev;

struct gpio_dev linefd_dev[LINE_MAX] = {{.type = LINEHANDLE}};

struct gpio_dev lineevtfd_dev[LINEEVENT_MAX] = {{.type = LINEEVT}};

#define INIT_GPIO_LINE_NAMES_ARRAY(inst)                                       \
    static const char* gpio_line_names_##inst[] =                              \
        DT_INST_PROP_OR(inst, gpio_line_names, {NULL});

DT_INST_FOREACH_STATUS_OKAY(INIT_GPIO_LINE_NAMES_ARRAY)

static int init_gpio_line_names(void)
{
#define INIT_GPIO_LINE_NAMES_CHECK(inst)                                       \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, gpio_line_names) <=                    \
                     CONFIG_GPIO_MAX_PINS_PER_DEVICE,                          \
                 "gpio_line_names too long for instance " #inst);              \
    LOG_DBG("Init_gpio_line_names gpio_line_names_" #inst " = %p",             \
            gpio_line_names_##inst);                                           \
    LOG_DBG("Init_gpio_line_names gpio_line_names_" #inst "[0] = %s",          \
            gpio_line_names_##inst[0] ? gpio_line_names_##inst[0]              \
                                      : "(empty)");

    DT_INST_FOREACH_STATUS_OKAY(INIT_GPIO_LINE_NAMES_CHECK);

    return 0;
}

SYS_INIT(init_gpio_line_names, POST_KERNEL, 70);

#define GPIO_DEVICE_PROCESS(inst)                                              \
    {.type = GPIOCHIP,                                                         \
     .gpio_union = {.chip = {                                                  \
                        .dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)),    \
                        .label = DT_INST_PROP(inst, label),                    \
                        .pin_count = DT_INST_PROP(inst, ngpios),               \
                        .timestamp = 0,                                        \
                        .opened = ATOMIC_INIT(0),                              \
                        .line_names = gpio_line_names_##inst,                  \
                    }}},

struct gpio_dev gpio_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(GPIO_DEVICE_PROCESS)};

#define GPIOCHIP_NUM ARRAY_SIZE(gpio_devices)
BUILD_ASSERT(GPIOCHIP_NUM <= MAX_GPIO_DEVICES,
             "Exceeded maximum GPIO device count");

static int gpio_index = 0;

static struct fs_mount_t gpiofs_mnt = {
    .type = FS_GPIO,
    .mnt_point = GPIO_MNT_POINT,
    .fs_data = &gpio_devices,
};

void print_gpio_devices_info(void)
{
    for (int i = 0; i < GPIOCHIP_NUM; i++)
    {
        const struct gpio_device_info* chip = &gpio_devices[i].gpio_union.chip;

        LOG_DBG(
            "print_gpio_devices_info gpio_devices[%d].label = %s, gpio_devices[%d].pin_count = %d, gpio_devices[%d].line_names = %p",
            i, chip->label, i, chip->pin_count, i, chip->line_names);

        if (chip->line_names)
        {
            for (int j = 0; j < chip->pin_count && j < 5; j++)
            {
                const char* name = chip->line_names[j];
                LOG_DBG("gpio_devices[%d].line_names[%d] = %s", i, j,
                        name ? name : "(empty)");
            }
        }
        else
        {
            LOG_DBG("gpio_devices[%d].line_names is NULL", i);
        }
    }
}

int malloc_state(struct gpio_dev* client, struct gpio_dev* dev_array)
{
    if (!client || !dev_array)
    {
        LOG_ERROR("malloc_state Invalid input pointers.\n");
        return -1;
    }
    LOG_DBG("malloc_state device type: %d\n", dev_array->type);

    if (dev_array->type == LINEHANDLE)
    {
        for (int i = 0; i < LINE_MAX; i++)
        {
            if (dev_array[i].gpio_union.linehandle.gdev == NULL)
            {
                dev_array[i].gpio_union.linehandle.gdev =
                    &client->gpio_union.chip;
                dev_array[i].type = client->type;

                LOG_DBG("Allocated state %d for device type: %d\n", i,
                        dev_array[i].type);
                return i;
            }
        }
    }
    else if (dev_array->type == LINEEVT)
    {
        for (int i = 0; i < LINEEVENT_MAX; i++)
        {
            if (dev_array[i].gpio_union.lineevent.gdev == NULL)
            {
                dev_array[i].gpio_union.lineevent.gdev =
                    &client->gpio_union.chip;
                dev_array[i].type = client->type;

                LOG_DBG("Allocated state %d for device type: %d\n", i,
                        dev_array[i].type);
                return i;
            }
        }
    }
    else
    {
        LOG_ERROR("Else device type: %d\n", dev_array->type);
        return -1;
    }

    LOG_ERROR("No available states in the pool.\n");
    return -1;
}

// 提取文件名的函数
static const char* getFileName(const char* filePath)
{
    if (!filePath || strlen(filePath) == 0)
    {
        return NULL;
    }
    const char* lastSlash = strrchr(filePath, '/');
    if (lastSlash != NULL)
    {
        return lastSlash + 1;
    }
    return filePath;
}

static int gpiodev_filename_check(const char* file_name, uint8_t* type,
                                  uint8_t* index)
{
    if (!file_name || strlen(file_name) == 0)
    {
        LOG_ERROR("Invalid file name\n");
        return -1;
    }
    if (strncmp(file_name, "/dev/gpio/gpiochip", 18) == 0)
    {
        int idx = -1;
        if (sscanf(file_name, "/dev/gpio/gpiochip%d", &idx) == 1 && idx >= 0)
        {
            *type = GPIOCHIP;
            *index = idx;
            LOG_DBG("Detected gpiochip device with index: %d\n", idx);
            return 0;
        }
        else
        {
            LOG_DBG("Invalid gpiochip format: %s\n", file_name);
            return -1;
        }
    }

    if (strncmp(file_name, "/dev/gpio/gpio-linehandle", 25) == 0)
    {
        int idx = -1;
        if (sscanf(file_name, "/dev/gpio/gpio-linehandle%d", &idx) == 1 &&
            idx >= 0)
        {
            *type = LINEHANDLE;
            *index = idx;
            LOG_DBG("Detected GPIO line handle device with index: %d\n", idx);
            return 0;
        }
        else
        {
            LOG_ERROR("Invalid GPIO line handle format: %s\n", file_name);
            return -1;
        }
    }

    if (strncmp(file_name, "/dev/gpio/gpio-lineevent", 24) == 0)
    {
        int idx = -1;
        if (sscanf(file_name, "/dev/gpio/gpio-lineevent%d", &idx) == 1 &&
            idx >= 0)
        {
            *type = LINEEVT;
            *index = idx;
            LOG_DBG("Detected GPIO lineevent device with index: %d\n", idx);
            return 0;
        }
        else
        {
            LOG_ERROR("Invalid GPIO lineevent format: %s\n", file_name);
            return -1;
        }
    }

    LOG_ERROR("Unknown file name format: %s\n", file_name);
    return -1;
}

// 从 zfp->filep 中提取 lineevent 索引
static int gpiodev_get_lineevt_idx(struct fs_file_t* zfp)
{
    if (!zfp || !zfp->filep)
    {
        LOG_ERROR("get_lineevt_idx: invalid params (zfp=%p, filep=%p)", zfp,
                  zfp->filep);
        return -1;
    }

    // 遍历 lineevtfd_dev 数组，找到 zfp->filep 对应的索引
    for (int i = 0; i < LINEEVENT_MAX; i++)
    {
        if (zfp->filep == &lineevtfd_dev[i])
        {
            return i; // 返回索引
        }
    }

    LOG_ERROR("lineevent instance not found (filep=%p)", zfp->filep);
    return -1;
}

static int gpiodev_open(struct fs_file_t* zfp, const char* file_name,
                        fs_mode_t mode)
{
    LOG_DBG("Mode value: %x\n", mode);
    uint8_t type = 0;
    uint8_t idx = 0;

    // 判断 file_name 类型
    if (gpiodev_filename_check(file_name, &type, &idx) == 0)
    {
        LOG_DBG("Type: %u, Index: %u\n", type, idx);
    }

    if (type == GPIOCHIP)
    {
        LOG_DBG("GPIOCHIP Mode value: %x\n", mode);
        struct gpio_dev* client = NULL;
        // 遍历所有 GPIO 设备实例，找到对应的设备
        for (size_t i = 0; i < GPIOCHIP_NUM; i++)
        {
            const char* dev_name = gpio_devices[i].gpio_union.chip.label;
            const char* file_name_from_path = getFileName(file_name);
            LOG_DBG(
                "Comparing: GPIOCHIP i=[%d], dev_name=%s, file_name_from_path=%s",
                i, dev_name, file_name_from_path);

            if (strcmp(gpio_devices[i].gpio_union.chip.label,
                       getFileName(file_name)) == 0)
            {
                client = &gpio_devices[i];
                break;
            }
        }

        if (client == NULL)
        {
            LOG_ERROR("Error: gpio device with name '%s' not found\n",
                      file_name);
            return -ENODEV;
        }

        if (!atomic_cas(&client->gpio_union.chip.opened, 0, 1))
        {
            LOG_ERROR("device name is opened");
            return -EINVAL;
        }

        if (!device_is_ready(client->gpio_union.chip.dev))
        {
            LOG_ERROR("Error: GPIO device with name '%s' is not ready\n",
                      file_name);
            atomic_set(&client->gpio_union.chip.opened, 0);
            return -ENODEV;
        }

        if (zfp->mp == NULL)
        {
            LOG_ERROR("Error: File system mount point is NULL\n");
            atomic_set(&client->gpio_union.chip.opened, 0);
            return -EINVAL;
        }

        zfp->filep = client;
    }
    else if (type == LINEHANDLE)
    {
        LOG_DBG("LINEHANDLE Mode value: %x\n", mode);
        int idx = -1;
        if (strncmp(file_name, "/dev/gpio/gpio-linehandle", 25) == 0)
        {
            if (sscanf(file_name, "/dev/gpio/gpio-linehandle%d", &idx) == 1 &&
                idx >= 0)
            {
                LOG_DBG("Detected GPIO line handle device with index: %d\n",
                        idx);
            }
            else
            {
                return -1;
            }
        }

        linefd_dev[idx].gpio_union.linehandle.open_mode = mode;
        zfp->filep = &linefd_dev[idx];
        if (linefd_dev[idx].gpio_union.linehandle.gdev == NULL)
        {
            LOG_ERROR(
                "linefd_dev[idx].gpio_union.linehandle.gdev is a NULL pointer.\n");
            return -1;
        }
        if (linefd_dev[idx].gpio_union.linehandle.gdev->dev == NULL)
        {
            LOG_ERROR(
                "linefd_dev[idx].gpio_union.linehandle.gdev.dev is a NULL pointer.\n");
            return -1;
        }
        LOG_DBG("linefd_dev[%d].gpio_union.linehandle.open_mode=[%d]!\n", idx,
                linefd_dev[idx].gpio_union.linehandle.open_mode);
    }
    else if (type == LINEEVT)
    {
        int idx = -1;
        if (strncmp(file_name, "/dev/gpio/gpio-lineevent", 24) == 0)
        {
            if (sscanf(file_name, "/dev/gpio/gpio-lineevent%d", &idx) == 1 &&
                idx >= 0)
            {
                LOG_DBG("Detected GPIO line event device with index: %d\n",
                        idx);
            }
            else
            {
                return -1;
            }
        }

        lineevtfd_dev[idx].gpio_union.lineevent.open_mode = mode;
        zfp->filep = &lineevtfd_dev[idx];
        if (lineevtfd_dev[idx].gpio_union.lineevent.gdev == NULL)
        {
            LOG_ERROR(
                "lineevtfd_dev[idx].gpio_union.lineevent.gdev is a NULL pointer.\n");
            return -1;
        }
        if (lineevtfd_dev[idx].gpio_union.lineevent.gdev->dev == NULL)
        {
            LOG_ERROR(
                "lineevtfd_dev[idx].gpio_union.lineevent.gdev.dev is a NULL pointer.\n");
            return -1;
        }
    }
    else
    {
        LOG_ERROR("Unknown file name format: %s\n", file_name);
        return -EINVAL;
    }
    return 0;
}

/**
 * @brief 释放 lineevent 实例资源
 * @param idx lineevent 实例在 lineevtfd_dev 中的索引
 */
static void lineevent_release_instance(int idx)
{
    if (idx < 0 || idx >= LINEEVENT_MAX)
    {
        LOG_ERROR("lineevent_release_instance: invalid idx");
        return;
    }
    struct lineevent_state* evt_state =
        &lineevtfd_dev[idx].gpio_union.lineevent;

    // 禁用事件监控
    atomic_set(&evt_state->is_enabled, 0);

    // 注销 GPIO 中断回调
    if (device_is_ready(evt_state->gdev->dev))
    {
        gpio_remove_callback(evt_state->gdev->dev, &evt_state->gpio_cb);
    }

    // 重置 poll 信号和缓存
    k_poll_signal_reset(&evt_state->poll_signal);
    ring_buf_reset(&evt_state->event_buf);

    // 标记实例为空闲
    memset(evt_state, 0, sizeof(struct lineevent_state));

    LOG_DBG("lineevent: release instance (idx=%d)", idx);
}

static int gpiodev_close(struct fs_file_t* zfp)
{
    if (!zfp || !zfp->filep)
    {
        LOG_ERROR("No valid device to close.\n");
        return -EINVAL;
    }

    struct gpio_dev* client = zfp->filep;
    if (client->type == GPIOCHIP)
    {
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
    }
    else if (client->type == LINEHANDLE)
    {
        if (client->gpio_union.linehandle.open_mode == 18)
        {
            LOG_DBG("LINEHANDLE zfp->filep is not set NULL!");
        }
        else
        {
            zfp->filep = NULL;
            client->gpio_union.linehandle.gdev = NULL;
            LOG_DBG("LINEHANDLE zfp->filep is set NULL!\n ");
        }
    }
    else if (client->type == LINEEVT)
    {
        // 仅在非特殊模式下执行释放
        if (client->gpio_union.lineevent.open_mode == 18)
        {
            LOG_DBG(
                "gpiodev_close: LINEEVT skip release (special open_mode=18)");
        }
        else
        {
            // 通过辅助函数获取索引
            int idx = gpiodev_get_lineevt_idx(zfp);

            // 释放 LINEEVT 实例资源（中断、回调、缓存等）
            if (idx >= 0)
            {
                lineevent_release_instance(idx);
            }
            else
            {
                LOG_ERROR("LINEEVT instance not found (invalid client)");
            }

            // 重置关联指针，避免野指针访问
            client->gpio_union.lineevent.gdev = NULL;
            zfp->filep = NULL;
        }
    }
    else
    {
        return -EINVAL;
    }

    return 0;
}

static void lineevent_gpio_isr(const struct device* dev,
                               struct gpio_callback* cb, uint32_t pins)
{
    struct lineevent_state* evt_state = CONTAINER_OF(cb, struct lineevent_state,
                                                     gpio_cb);
    struct gpioevent_data event = {0};
    int ret;
    if (!atomic_get(&evt_state->is_enabled))
    {
        return;
    }

    uint64_t uptime_ms = k_uptime_get();
    __u64 uptime_ns = uptime_ms * NSEC_PER_MSEC;
    event.timestamp = uptime_ns;

    for (__u32 i = 0; i < evt_state->lines; i++)
    {
        __u32 pin = evt_state->lineoffset[i];
        if (pin >= evt_state->gdev->pin_count)
        {
            LOG_WRN("lineevent: pin %d out of range (max=%d)", pin,
                    evt_state->gdev->pin_count);
            continue;
        }

        if (pins & BIT(pin))
        {
            if (evt_state->flags & GPIOEVENT_REQUEST_RISING_EDGE &&
                evt_state->flags & GPIOEVENT_REQUEST_FALLING_EDGE)
            {
                if (!device_is_ready(evt_state->gdev->dev))
                {
                    LOG_WRN("lineevent: GPIO device not ready (pin=%d)", pin);
                    continue;
                }

                bool pin_val = gpio_pin_get(evt_state->gdev->dev, pin);
                if (pin_val)
                {
                    event.id = GPIOEVENT_EVENT_RISING_EDGE;
                }
                else
                {
                    event.id = GPIOEVENT_EVENT_FALLING_EDGE;
                }
            }
            else if (evt_state->flags & GPIOEVENT_REQUEST_RISING_EDGE)
            {
                event.id = GPIOEVENT_EVENT_RISING_EDGE;
            }
            else if (evt_state->flags & GPIOEVENT_REQUEST_FALLING_EDGE)
            {
                event.id = GPIOEVENT_EVENT_FALLING_EDGE;
            }

            ret = ring_buf_put(&evt_state->event_buf, (const uint8_t*)&event,
                               sizeof(event));
            if (ret != sizeof(event))
            {
                LOG_WRN(
                    "lineevent: event buffer full, drop event (pin=%d, id=%d)",
                    pin, event.id);
                continue;
            }

            k_poll_signal_raise(&evt_state->poll_signal, 1);
        }
    }
}

// gpiodev_read（完全用索引访问 lineevtfd_dev）
static ssize_t gpiodev_read(struct fs_file_t* zfp, void* buf, size_t len)
{
    if (zfp == NULL || buf == NULL || len == 0)
    {
        return -EINVAL;
    }

    // 获取 lineevent 索引
    int idx = gpiodev_get_lineevt_idx(zfp);
    if (idx < 0 || idx >= LINEEVENT_MAX || lineevtfd_dev[idx].type != LINEEVT)
    {
        return -EINVAL;
    }

    // 通过索引访问 lineevtfd_dev 数组
    struct lineevent_state* evt_state =
        &lineevtfd_dev[idx].gpio_union.lineevent;

    // 确保 GPIO 设备资源有效
    if (evt_state->gdev == NULL || evt_state->gdev->dev == NULL)
    {
        return -EINVAL;
    }

    ssize_t total_read = 0;

    // 循环读取缓存中的事件，直到缓冲区满或无事件
    while (total_read + sizeof(struct gpioevent_data) <= len)
    {
        struct gpioevent_data event;
        ssize_t ret = ring_buf_get(
            &evt_state->event_buf,
            (uint8_t*)&event, // 强制转换：struct gpioevent_data* → uint8_t*
            sizeof(event));

        // 无更多事件，退出循环
        if (ret != sizeof(event))
        {
            break;
        }

        // 拷贝事件到上层缓冲区
        memcpy((uint8_t*)buf + total_read, &event, sizeof(event));
        total_read += sizeof(event);

        LOG_DBG("gpiodev_read: event (idx=%d, id=%d, ts=%lld ns)", idx,
                event.id, (long long)event.timestamp);
    }

    // 返回实际读取长度（0表示无事件，正数表示成功）
    return total_read;
}

static int gpiodev_ioctl(struct fs_file_t* zfp, unsigned long request,
                         va_list args)
{
    int ret = -1;

    if (zfp == NULL)
    {
        LOG_ERROR("GPIO_GET_CHIPINFO_IOCTL [zfp == NULL]\n");
        return -EINVAL;
    }

    switch (request)
    {
        case GPIO_GET_CHIPINFO_IOCTL:
        {
            struct gpio_dev* client = (struct gpio_dev*)zfp->filep;
            if (client == NULL || client->type != GPIOCHIP)
            {
                LOG_ERROR(
                    "GPIO_GET_CHIPINFO_IOCTL [client == NULL] or [client->type != GPIOCHIP]\n");
                return -EINVAL;
            }

            struct gpiochip_info* chipinfo = va_arg(args,
                                                    struct gpiochip_info*);
            memset(chipinfo, 0, sizeof(*chipinfo));
            // 填充芯片信息
            strncpy(chipinfo->name, client->gpio_union.chip.dev->name,
                    sizeof(chipinfo->name) - 1);
            strncpy(chipinfo->label, client->gpio_union.chip.label,
                    sizeof(chipinfo->label) - 1);
            chipinfo->lines = client->gpio_union.chip.pin_count;
            return 0;
        }

        case GPIO_GET_LINEINFO_IOCTL:
        {
            struct gpio_dev* client = (struct gpio_dev*)zfp->filep;
            if (client == NULL || client->type != GPIOCHIP)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEINFO_IOCTL [client == NULL] or [client->type != GPIOCHIP]\n");
                return -EINVAL;
            }

            LOG_DBG(
                "GPIO_GET_LINEINFO_IOCTL Handling ioctl for chip label: %s, Current line_names array address: %p, Pin count for this chip: %d ",
                client->gpio_union.chip.label,
                client->gpio_union.chip.line_names,
                client->gpio_union.chip.pin_count);

            struct gpioline_info* line_info = va_arg(args,
                                                     struct gpioline_info*);
            if (line_info == NULL)
            {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL [line_info == NULL]\n");
                return -EINVAL;
            }

            __u32 offset = line_info->line_offset;
            LOG_DBG("GPIO_GET_LINEINFO_IOCTL Offset %d , pin count %d", offset,
                    client->gpio_union.chip.pin_count);
            if (offset >= client->gpio_union.chip.pin_count)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEINFO_IOCTL Offset %d exceeds pin count %d",
                    offset, client->gpio_union.chip.pin_count);
                return -EINVAL;
            }

            const char* name = client->gpio_union.chip.line_names[offset];
            if (name == NULL || name[0] == '\0')
            {
                line_info->name[0] = '\0';
            }
            else
            {
                strncpy(line_info->name, name, sizeof(line_info->name) - 1);
                line_info->name[sizeof(line_info->name) - 1] = '\0';
            }

            strncpy(line_info->consumer, "zephyr-gpio",
                    sizeof(line_info->consumer) - 1);
            line_info->consumer[sizeof(line_info->consumer) - 1] = '\0';
            line_info->flags = GPIO_V2_LINE_FLAG_INPUT;

            LOG_DBG("Line %d name: %s", offset, line_info->name);
            return 0;
        }

        case GPIO_GET_LINEHANDLE_IOCTL:
        {
            struct gpio_dev* client = (struct gpio_dev*)zfp->filep;
            if (client == NULL || client->type != GPIOCHIP)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEHANDLE_IOCTL [client == NULL] or [client->type != GPIOCHIP]\n");
                return -EINVAL;
            }

            struct gpiohandle_request* req = va_arg(args,
                                                    struct gpiohandle_request*);
            if (req->lines == 0 || req->lines > CONFIG_GPIO_MAX_PINS_PER_DEVICE)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEHANDLE_IOCTL invalid lines=[%u] (must 1~%d)\n",
                    req->lines, CONFIG_GPIO_MAX_PINS_PER_DEVICE);
                return -EINVAL;
            }
            for (int i = 0; i < req->lines; i++)
            {
                __u32 offset = req->lineoffsets[i];
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE ||
                    offset == (unsigned int)-1)
                {
                    LOG_ERROR(
                        "GPIO_GET_LINEHANDLE_IOCTL i=[%d] offset=[%u] exceed max=[%d]\n",
                        i, offset, CONFIG_GPIO_MAX_PINS_PER_DEVICE - 1);
                    return -EINVAL;
                }
            }

            int idx = malloc_state(client, linefd_dev);
            if (idx < 0)
            {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL [idx < 0]\n");
                return -EINVAL;
            }

            char path[64];
            memset(path, 0x00, sizeof(path));
            snprintf(path, sizeof(path), "/dev/gpio/gpio-linehandle%d", idx);
            linefd_dev[idx].gpio_union.linehandle.gdev =
                &client->gpio_union.chip;
            if (linefd_dev[idx].gpio_union.linehandle.gdev == NULL)
            {
                LOG_ERROR("GPIO_GET_LINEINFO_IOCTL gdev is a NULL pointer.\n");
                return -1;
            }

            if (linefd_dev[idx].gpio_union.linehandle.gdev->dev == NULL)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEINFO_IOCTL gdev.dev is a NULL pointer.\n");
                return -1;
            }

            linefd_dev[idx].type = LINEHANDLE;
            memcpy(linefd_dev[idx].gpio_union.linehandle.lineoffsets,
                   req->lineoffsets, sizeof(req->lineoffsets));
            memcpy(linefd_dev[idx].gpio_union.linehandle.default_values,
                   req->default_values, sizeof(req->default_values));
            linefd_dev[idx].gpio_union.linehandle.flags = req->flags;
            linefd_dev[idx].gpio_union.linehandle.lines = req->lines;

            req->fd = zvfs_open(path, O_CREAT, O_RDWR);
            if (req->fd < 0)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEINFO_IOCTL Failed to open file: %s, error code: %d\n",
                    path, req->fd);
                return -1;
            }
            return 0;
        }
        case GPIOHANDLE_GET_LINE_VALUES_IOCTL:
        {
            struct gpio_dev* client = (struct gpio_dev*)zfp->filep;
            if (client == NULL || client->type != LINEHANDLE)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEHANDLE_IOCTL [client == NULL] or [client->type != LINEHANDLE]\n");
                return -EINVAL;
            }

            struct gpiohandle_data* data = va_arg(args,
                                                  struct gpiohandle_data*);
            __u32 port_value;
            ret = gpio_port_get(client->gpio_union.linehandle.gdev->dev,
                                &port_value);
            if (ret < 0)
            {
                LOG_ERROR(
                    "GPIOHANDLE_GET_LINE_VALUES_IOCTL gpio_port_get fail, ret=[%d]\n",
                    ret);
                return ret;
            }

            __u32 lines = client->gpio_union.linehandle.lines;
            if (lines == 0 || lines > CONFIG_GPIO_MAX_PINS_PER_DEVICE)
            {
                LOG_ERROR(
                    "GPIOHANDLE_SET_LINE_VALUES_IOCTL invalid lines=[%u]\n",
                    lines);
                return -EINVAL;
            }
            for (int i = 0; i < lines; i++)
            {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE ||
                    offset == (unsigned int)-1)
                {
                    LOG_ERROR(
                        "GPIOHANDLE_GET_LINE_VALUES_IOCTL i=[%d] offset=[%u] exceed max=[%d]\n",
                        i, offset, CONFIG_GPIO_MAX_PINS_PER_DEVICE - 1);
                    return -EINVAL;
                }
                // 提取第 offset 个引脚的状态
                data->values[i] = (port_value >> offset) & 0x1;
                LOG_DBG(
                    "GPIOHANDLE_GET_LINE_VALUES_IOCTL i =[%d], offset =[%u],data->values[i]=[%u], port_value=[%u]\n",
                    i, offset, data->values[i], port_value);
            }
            return 0;
        }
        case GPIOHANDLE_SET_LINE_VALUES_IOCTL:
        {
            struct gpio_dev* client = (struct gpio_dev*)zfp->filep;
            if (client == NULL || client->type != LINEHANDLE)
            {
                LOG_ERROR(
                    "GPIOHANDLE_SET_LINE_VALUES_IOCTL [client == NULL] or [client->type != LINEHANDLE]\n");
                return -EINVAL;
            }
            // client->gpio_union.linehandle.open_mode = 11;
            if (client->gpio_union.linehandle.gdev == NULL)
            {
                LOG_ERROR(
                    "GPIOHANDLE_SET_LINE_VALUES_IOCTL gdev is a NULL pointer.\n");
                return -1;
            }

            if (client->gpio_union.linehandle.gdev->dev == NULL)
            {
                LOG_ERROR(
                    "GPIOHANDLE_SET_LINE_VALUES_IOCTL gdev.dev is a NULL pointer.\n");
                return -1;
            }

            struct gpiohandle_data* data = va_arg(args,
                                                  struct gpiohandle_data*);
            __u32 mask = 0;
            __u32 value = 0;
            __u32 lines = client->gpio_union.linehandle.lines;
            if (lines == 0 || lines > CONFIG_GPIO_MAX_PINS_PER_DEVICE)
            {
                LOG_ERROR(
                    "GPIOHANDLE_SET_LINE_VALUES_IOCTL invalid lines=[%u]\n",
                    lines);
                return -EINVAL;
            }

            for (int i = 0; i < lines; i++)
            {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
                LOG_DBG(
                    "GPIOHANDLE_SET_LINE_VALUES_IOCTL i =[%d], offset =[%u]\n",
                    i, offset);
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE ||
                    offset == (unsigned int)-1)
                {
                    LOG_ERROR(
                        "GPIOHANDLE_SET_LINE_VALUES_IOCTL Offset %u exceeds max pins per device (%d)",
                        offset, CONFIG_GPIO_MAX_PINS_PER_DEVICE);
                    return -EINVAL;
                }

                mask |= BIT(offset);
                // 设置值
                if (data->values[i])
                {
                    value |= BIT(offset);
                    LOG_DBG(
                        "GPIOHANDLE_SET_LINE_VALUES_IOCTL Pin = %u & value = %u set to HIGH",
                        offset, value);
                }
                else
                {
                    value &= ~BIT(offset);
                    LOG_DBG(
                        "GPIOHANDLE_SET_LINE_VALUES_IOCTL Pin = %u & value = %u set to LOW",
                        offset, value);
                }
            }

            LOG_DBG(
                "GPIOHANDLE_SET_LINE_VALUES_IOCTL mask = 0x%x, value = 0x%x\n",
                mask, value);
            if (client->gpio_union.linehandle.gdev->dev != NULL)
            {
                ret = gpio_port_set_masked_raw(
                    client->gpio_union.linehandle.gdev->dev, mask, value);
                if (ret < 0)
                {
                    LOG_ERROR(
                        "GPIOHANDLE_SET_LINE_VALUES_IOCTL gpio_port_set_masked_raw fail ret =[%d]\n",
                        ret);
                    return ret;
                }
            }
            else
            {
                LOG_ERROR(
                    "GPIOHANDLE_SET_LINE_VALUES_IOCTL Error:gdev->dev is a NULL pointer.\n");
                return -1;
            }
            LOG_DBG(
                "GPIOHANDLE_SET_LINE_VALUES_IOCTL mask = 0x%08x (%u), value = 0x%08x (%u)",
                mask, mask, value, value);
            return 0;
        }
        case GPIOHANDLE_SET_CONFIG_IOCTL:
        {
            struct gpio_dev* client = (struct gpio_dev*)zfp->filep;
            if (client == NULL || client->type != LINEHANDLE)
            {
                LOG_ERROR(
                    "GPIOHANDLE_SET_CONFIG_IOCTL [client == NULL] or [client->type != LINEHANDLE]\n");
                return -EINVAL;
            }

            struct gpiohandle_config* config =
                va_arg(args, struct gpiohandle_config*);
            __u32 lines = client->gpio_union.linehandle.lines;
            if (lines == 0 || lines > CONFIG_GPIO_MAX_PINS_PER_DEVICE)
            {
                LOG_ERROR("GPIOHANDLE_SET_CONFIG_IOCTL invalid lines=[%u]\n",
                          lines);
                return -EINVAL;
            }

            for (int i = 0; i < lines; i++)
            {
                __u32 offset = client->gpio_union.linehandle.lineoffsets[i];
                if (offset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE ||
                    offset == (unsigned int)-1)
                {
                    LOG_ERROR(
                        "GPIOHANDLE_SET_CONFIG_IOCTL Offset %u exceeds max pins per device (%d)",
                        offset, CONFIG_GPIO_MAX_PINS_PER_DEVICE);
                    return -EINVAL;
                }
                ret = -1;
                ret =
                    gpio_pin_configure(client->gpio_union.linehandle.gdev->dev,
                                       offset, config->flags);
                if (ret < 0)
                {
                    LOG_ERROR("GPIOHANDLE_SET_CONFIG_IOCTL ret=[%d]\n", ret);
                    return ret;
                }
            }
            return 0;
        }
        case GPIO_GET_LINEEVENT_IOCTL:
        {
            struct gpio_dev* client = (struct gpio_dev*)zfp->filep;
            if (client == NULL || client->type != GPIOCHIP)
            {
                return -EINVAL;
            }
            LOG_DBG("GPIO_GET_LINEEVENT_IOCTL: client->type (client->type=%d)",
                    client->type);
            struct gpioevent_request* req = va_arg(args,
                                                   struct gpioevent_request*);
            if (req == NULL)
            {
                return -EINVAL;
            }
            if (req->lineoffset >= CONFIG_GPIO_MAX_PINS_PER_DEVICE)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEEVENT_IOCTL invalid lineoffset=[%u] (max=[%d])\n",
                    req->lineoffset, CONFIG_GPIO_MAX_PINS_PER_DEVICE - 1);
                return -EINVAL;
            }
            if ((req->eventflags & GPIOEVENT_REQUEST_BOTH_EDGES) == 0 &&
                (req->eventflags & GPIOEVENT_REQUEST_RISING_EDGE) == 0 &&
                (req->eventflags & GPIOEVENT_REQUEST_FALLING_EDGE) == 0)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEEVENT_IOCTL invalid eventflags=0x%x! No edge trigger specified.\n",
                    req->eventflags);
                return -EINVAL;
            }

            int idx = malloc_state(client, lineevtfd_dev);
            if (idx < 0)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEEVENT_IOCTL: malloc_state failed (idx=%d)",
                    idx);
                return -ENOMEM;
            }

            char path[64];
            memset(path, 0x00, sizeof(path));
            snprintf(path, sizeof(path), "/dev/gpio/gpio-lineevent%d", idx);

            struct lineevent_state* evt_state =
                &lineevtfd_dev[idx].gpio_union.lineevent;
            evt_state->gdev = &client->gpio_union.chip;
            if (evt_state->gdev == NULL || evt_state->gdev->dev == NULL)
            {
                // lineevtfd_dev[idx].type = 0;
                return -EINVAL;
            }

            LOG_DBG(
                "client->gpio_union.chip.label = %s, path = %s, evt_state = %p.\n",
                client->gpio_union.chip.label, path, evt_state);

            evt_state->lines = 1;
            evt_state->flags = req->eventflags;
            evt_state->lineoffset[0] = req->lineoffset;

            ring_buf_init(&evt_state->event_buf,
                          sizeof(evt_state->event_buf_data),
                          evt_state->event_buf_data);
            k_poll_signal_init(&evt_state->poll_signal);
            atomic_set(&evt_state->is_enabled, 1);
            lineevtfd_dev[idx].type = LINEEVT;

            struct gpio_callback* gpio_cb = &evt_state->gpio_cb;
            __u32 pin = evt_state->lineoffset[0];

            gpio_init_callback(gpio_cb, lineevent_gpio_isr, BIT(pin));

            int ret = 0;
            const struct device* gpio_dev = evt_state->gdev->dev;
            uint32_t intr_config;
            if (evt_state->flags == GPIOEVENT_REQUEST_BOTH_EDGES)
            {
                intr_config = GPIO_INT_EDGE_BOTH;
            }
            else if (evt_state->flags & GPIOEVENT_REQUEST_RISING_EDGE)
            {
                intr_config = GPIO_INT_EDGE_RISING;
            }
            else if (evt_state->flags & GPIOEVENT_REQUEST_FALLING_EDGE)
            {
                intr_config = GPIO_INT_EDGE_FALLING;
            }
            else
            {
                ret = -EINVAL;
                goto err_cleanup;
            }

            if (!device_is_ready(gpio_dev))
            {
                LOG_ERROR("GPIO_GET_LINEEVENT_IOCTL: GPIO device not ready!\n");
                goto err_cleanup;
            }
            ret = gpio_pin_configure(gpio_dev, pin, GPIO_INPUT);
            if (ret < 0)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEEVENT_IOCTL: Pin %u configure INPUT failed (ret=%d)\n",
                    pin, ret);
                goto err_cleanup;
            }
            ret = gpio_pin_interrupt_configure(gpio_dev, pin, intr_config);
            if (ret < 0)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEEVENT_IOCTL: Pin %u interrupt config failed (ret=%d)\n",
                    pin, ret);
                goto err_cleanup;
            }

            ret = gpio_add_callback(gpio_dev, gpio_cb);
            if (ret < 0)
            {
                LOG_ERROR(
                    "GPIO_GET_LINEEVENT_IOCTL: Pin %u add callback failed (ret=%d)\n",
                    pin, ret);
                goto err_cleanup;
            }
            req->fd = zvfs_open(path, O_CREAT, O_RDWR);
            if (req->fd < 0)
            {
                LOG_ERROR("req->fd < 0 %d.\n", req->fd);
                gpio_remove_callback(gpio_dev, gpio_cb);
                goto err_cleanup;
            }
            LOG_DBG(
                "evt_state->lineoffset[0] = %d, idx = %d, gpio_cb = %p, evt_state = %p, gpio_dev = %p, req->fd = %d\n",
                evt_state->lineoffset[0], idx, gpio_cb, evt_state, gpio_dev,
                req->fd);
            return 0;

        err_cleanup:
            if (device_is_ready(gpio_dev))
            {
                gpio_remove_callback(gpio_dev, gpio_cb);
            }
            memset(evt_state, 0, sizeof(struct lineevent_state));
            lineevtfd_dev[idx].type = 0;
            return ret;
        }
        case ZFD_IOCTL_POLL_PREPARE:
        {
            struct zvfs_pollfd* pfd;
            struct k_poll_event** pev;
            struct k_poll_event* pev_end;
            pfd = va_arg(args, struct zvfs_pollfd*);
            pev = va_arg(args, struct k_poll_event**);
            pev_end = va_arg(args, struct k_poll_event*);
            // 入参校验
            if (zfp == NULL || zfp->filep == NULL || pfd == NULL ||
                pev == NULL || pev_end == NULL)
            {
                LOG_ERROR(
                    "ZFD_IOCTL_POLL_PREPARE: invalid params (zfp=%p, filep=%p, pfd=%p, pev=%p, pev_end=%p)",
                    zfp, zfp->filep, pfd, pev, pev_end);
                return -EINVAL;
            }

            // 获取 lineevent 索引
            int idx = gpiodev_get_lineevt_idx(zfp);
            if (idx < 0 || idx >= LINEEVENT_MAX ||
                lineevtfd_dev[idx].type != LINEEVT)
            {
                LOG_ERROR("ZFD_IOCTL_POLL_PREPARE: invalid lineevent (idx=%d)",
                          idx);
                return -EINVAL;
            }

            // 通过索引访问 lineevtfd_dev 数组
            struct lineevent_state* evt_state =
                &lineevtfd_dev[idx].gpio_union.lineevent;

            // 仅处理 ZVFS_POLLIN 事件
            if (pfd->events & ZVFS_POLLIN)
            {
                // 检查事件数组是否已满
                if (*pev == pev_end)
                {
                    LOG_ERROR(
                        "ZFD_IOCTL_POLL_PREPARE: no free poll event slot");
                    return -ENOMEM;
                }
                // 初始化内核 poll 事件
                (*pev)->obj = &evt_state->poll_signal;
                (*pev)->type = K_POLL_TYPE_SIGNAL;
                (*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
                (*pev)->state = K_POLL_STATE_NOT_READY;
                LOG_DBG(
                    "ZFD_IOCTL_POLL_PREPARE: init poll event (idx=%d, fd=%d, signal=%p)",
                    idx, pfd->fd, &evt_state->poll_signal);

                (*pev)++;
            }
            return 0;
        }

        case ZFD_IOCTL_POLL_UPDATE:
        {
            struct zvfs_pollfd* pfd;
            struct k_poll_event** pev;
            pfd = va_arg(args, struct zvfs_pollfd*);
            pev = va_arg(args, struct k_poll_event**);
            if (zfp == NULL || zfp->filep == NULL || pfd == NULL ||
                pev == NULL || *pev == NULL)
            {
                LOG_ERROR(
                    "ZFD_IOCTL_POLL_UPDATE: invalid params (zfp=%p, filep=%p, pfd=%p, pev=%p, *pev=%p)",
                    zfp, zfp->filep, pfd, pev, *pev);
                return -EINVAL;
            }

            // 获取 lineevent 索引
            int idx = gpiodev_get_lineevt_idx(zfp);
            if (idx < 0 || idx >= LINEEVENT_MAX ||
                lineevtfd_dev[idx].type != LINEEVT)
            {
                LOG_ERROR("ZFD_IOCTL_POLL_UPDATE: invalid lineevent (idx=%d)",
                          idx);
                return -EINVAL;
            }

            // 通过索引访问 lineevtfd_dev 数组
            struct lineevent_state* evt_state =
                &lineevtfd_dev[idx].gpio_union.lineevent;

            // 仅处理 ZVFS_POLLIN 事件
            if (pfd->events & ZVFS_POLLIN)
            {
                // 检查缓冲区是否有数据
                size_t buf_used = ring_buf_size_get(&evt_state->event_buf);
                if (buf_used >= sizeof(struct gpioevent_data))
                {
                    pfd->revents |= ZVFS_POLLIN;
                    LOG_DBG(
                        "ZFD_IOCTL_POLL_UPDATE: triggered (idx=%d, used=%zu)",
                        idx, buf_used);

                    k_poll_signal_reset(&evt_state->poll_signal);
                    (*pev)->state = K_POLL_STATE_NOT_READY;
                }
                (*pev)++;
            }

            return 0;
        }
        default:
            LOG_ERROR("Unsupported ioctl request: %lu", request);
            return -ENOTTY;
    }
    return 0;
}

static int gpiodev_mount(struct fs_mount_t* mountp)
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

static int gpiodev_unmount(struct fs_mount_t* mountp)
{
    if (mountp->mnt_point == NULL)
    {
        LOG_ERROR("Invalid mount point\n");
        return -EINVAL;
    }

    mountp->mnt_point = NULL;
    return 0;
}

static int gpiodev_opendir(struct fs_dir_t* dp, const char* path)
{
    if (strcmp(path, "/dev/gpio") != 0)
    {
        LOG_ERROR("Invalid directory path\n");
        return -EINVAL;
    }

    dp->dirp = &gpio_index;
    return 0;
}

static int gpiodev_readdir(struct fs_dir_t* dp, struct fs_dirent* entry)
{
    int* index = dp->dirp;

    if (*index < GPIOCHIP_NUM)
    {
        // 构造文件条目信息
        snprintf(entry->name, sizeof(entry->name), "%s",
                 gpio_devices[*index].gpio_union.chip.label);
        entry->type = FS_DIR_ENTRY_FILE;
        entry->size = 0; // GPIO 设备没有文件大小
        (*index)++;
        return 0;
    }

    // 没有更多条目
    memset(entry, 0, sizeof(struct fs_dirent));
    return 0;
}

static int gpiodev_closedir(struct fs_dir_t* dp)
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

static int gpiodev_stat(struct fs_mount_t* mountp, const char* path,
                        struct fs_dirent* entry)
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
    .read = gpiodev_read,         // 新增 read
    .ioctl = gpiodev_ioctl,       // 修改 ioctl
    .opendir = gpiodev_opendir,   // fs ls访问
    .readdir = gpiodev_readdir,   // fs ls访问
    .closedir = gpiodev_closedir, // fs ls访问
    .stat = gpiodev_stat,         // fs cd访问
};

static int gpio_dev_init()
{
    LOG_DBG("gpiochip_num = %d\n", GPIOCHIP_NUM);

    // 输出gpio设备name和label信息
    for (int i = 0; i < GPIOCHIP_NUM; i++)
    {
        if (gpio_devices[i].gpio_union.chip.dev->name != NULL)
        {
            LOG_DBG("gpio_devices[%d].dev->name = %s\n", i,
                    gpio_devices[i].gpio_union.chip.dev->name);
        }
        if (gpio_devices[i].gpio_union.chip.label != NULL)
        {
            LOG_DBG("gpio_devices[%d].label = %s\n", i,
                    gpio_devices[i].gpio_union.chip.label);
        }
    }

    gpiofs_mnt.mnt_point = "/dev/gpio";
    LOG_DBG("gpiofs_mnt: type: %d, mnt_point: %s, fs_data: %p\n",
            gpiofs_mnt.type, gpiofs_mnt.mnt_point, gpiofs_mnt.fs_data);

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
    if (rc < 0)
    {
        LOG_ERROR("gpiofs register error (%d)\n", rc);
    }

    rc = gpio_dev_init();
    if (rc != 0)
    {
        LOG_ERROR("gpiofs init failed\n");
        return rc;
    }

    LOG_DBG("gpio_init end\n");
    return rc;
}

// 系统初始化
SYS_INIT(gpio_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
