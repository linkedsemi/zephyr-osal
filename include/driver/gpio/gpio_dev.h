
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/fs.h>
#include <zephyr/drivers/gpio.h>
#include <stdio.h>
#include <poll.h>
#include <linux/uapi/gpio.h>
#include <linux/uapi/ioctl.h>
#include <linux/types.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/posix/dirent.h>
#include <fcntl.h>

#define GPIOHANDLES_MAX 64
#define GPIO_MAX_NAME_SIZE 32

#define GPIOCHIP 0
#define LINEHANDLE 1
#define LINEEVT 2

#define LINE_MAX 20
#define LINEEVENT_MAX 30

// GPIO 描述符
struct gpio_desc {
    uint32_t pin;             // 引脚编号
    uint32_t flags;           // 配置标志
};


int zvfs_open(const char *name, int flags, int mode);

/* todo wangying*/
int major(dev_t dev);

/* todo wangying*/
int minor(dev_t dev);

// todo wangying
int alphasort(const struct dirent **a, const struct dirent **b);

// todo wangying
char *realpath(const char *path, char *resolved_path);

int access(const char *pathname, int mode);

/* 用于监视文件描述符集合的状态变化（如可读、可写或异常），同时支持超时和信号掩码控制 */
int ppoll(struct pollfd *fds, unsigned int nfds, const struct timespec *timeout_ts, const sigset_t *sigmask);

int scandir(const char *dirpath, struct dirent ***namelist,
    int (*filter)(const struct dirent *),
    int (*compar)(const struct dirent **, const struct dirent **));