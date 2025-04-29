#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/sys/ioctl.h>
#include <driver/gpio/gpio_dev.h>

#define MAX_LINES 4

void err(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

// 测试：获取芯片信息 (GPIO_GET_CHIPINFO_IOCTL)
void test_get_chip_info(int fd)
{
    struct gpiochip_info info;

    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
        err("[wy88]---Failed to GPIO_GET_CHIPINFO_IOCTL failed-------");
    }

    printk("Chip name: %s\n", info.name);
    printk("Chip label: %s\n", info.label);
    printk("Number of lines: %u\n", info.lines);
}

// 测试：获取引脚信息 (GPIO_GET_LINEINFO_IOCTL)
void test_get_line_info(int fd, unsigned int offset)
{
    struct gpioline_info info;
    memset(&info, 0, sizeof(info));
    info.line_offset = offset;

    if (ioctl(fd, GPIO_GET_LINEINFO_IOCTL, &info) < 0) {
        err("[wy88]---Failed to GPIO_GET_LINEINFO_IOCTL failed-------");
    }

    printk("\nLine Offset: %u\n", info.line_offset);
    printk("Flags: 0x%x\n", info.flags);
    printk("Name: %s\n", info.name);
    printk("Consumer: %s\n", info.consumer);
}

// 测试：设置引脚电平 (GPIOHANDLE_SET_LINE_VALUES_IOCTL)
void test_set_values(int handle_fd, unsigned int *values, int num_values)
{
    struct gpiohandle_data data;

    if (num_values > GPIOHANDLES_MAX)
        num_values = GPIOHANDLES_MAX;

    for (int i = 0; i < num_values; i++) {
        data.values[i] = values[i];
    }

    if (ioctl(handle_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
        err("[wy88]---Failed to GPIO_GET_LINEINFO_IOCTL failed-------");
    }

    printk("Set GPIO values: ");
    for (int i = 0; i < num_values; i++) {
        printk("%u ", data.values[i]);
    }

    printk("\n");
}

// 测试：读取引脚电平 (GPIOHANDLE_GET_LINE_VALUES_IOCTL)
void test_get_values(int handle_fd, int num_values)
{
    struct gpiohandle_data data;

    if (ioctl(handle_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0) {
        err("Failed to get line values");
    }

    printk("Current GPIO values: ");
    for (int i = 0; i < num_values; i++) {
        printk("%u ", data.values[i]);
    }
    printk("\n");
}

// 测试：设置引脚配置 (GPIOHANDLE_SET_CONFIG_IOCTL)
void test_set_config(int handle_fd, unsigned int *offsets, int num_lines, unsigned int flags, unsigned int *default_values)
{
    struct gpiohandle_config config;
    memset(&config, 0, sizeof(config));

    config.flags = flags;


    // 设置默认电平值
    for (int i = 0; i < GPIOHANDLES_MAX; ++i) {
        if (i < MAX_LINES && default_values != NULL) {
            config.default_values[i] = default_values[i];
        } else {
            config.default_values[i] = 0; // 默认为低电平
        }
    }

    // 执行 IOCTL 设置配置
    if (ioctl(handle_fd, GPIOHANDLE_SET_CONFIG_IOCTL, &config) == -1) {
        err("Failed to set GPIO config");
    }
    printk("Successfully set configuration for %d lines\n", num_lines);
}

int gpio_main_test()
{
    const char *file_name = "/dev/gpio/gpiochip0";

    int fd = open(file_name, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        err("Failed to open GPIO chip");
    }

    // 测试获取芯片信息
    test_get_chip_info(fd);

    // 测试获取某个引脚信息（假设是 offset=0）
    test_get_line_info(fd, 0);
    
   // 请求 GPIO 线句柄
   struct gpiohandle_request req;
   memset(&req, 0, sizeof(req));
   req.lineoffsets[0] = 0; // 请求第 0 号引脚
   req.lineoffsets[1] = 1; // 请求第 1 号引脚
   req.flags = GPIOHANDLE_REQUEST_OUTPUT; // 设置为输出模式
   req.default_values[0] = 0; // 默认值为低电平
   req.default_values[1] = 0; // 默认值为低电平
   req.lines = 2; // 请求的引脚数量

    if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        err("[wy88]---Failed to request GPIO---------");
    }

    // 设置 GPIO 配置
    unsigned int new_flags = GPIOHANDLE_REQUEST_INPUT; // 更改为输入模式
    unsigned int new_default_values[] = {0, 0}; // 新的默认值
    unsigned int offsets[] = {0, 1};
    
    int handle_fd = req.fd;

    unsigned int values_on[] = {1, 1};
    unsigned int values_off[] = {0, 0};
    // 设置高电平
    test_set_values(handle_fd, values_on, 2);
    
    sleep(1); // 延时一下看效果

    // 设置低电平
    test_set_values(handle_fd, values_off, 2);

    // 读取当前值
    test_get_values(handle_fd, 2);
    
    // 设置新的配置（例如输入模式）
    test_set_config(handle_fd, offsets, 2, GPIOHANDLE_REQUEST_INPUT, NULL);

    close(handle_fd);
    close(fd);

    return 0;
}