#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/sys/ioctl.h>
#include <driver/gpio/gpio_dev.h>

#define MAX_LINES 4

// 测试：设置引脚电平 (GPIOHANDLE_SET_LINE_VALUES_IOCTL)
void test_set_values(int handle_fd, uint8_t *values, int num_values)
{
    struct gpiohandle_data data;

    if (num_values > GPIOHANDLES_MAX)
        num_values = GPIOHANDLES_MAX;

    for (int i = 0; i < num_values; i++) {
        printk("[TEST] before---%i, %u \n", i, values[i]);
        data.values[i] = values[i];
    }

    if (ioctl(handle_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
        printk("[TEST]---Failed to GPIO_SET_LINEINFO_IOCTL failed-------\n");
    }

    printk("[TEST]Set GPIO values: \n");
    for (int i = 0; i < num_values; i++) {
        printk("[TEST] after---%i, %u \n", i, data.values[i]);
    }

    printk("\n");
}

// 测试：读取引脚电平 (GPIOHANDLE_GET_LINE_VALUES_IOCTL)
void test_get_values(int handle_fd, int num_values)
{
    printk("[TEST] GPIOHANDLE_GET_LINE_VALUES_IOCTL start\n");
    struct gpiohandle_data data;

    if (ioctl(handle_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0) {
        printk("[TEST] Failed to get line values");
    }

    printk("[TEST]Current GPIO values: ");
    for (int i = 0; i < num_values; i++) {
        printk("%u ", data.values[i]);
    }
    printk("\n");
    printk("[TEST] GPIOHANDLE_GET_LINE_VALUES_IOCTL end\n");
}

// 测试：设置引脚配置 (GPIOHANDLE_SET_CONFIG_IOCTL)
void test_set_config(int handle_fd, unsigned int *offsets, int num_lines, unsigned int flags, unsigned int *default_values)
{
    printk("[TEST] GPIOHANDLE_SET_CONFIG_IOCTL start\n");
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
        printk("[TEST] Failed to set GPIO config\n");
    }
    printk("[TEST] Successfully set configuration for %d lines\n", num_lines);
    printk("[TEST] GPIOHANDLE_SET_CONFIG_IOCTL end\n");
}

int gpio_main_test()
{
    printk("[TEST]----------start\n");
    
    const char *file_name = "/dev/gpio/gpiochip4";
    int fd = open(file_name, O_RDWR | O_CLOEXEC);
    printk("[TEST] file_name = [/dev/gpio/gpiochip4]-----fd=[%d]\n", fd);
    if (fd < 0) {
        printk("[TEST] ----Failed to open GPIO chip\n");
        return -1;
    }

    // 测试获取芯片信息
    printk("[TEST]----GPIO_GET_CHIPINFO_IOCTL start\n");
    struct gpiochip_info chipinfo;
    memset(&chipinfo, 0, sizeof(chipinfo));

    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chipinfo) < 0) {
        printk("[TEST] Failed to GPIO_GET_CHIPINFO_IOCTL failed!\n");
        close(fd);
        return -1;
    }
    printk("[TEST] Chip name: %s\n", chipinfo.name);
    printk("[TEST] Chip label: %s\n", chipinfo.label);
    printk("[TEST] Number of lines: %u\n", chipinfo.lines);
    printk("[TEST] ----GPIO_GET_CHIPINFO_IOCTL end\n");

    k_sleep(K_SECONDS(1));
    
    // 测试获取某个引脚信息（假设是 offset=0）
    printk("[TEST] ----GPIO_GET_LINEINFO_IOCTL start\n");
    struct gpioline_info lineinfo;
    memset(&lineinfo, 0, sizeof(lineinfo));
    lineinfo.line_offset = 2;

    if (ioctl(fd, GPIO_GET_LINEINFO_IOCTL, &lineinfo) < 0) {
        printk("[TEST] Failed to GPIO_GET_LINEINFO_IOCTL failed!\n");
        close(fd);
        return -1;
    }

    printk("\n[TEST] Line Offset: %u\n", lineinfo.line_offset);
    printk("[TEST] Flags: 0x%x\n", lineinfo.flags);
    printk("[TEST] Name: %s\n", lineinfo.name);
    printk("[TEST] Consumer: %s\n", lineinfo.consumer);
    printk("[TEST] ----GPIO_GET_LINEINFO_IOCTL end\n");

    k_sleep(K_SECONDS(1));
    
    printk("[TEST] ----GPIO_GET_LINEHANDLE_IOCTL start\n");
   // 请求 GPIO 线句柄
   struct gpiohandle_request req;
   memset(&req, 0, sizeof(req));
   req.lineoffsets[0] = 3;
   req.lineoffsets[1] = 5;
   req.flags = GPIOHANDLE_REQUEST_OUTPUT; // 设置为输出模式
   req.default_values[0] = 0; // 默认值为低电平
   req.default_values[1] = 0; // 默认值为低电平
   req.lines = 2; // 请求的引脚数量

    if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        printk("[TEST] Failed to GPIO_GET_LINEHANDLE_IOCTL failed!\n");
        close(fd);
        return -1;
    }
    printk("[TEST]gpio_main_test----GPIO_GET_LINEHANDLE_IOCTL end\n");
    k_sleep(K_SECONDS(1));

    printk("[TEST] ----test_set_config start\n");

    // 设置 GPIO 配置
    uint8_t values_on[] = {1, 1};
    uint8_t values_off[] = {0, 0};
    unsigned int offsets[] = {0, 0};
    int handle_fd = req.fd;


    // 设置新的配置（例如输出模式）
    test_set_config(handle_fd, offsets, 2, GPIO_OUTPUT, NULL);
    k_sleep(K_SECONDS(1));

    // 设置高电平
    test_set_values(handle_fd, values_on, 2);
    k_sleep(K_SECONDS(1));

    // 读取当前值
    test_get_values(handle_fd, 2);
    k_sleep(K_SECONDS(1));

    // // 设置新的配置（例如输出模式）
    // test_set_config(handle_fd, offsets, 2, GPIO_OUTPUT, NULL);

    // // 设置低电平
    // test_set_values(handle_fd, values_off, 2);


    // // 读取当前值
    // test_get_values(handle_fd, 2);

    close(handle_fd);
    close(fd);

    return 0;
}