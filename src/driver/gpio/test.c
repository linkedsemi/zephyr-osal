/**
 * @file gpio_line_event_test.c
 * @brief GPIO线事件（Line Event）功能测试程序
 * @details 测试单引脚/多引脚GPIO事件监听、异常场景验证，基于Zephyr
 * POSIX接口实现
 * @note 适配gpiochip6(gpiog)、gpiochip7(gpioh)，测试下降沿/上升沿事件触发
 */

#include <driver/gpio/gpio_dev.h> // GPIO设备驱动核心头文件
#include <errno.h>                // 错误码定义（补充：原代码使用errno但未包含）
#include <string.h>               // 字符串操作函数
#include <zephyr/kernel.h>        // Zephyr内核核心头文件
#include <zephyr/posix/fcntl.h>   // POSIX文件控制接口
#include <zephyr/posix/sys/ioctl.h> // IO控制接口
#include <zephyr/posix/unistd.h>    // POSIX标准函数（read/close等）
#include <zephyr/sys/printk.h>      // 内核打印函数

/************************** 全局配置参数 **************************/
#define MAX_LINES 4               // 最大支持的GPIO线数量（预留）
#define EVENT_WAIT_TIMEOUT 20000  // 单引脚Poll超时时间(ms)：20秒（延长容错）
#define BUFFER_TRIGGER_TIMEOUT 30 // 缓冲区测试触发超时时间(s)：30秒
#define MULTI_PIN_TIMEOUT 25000   // 多引脚Poll超时时间(ms)：25秒
#define TEST_EVENT_PIN1 1     // 测试引脚1：gpioh-1（对应gpiochip7的1号引脚）
#define TEST_EVENT_PIN2 4     // 测试引脚2：gpioh-4（对应gpiochip7的4号引脚）
#define GPIO_MAX_NAME_SIZE 32 // GPIO消费者标签最大长度（防溢出）
#define BUFFER_TEST_EVENT_CNT                                                  \
    15                    // 缓冲区测试：触发事件次数（超过内核默认10条上限）
#define READ_RETRY_MAX 50 // 读取事件最大重试次数（原30次，延长容错）
#define COUNT_DOWN_INTERVAL 5 // 倒计时提示间隔(s)：提升用户操作体验
#define INVALID_PIN 20        // 无效引脚（超出gpiochip7的16个引脚范围）

/************************** 全局变量 **************************/
/**
 * @brief 多引脚事件读取计数（统计成功触发的事件数）
 * @note 全局变量仅用于测试统计，实际产品中建议使用局部变量+回调
 */
static int g_event_read_cnt = 0;

/**
 * @brief 异常测试：非法参数/无效引脚
 * @param chip_fd GPIO芯片设备fd
 * @param invalid_pin 无效引脚（超出芯片引脚范围）
 * @return 0=符合预期，-1=不符合预期
 */
static int test_event_abnormal(int chip_fd, uint32_t invalid_pin)
{
    printk("\n===================================================\n");
    printk("[TEST] 异常测试（无效引脚：%d）开始\n", invalid_pin);
    printk("测试逻辑：验证无效引脚无法创建事件句柄（符合预期）\n");
    printk("===================================================\n");

    struct gpioevent_request evt_req;
    memset(&evt_req, 0, sizeof(evt_req));
    evt_req.lineoffset = invalid_pin;
    evt_req.handleflags = GPIOHANDLE_REQUEST_INPUT;
    evt_req.eventflags = GPIOEVENT_REQUEST_BOTH_EDGES;
    strncpy(evt_req.consumer_label, "test_abnormal", GPIO_MAX_NAME_SIZE - 1);

    printk("[TEST] 尝试创建无效引脚%d的事件句柄...\n", invalid_pin);
    // 预期：创建事件句柄失败
    if (ioctl(chip_fd, GPIO_GET_LINEEVENT_IOCTL, &evt_req) >= 0)
    {
        printk("[TEST] 失败：无效引脚创建成功（不符合预期）\n");
        close(evt_req.fd);
        return -1;
    }

    printk("[TEST] 成功：无效引脚创建失败（符合预期，errno=%d）\n", errno);
    printk("\n[TEST] 异常测试结束（成功）\n");
    printk("===================================================\n");
    return 0;
}

// ---------------------- 以下是你的GPIO配置/电平读写测试用例
// ----------------------
/**
 * @brief 测试：设置引脚电平 (GPIOHANDLE_SET_LINE_VALUES_IOCTL)
 */
static void test_set_values(int handle_fd, uint8_t* values, int num_values)
{
    printk("[TEST] GPIOHANDLE_SET_LINE_VALUES_IOCTL start\n");
    struct gpiohandle_data data;

    if (num_values > GPIOHANDLES_MAX)
        num_values = GPIOHANDLES_MAX;

    for (int i = 0; i < num_values; i++)
    {
        printk("[TEST] before---%i, %u \n", i, values[i]);
        data.values[i] = values[i];
    }

    if (ioctl(handle_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0)
    {
        printk(
            "[TEST] Failed to GPIOHANDLE_SET_LINE_VALUES_IOCTL failed-------\n");
    }

    printk("[TEST]Set GPIO values: \n");
    for (int i = 0; i < num_values; i++)
    {
        printk("[TEST] after---%i, %u \n", i, data.values[i]);
    }

    printk("\n");
    printk("[TEST] Successfully GPIOHANDLE_SET_LINE_VALUES_IOCTL\n");
    printk("[TEST] GPIOHANDLE_SET_LINE_VALUES_IOCTL end!\n");
    printk("********************************************");
    printk("\n");
}

/**
 * @brief 测试：读取引脚电平 (GPIOHANDLE_GET_LINE_VALUES_IOCTL)
 */
static void test_get_values(int handle_fd, int num_values)
{
    printk("[TEST] GPIOHANDLE_GET_LINE_VALUES_IOCTL start\n");
    struct gpiohandle_data data;

    if (ioctl(handle_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0)
    {
        printk("[TEST] Failed to get line values（errno=%d）\n", errno);
    }

    printk("[TEST]Current GPIO values: ");
    for (int i = 0; i < num_values; i++)
    {
        printk("%u ", data.values[i]);
    }

    printk("\n");
    printk("[TEST] Successfully GPIOHANDLE_GET_LINE_VALUES_IOCTL\n");
    printk("[TEST] GPIOHANDLE_GET_LINE_VALUES_IOCTL end!\n");
    printk("********************************************");
    printk("\n");
}

/**
 * @brief 测试：设置引脚配置 (GPIOHANDLE_SET_CONFIG_IOCTL)
 */
static void test_set_config(int handle_fd, unsigned int* offsets, int num_lines,
                            unsigned int flags, unsigned int* default_values)
{
    printk("[TEST] GPIOHANDLE_SET_CONFIG_IOCTL start\n");
    struct gpiohandle_config config;
    memset(&config, 0, sizeof(config));

    config.flags = flags;

    // 设置默认电平值
    for (int i = 0; i < GPIOHANDLES_MAX; ++i)
    {
        if (i < MAX_LINES && default_values != NULL)
        {
            config.default_values[i] = default_values[i];
        }
        else
        {
            config.default_values[i] = 0; // 默认为低电平
        }
    }

    // 执行 IOCTL 设置配置
    if (ioctl(handle_fd, GPIOHANDLE_SET_CONFIG_IOCTL, &config) == -1)
    {
        printk("[TEST] Failed to set GPIO config（errno=%d）\n", errno);
    }
    printk("[TEST] Successfully set configuration for %d lines\n", num_lines);
    printk("[TEST] GPIOHANDLE_SET_CONFIG_IOCTL end!\n");
    printk("********************************************");
    printk("\n");
}
/**
 * @brief 倒计时提示函数（提升用户交互体验，避免错过触发时机）
 * @param total_sec 总倒计时时间（单位：秒）
 * @param tip 倒计时提示文案（说明当前需要执行的操作）
 * @note 每隔COUNT_DOWN_INTERVAL秒打印一次剩余时间，最后5秒结束后提示处理事件
 */
static void count_down_tip(uint32_t total_sec, const char* tip)
{
    printk("\n[提醒] %s（剩余时间：%d秒）\n", tip, total_sec);

    // 分段倒计时提示（除最后5秒外）
    for (int i = total_sec - COUNT_DOWN_INTERVAL; i > 0;
         i -= COUNT_DOWN_INTERVAL)
    {
        k_sleep(K_SECONDS(COUNT_DOWN_INTERVAL)); // 等待指定间隔
        printk("[提醒] 触发时间还剩 %d 秒！请尽快操作！\n", i);
    }

    k_sleep(K_SECONDS(COUNT_DOWN_INTERVAL)); // 等待最后5秒
    printk("[提醒] 触发时间结束！开始处理事件...\n");
}

/**
 * @brief 单引脚事件测试（注册+监听+手动触发+读取）
 * @param chip_fd GPIO芯片设备fd
 * @param event_pin 事件输入引脚（14/15，属于gpiochip3=gpiod）
 * @param edge 触发边缘（GPIOEVENT_REQUEST_XXX）
 * @return 0=成功，-1=失败
 */
static int test_single_pin_event(int chip_fd, uint32_t event_pin, uint32_t edge)
{
    printk("\n===================================================\n");
    printk("[TEST] 单引脚事件测试（引脚%d，触发边缘：%s）开始\n", event_pin,
           edge == GPIOEVENT_REQUEST_FALLING_EDGE  ? "下降沿"
           : edge == GPIOEVENT_REQUEST_RISING_EDGE ? "上升沿"
                                                   : "双边沿");
    printk("===================================================\n");

    struct gpioevent_request evt_req;
    memset(&evt_req, 0, sizeof(evt_req));

    evt_req.lineoffset = event_pin;
    evt_req.handleflags = GPIOHANDLE_REQUEST_INPUT;
    evt_req.eventflags = edge;
    strncpy(evt_req.consumer_label, "test_single_pin", GPIO_MAX_NAME_SIZE - 1);
    evt_req.consumer_label[GPIO_MAX_NAME_SIZE - 1] = '\0';

    // 创建事件句柄（核心功能正常，日志已验证）
    if (ioctl(chip_fd, GPIO_GET_LINEEVENT_IOCTL, &evt_req) < 0)
    {
        printk("[TEST] 创建事件句柄失败（errno=%d）\n", errno);
        return -1;
    }
    int event_fd = evt_req.fd;
    printk("[TEST] 事件句柄创建成功（fd=%d），开始监听（超时：%dms）...\n",
           event_fd, EVENT_WAIT_TIMEOUT);

    struct pollfd fds;
    fds.fd = event_fd;
    fds.events = POLLIN;
    fds.revents = 0;

    while (1)
    {
        int poll_ret = poll(&fds, 1, EVENT_WAIT_TIMEOUT);
        if (poll_ret < 0)
        {
            printk("[TEST] Poll出错（ret=%d, errno=%d）\n", poll_ret, errno);
            close(event_fd);
            return -1;
        }
        else if (poll_ret == 0)
        {
            printk("[TEST] Poll超时（%dms）\n", EVENT_WAIT_TIMEOUT);
            break;
        }

        if (fds.revents & POLLIN)
        {
            struct gpioevent_data evt_data;
            ssize_t read_len = read(event_fd, &evt_data, sizeof(evt_data));
            printk("read_len = %zd, 时间戳：%lld, 事件类型：%d\n", read_len,
                   evt_data.timestamp, evt_data.id);
            if (read_len == sizeof(evt_data))
            {
                printk("[TEST] 成功读取事件\n");
                g_event_read_cnt++;
            }
            else
            {
                printk("[TEST] 事件读取失败\n");
            }
        }
    }

    k_msleep(1000);
    close(event_fd);
    printk("[TEST] 单引脚事件测试结束，共读取 %d 个事件\n", g_event_read_cnt);
    printk("===================================================\n");
    return 0;
}
/**
 * @brief 多引脚GPIO事件测试（核心功能）
 * @details 同时监听3个不同GPIO芯片的引脚事件：
 *          - gpiochip7(gpioh)：1号引脚（双边沿触发）、4号引脚（上升沿触发）
 *          - gpiochip6(gpiog)：4号引脚（双边沿触发）
 * @param chip_fd_0 gpiochip7的文件描述符（对应gpioh）
 * @param chip_fd_1 gpiochip6的文件描述符（对应gpiog）
 * @return 0:测试成功，-1:测试失败（事件句柄创建/POLL/读取失败）
 * @note 无限循环监听事件，需外部终止（如按键/定时器）
 */
int test_multi_pin_event(int chip_fd_0, int chip_fd_1)
{
    // 兼容驱动未定义双边沿宏的情况
#ifndef GPIOEVENT_REQUEST_BOTH_EDGES
#define GPIOEVENT_REQUEST_BOTH_EDGES                                           \
    (GPIOEVENT_REQUEST_RISING_EDGE | GPIOEVENT_REQUEST_FALLING_EDGE)
#endif

    // 定义3个引脚的事件请求结构体（初始化清零）
    struct gpioevent_request evt_req1, evt_req2, evt_req3;
    memset(&evt_req1, 0, sizeof(evt_req1));
    memset(&evt_req2, 0, sizeof(evt_req2));
    memset(&evt_req3, 0, sizeof(evt_req3));

    /********** 配置引脚1：gpioh-1（gpiochip7，双边沿触发） **********/
    evt_req1.lineoffset = 1; // 引脚偏移量（gpiochip7的1号引脚）
    evt_req1.handleflags = GPIOHANDLE_REQUEST_INPUT; // 配置为输入模式
    evt_req1.eventflags =
        GPIOEVENT_REQUEST_BOTH_EDGES;                // 双边沿触发（上升+下降）
    strncpy(evt_req1.consumer_label, "test_multi1",
            GPIO_MAX_NAME_SIZE - 1);                 // 消费者标签（防溢出）

    /********** 配置引脚2：gpioh-4（gpiochip7，上升沿触发） **********/
    evt_req2.lineoffset = 4; // 引脚偏移量（gpiochip7的4号引脚）
    evt_req2.handleflags = GPIOHANDLE_REQUEST_INPUT; // 配置为输入模式
    evt_req2.eventflags =
        GPIOEVENT_REQUEST_RISING_EDGE; // 上升沿触发（保持原有逻辑）
    strncpy(evt_req2.consumer_label, "test_multi2",
            GPIO_MAX_NAME_SIZE - 1);   // 消费者标签

    /********** 配置引脚3：gpiog-4（gpiochip6，双边沿触发） **********/
    evt_req3.lineoffset = 4; // 引脚偏移量（gpiochip6的4号引脚）
    evt_req3.handleflags = GPIOHANDLE_REQUEST_INPUT; // 配置为输入模式
    evt_req3.eventflags =
        GPIOEVENT_REQUEST_BOTH_EDGES;                // 双边沿触发（上升+下降）
    strncpy(evt_req3.consumer_label, "test_multi3",
            GPIO_MAX_NAME_SIZE - 1);                 // 消费者标签

    /********** 创建事件句柄（核心IOCTL调用） **********/
    if (ioctl(chip_fd_0, GPIO_GET_LINEEVENT_IOCTL, &evt_req1) < 0 ||
        ioctl(chip_fd_0, GPIO_GET_LINEEVENT_IOCTL, &evt_req2) < 0 ||
        ioctl(chip_fd_1, GPIO_GET_LINEEVENT_IOCTL, &evt_req3) < 0)
    {
        printk("[ERROR] 创建多引脚事件句柄失败（errno=%d）\n", errno);
        // 关闭已成功创建的句柄，避免资源泄漏
        if (evt_req1.fd >= 0)
            close(evt_req1.fd);
        if (evt_req2.fd >= 0)
            close(evt_req2.fd);
        if (evt_req3.fd >= 0)
            close(evt_req3.fd);
        return -1;
    }

    /********** 配置POLL监听结构体 **********/
    struct pollfd fds[3] = {
        {.fd = evt_req1.fd,
         .events = POLLIN,
         .revents = 0}, // 监听引脚1的可读事件
        {.fd = evt_req2.fd,
         .events = POLLIN,
         .revents = 0}, // 监听引脚2的可读事件
        {.fd = evt_req3.fd, .events = POLLIN, .revents = 0}
        // 监听引脚3的可读事件
    };

    printk("[TEST] 多引脚事件监听启动：\n");
    printk("  - gpioh-1：双边沿触发（上升沿+下降沿）\n");
    printk("  - gpioh-4：上升沿触发\n");
    printk("  - gpiog-4：双边沿触发（上升沿+下降沿）\n");

    /********** 无限循环监听事件 **********/
    while (1)
    {
        // 阻塞式POLL监听（超时时间-1=永久阻塞）
        int poll_ret = poll(fds, 3, -1);

        // POLL调用失败：关闭句柄并返回错误
        if (poll_ret < 0)
        {
            printk("[ERROR] Poll调用失败（ret=%d, errno=%d）\n", poll_ret,
                   errno);
            close(evt_req1.fd);
            close(evt_req2.fd);
            close(evt_req3.fd);
            return -1;
        }
        // POLL超时（理论上不会触发，因为超时时间设为-1）
        else if (poll_ret == 0)
        {
            printk("[WARN] Poll超时（ret=%d）\n", poll_ret);
            close(evt_req1.fd);
            close(evt_req2.fd);
            close(evt_req3.fd);
            return -1;
        }

        /********** 处理引脚1事件（gpioh-1，双边沿） **********/
        if (fds[0].revents & POLLIN)
        {
            struct gpioevent_data evt1;
            ssize_t read_len = read(evt_req1.fd, &evt1, sizeof(evt1));

            if (read_len == sizeof(evt1))
            {
                g_event_read_cnt++; // 统计成功次数
                // 区分双边沿类型，打印对应日志
                if (evt1.id == GPIOEVENT_EVENT_FALLING_EDGE)
                {
                    printk("[TEST] 事件触发成功：gpioh-1（下降沿）\n");
                }
                else if (evt1.id == GPIOEVENT_EVENT_RISING_EDGE)
                {
                    printk("[TEST] 事件触发成功：gpioh-1（上升沿）\n");
                }
                else
                {
                    printk("[TEST] 事件触发成功：gpioh-1（未知边沿，id=%d）\n",
                           evt1.id);
                }
            }
            else
            {
                printk(
                    "[ERROR] 事件读取失败：gpioh-1（read_len=%zd, event_id=%d）\n",
                    read_len, evt1.id);
            }
        }

        /********** 处理引脚2事件（gpioh-4，上升沿） **********/
        if (fds[1].revents & POLLIN)
        {
            struct gpioevent_data evt2;
            ssize_t read_len = read(evt_req2.fd, &evt2, sizeof(evt2));

            // 验证读取长度和事件类型（上升沿）
            if (read_len == sizeof(evt2) &&
                evt2.id == GPIOEVENT_EVENT_RISING_EDGE)
            {
                printk("[TEST] 事件触发成功：gpioh-4（上升沿）\n");
                g_event_read_cnt++; // 统计成功次数
            }
            else
            {
                printk(
                    "[ERROR] 事件读取失败：gpioh-4（read_len=%zd, event_id=%d）\n",
                    read_len, evt2.id);
            }
        }

        /********** 处理引脚3事件（gpiog-4，双边沿） **********/
        if (fds[2].revents & POLLIN)
        {
            struct gpioevent_data evt3;
            ssize_t read_len = read(evt_req3.fd, &evt3, sizeof(evt3));

            if (read_len == sizeof(evt3))
            {
                g_event_read_cnt++; // 统计成功次数
                // 区分双边沿类型，打印对应日志
                if (evt3.id == GPIOEVENT_EVENT_FALLING_EDGE)
                {
                    printk("[TEST] 事件触发成功：gpiog-4（下降沿）\n");
                }
                else if (evt3.id == GPIOEVENT_EVENT_RISING_EDGE)
                {
                    printk("[TEST] 事件触发成功：gpiog-4（上升沿）\n");
                }
                else
                {
                    printk("[TEST] 事件触发成功：gpiog-4（未知边沿，id=%d）\n",
                           evt3.id);
                }
            }
            else
            {
                printk(
                    "[ERROR] 事件读取失败：gpiog-4（read_len=%zd, event_id=%d）\n",
                    read_len, evt3.id);
            }
        }
    }

    // 理论上不会执行到此处（无限循环），保险关闭句柄
    close(evt_req1.fd);
    close(evt_req2.fd);
    close(evt_req3.fd);
    return 0;
}

/**
 * @brief GPIO事件测试主函数（入口）
 * @details 初始化GPIO芯片设备、执行多引脚事件测试（核心）
 * @return 0:测试启动成功，-1:设备打开失败
 */
int gpio_main_test()
{
    printk("\n===================================================\n");
    printk("                GPIO 完整测试套件启动                \n");
    printk("===================================================\n");

    int ret = 0;
    g_event_read_cnt = 0; // 重置事件计数

    // ==============================================
    // 第一部分：GPIO配置/电平读写测试（gpiochip4）
    // ==============================================
    printk("\n[TEST] ========== 开始：GPIO配置/电平读写测试 ==========\n");
    const char* chip4_path = "/dev/gpio/gpiochip4";
    int fd4 = open(chip4_path, O_RDWR | O_CLOEXEC);
    printk("[TEST] 打开GPIO芯片：%s，fd=%d\n", chip4_path, fd4);
    if (fd4 < 0)
    {
        printk("[TEST] 打开gpiochip4失败（errno=%d）\n", errno);
        ret = -1;
    }
    else
    {
        // 1. 测试获取芯片信息
        printk("[TEST] GPIO_GET_CHIPINFO_IOCTL start\n");
        struct gpiochip_info chipinfo;
        memset(&chipinfo, 0, sizeof(chipinfo));
        if (ioctl(fd4, GPIO_GET_CHIPINFO_IOCTL, &chipinfo) < 0)
        {
            printk(
                "[TEST] Failed to GPIO_GET_CHIPINFO_IOCTL failed!（errno=%d）\n",
                errno);
            ret = -1;
        }
        else
        {
            printk("[TEST] Chip name: %s\n", chipinfo.name);
            printk("[TEST] Chip label: %s\n", chipinfo.label);
            printk("[TEST] Number of lines: %u\n", chipinfo.lines);
        }

        printk("[TEST] Successfully GPIO_GET_CHIPINFO_IOCTL\n");
        printk("[TEST] GPIO_GET_CHIPINFO_IOCTL end\n");
        printk("********************************************");
        printk("\n");
        k_sleep(K_SECONDS(1));

        // 2. 测试获取某个引脚信息（offset=2）
        printk("[TEST] GPIO_GET_LINEINFO_IOCTL start\n");
        struct gpioline_info lineinfo;
        memset(&lineinfo, 0, sizeof(lineinfo));
        // lineinfo.line_offset = 20;
        lineinfo.line_offset = 2;
        if (ioctl(fd4, GPIO_GET_LINEINFO_IOCTL, &lineinfo) < 0)
        {
            printk(
                "[TEST] Failed to GPIO_GET_LINEINFO_IOCTL failed!（errno=%d）\n",
                errno);
            ret = -1;
        }
        else
        {
            printk("\n[TEST] Line Offset: %u\n", lineinfo.line_offset);
            printk("[TEST] Flags: 0x%x\n", lineinfo.flags);
            printk("[TEST] Name: %s\n", lineinfo.name);
            printk("[TEST] Consumer: %s\n", lineinfo.consumer);
        }
        printk("[TEST] Successfully GPIO_GET_LINEINFO_IOCTL\n");
        printk("[TEST] GPIO_GET_LINEINFO_IOCTL end\n");
        printk("********************************************");
        printk("\n");
        k_sleep(K_SECONDS(1));

        // 3. 测试获取引脚句柄
        printk("[TEST] GPIO_GET_LINEHANDLE_IOCTL start\n");
        struct gpiohandle_request req;
        memset(&req, 0, sizeof(req));
        req.lineoffsets[0] = 3;
        // req.lineoffsets[1] = 25;
        req.lineoffsets[1] = 5;
        req.flags = GPIOHANDLE_REQUEST_OUTPUT; // 设置为输出模式
        req.default_values[0] = 0;             // 默认值为低电平
        req.default_values[1] = 0;             // 默认值为低电平
        req.lines = 2;                         // 请求的引脚数量
        if (ioctl(fd4, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0)
        {
            printk(
                "[TEST] Failed to GPIO_GET_LINEHANDLE_IOCTL failed!（errno=%d）\n",
                errno);
            ret = -1;
        }
        else
        {
            printk("[TEST] Successfully GPIO_GET_LINEHANDLE_IOCTL end\n");
            printk("********************************************");
            printk("\n");
            k_sleep(K_SECONDS(1));

            // 4. 测试配置+电平读写
            uint8_t values_on[] = {1, 1};
            uint8_t values_off[] = {0, 0};
            unsigned int offsets[] = {0, 0};
            int handle_fd = req.fd;

            // 设置输出模式
            test_set_config(handle_fd, offsets, 2, GPIO_OUTPUT, NULL);
            k_sleep(K_SECONDS(1));

            // 设置高电平
            test_set_values(handle_fd, values_on, 2);
            k_sleep(K_SECONDS(1));

            // 读取当前值
            test_get_values(handle_fd, 2);
            k_sleep(K_SECONDS(1));

            // 再次设置输出模式
            test_set_config(handle_fd, offsets, 2, GPIO_OUTPUT, NULL);

            // 设置低电平
            test_set_values(handle_fd, values_off, 2);
            k_sleep(K_SECONDS(1));

            // 读取当前值
            test_get_values(handle_fd, 2);

            close(handle_fd);
        }

        close(fd4);
    }
    printk("[TEST] ========== 结束：GPIO配置/电平读写测试 ==========\n");

    // ==============================================
    // 第二部分：GPIO事件监听测试（gpiochip6+gpiochip7）
    // ==============================================
    printk("\n===================================================\n");
    printk("[TEST] GPIO Line Event 完整测试启动\n");
    printk("测试范围：gpiochip7(gpioh)、gpiochip6(gpiog)多引脚事件监听\n");
    printk("===================================================\n");

    /********** 打开GPIO芯片设备 **********/
    const char* gpiochip7_path = "/dev/gpio/gpiochip7"; // gpioh对应的设备节点
    const char* gpiochip6_path = "/dev/gpio/gpiochip6"; // gpiog对应的设备节点

    // 以读写模式打开设备（O_CLOEXEC：进程替换时关闭FD）
    int fd_0 = open(gpiochip7_path, O_RDWR | O_CLOEXEC);
    int fd_1 = open(gpiochip6_path, O_RDWR | O_CLOEXEC);

    // 设备打开失败检查
    if (fd_0 < 0 || fd_1 < 0)
    {
        printk(
            "[ERROR] 打开GPIO芯片失败（gpiochip7_fd=%d, gpiochip6_fd=%d, errno=%d）\n",
            fd_0, fd_1, errno);
        return -1;
    }
    printk("[INFO] GPIO芯片打开成功（gpiochip7_fd=%d, gpiochip6_fd=%d）\n",
           fd_0, fd_1);

    /********** 新增：执行异常测试（无效引脚） **********/
    printk("\n[TEST] ========== 开始：GPIO事件异常场景测试 ==========\n");
    // 测试gpiochip7的无效引脚（20，超出16个引脚范围）
    int abnormal_ret = test_event_abnormal(fd_0, INVALID_PIN);
    if (abnormal_ret == 0)
    {
        printk("[TEST] GPIO事件异常测试通过！\n");
    }
    else
    {
        printk("[TEST] GPIO事件异常测试失败！\n");
        ret = abnormal_ret; // 记录异常测试结果
    }
    printk("[TEST] ========== 结束：GPIO事件异常场景测试 ==========\n");
    k_sleep(K_SECONDS(2)); // 间隔2秒，区分测试阶段

    /********** 执行多引脚事件测试（核心测试） **********/
    printk("\n[TEST] ========== 开始：多引脚事件监听核心测试 ==========\n");
    test_multi_pin_event(fd_0, fd_1);

    // test_single_pin_event(fd_1, 4, GPIOEVENT_REQUEST_FALLING_EDGE); //
    // 单引脚测试（注释备用）

    /********** 关闭设备（理论上不会执行到此处） **********/
    close(fd_0);
    close(fd_1);

    return ret;
}
