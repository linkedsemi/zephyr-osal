/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <termios.h>
#include <driver/uart/uart_dev.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/sys/fdtable.h>

LOG_MODULE_REGISTER(uart_dev, LOG_LEVEL_INF);

#define CONFIG_UART_DEV_MAX_SIZE 1024
#define MAX_UART_DEVICES 5

//#define DT_DRV_COMPAT linkedsemi_ls_uart
// #define DT_DRV_COMPAT ns16550
#define DT_DRV_COMPAT linkedsemi_vuart
#define UART_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(UART_NUM <= MAX_UART_DEVICES,
             "Exceeded maximum UART device count");

/* 设备初始化宏 */
#define UART_DEVICE_PROCESS(inst)                            \
    {                                                        \
        .dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)), \
        .label = DT_INST_PROP(inst, label),                  \
        .opened = ATOMIC_INIT(0)                             \
    },

static struct uart_device_info uart_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(UART_DEVICE_PROCESS)
};

static int uart_index = 0;

static struct fs_mount_t uartfs_mnt = {
    .type = FS_UART,
    .fs_data = &uart_devices,
    .mnt_point = UART_MNT_POINT
};

/* 文件系统接口实现 */

static const char *get_filename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static void uart_rx_handle(const struct device *dev, struct uart_device_info *uart_dev)
{
	uint8_t *data;
	uint32_t len;
	uint32_t rd_len;
	do {
		len = ring_buf_put_claim(&uart_dev->rx_ringbuf, &data,
					 uart_dev->rx_ringbuf.size);

		if (len > 0) {
			rd_len = uart_fifo_read(dev, data, len);
			int err = ring_buf_put_finish(&uart_dev->rx_ringbuf, rd_len);
			(void)err;
			__ASSERT_NO_MSG(err == 0);
		} else {
			uint8_t dummy;

			/* No space in the ring buffer - consume byte. */
			printk("RX ring buffer full.\n");

			rd_len = uart_fifo_read(dev, &dummy, 1);
		}
        // printk("rd_len = %d, len=%d\n", rd_len, len);
        // for (int i=0; i<rd_len; i++) {
        //     printk("%d ", data[i]);
        // }
        // printk("\n");

	} while (rd_len && (rd_len <= len));
    // printk("%s:%d\n", __func__, __LINE__);
    k_poll_signal_raise(&uart_dev->poll_sig, 1);  // 触发信号

}

static void uart_tx_handle(const struct device *dev, struct uart_device_info *uart_dev)
{
	uint32_t len;
	const uint8_t *data;

	len = ring_buf_get_claim(&uart_dev->tx_ringbuf, (uint8_t **)&data,
				 uart_dev->tx_ringbuf.size);
	if (len) {
		int err;
		// for(int i=0; i<len; i++) {
		//     printk("0x%x ", data[i]);
		// }
		// printk("\n");
		len = uart_fifo_fill(dev, data, len);
		err = ring_buf_get_finish(&uart_dev->tx_ringbuf, len);
		__ASSERT_NO_MSG(err == 0);
		ARG_UNUSED(err);
	} else {
		uart_irq_tx_disable(dev);
		uart_dev->tx_busy = 0;
	}
}

static void uart_callback(const struct device *dev, void *user_data)
{
	struct uart_device_info *uart_dev = (struct uart_device_info *)user_data;
	uart_irq_update(uart_dev->dev);

	if (uart_irq_rx_ready(uart_dev->dev)) {
		uart_rx_handle(uart_dev->dev, uart_dev);
	}

	if (uart_irq_tx_ready(uart_dev->dev)) {
		uart_tx_handle(uart_dev->dev, uart_dev);
	}
}

void init_uart_config(struct uart_device_info * uart_dev)
{
	struct uart_config uart_cfg;
	int rc;
    // const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	/* Get the current UART configuration */
	rc = uart_config_get(uart_dev->dev, &uart_cfg);
	if (rc) {
		LOG_ERROR("Can't read tty configuration!");
		return;
	}

    uart_cfg.baudrate = 115200;

	/* Set the UART configuration */
	rc = uart_configure(uart_dev->dev, &uart_cfg);
	if (rc) {
		LOG_ERROR("Can't set tty configuration!");
	}
}
static int uartdev_open(struct fs_file_t *zfp, const char *path, fs_mode_t mode)
{
    const char *name = get_filename(path);
    struct uart_device_info *uart_dev = NULL;

    // printk("%s:%d\n", __func__, __LINE__);

    for (int i = 0; i < UART_NUM; i++) {
        if (strcmp(uart_devices[i].label, name) == 0) {
            uart_dev = &uart_devices[i];
            break;
        }
    }

    if (!uart_dev) {
        LOG_ERROR("UART device %s not found", name);
        return -ENODEV;
    }

    if (!atomic_cas(&uart_dev->opened, 0, 1)) {
        LOG_ERROR("Device %s already opened", name);
        return -EBUSY;
    }

    if (!device_is_ready(uart_dev->dev)) {
        LOG_ERROR("Device %s not ready", name);
        atomic_set(&uart_dev->opened, 0);
        return -ENODEV;
    }
	ring_buf_init(&uart_dev->rx_ringbuf, RX_RING_BUFFER_SIZE, uart_dev->rx_buf);
	ring_buf_init(&uart_dev->tx_ringbuf, TX_RING_BUFFER_SIZE, uart_dev->tx_buf);
    /* 初始化终端属性 */
    // init_uart_config(uart_dev);
    //注册uart设备读写回调函数
	uart_dev->tx_busy = 0;
	uart_irq_callback_user_data_set(uart_dev->dev, uart_callback, (void *)uart_dev);
	uart_irq_rx_enable(uart_dev->dev);
    // printk("uart_irq_rx_enable!regVal=0x%x\n", *((int*)(0x40088004)) );
    //初始化 poll event，等待 signal
    k_poll_signal_init(&uart_dev->poll_sig);
    // printk("%s:%d\n", __func__, __LINE__);
    
    zfp->filep = uart_dev;
    return 0;
}

static int uartdev_close(struct fs_file_t *zfp)
{
    struct uart_device_info *uart_dev = zfp->filep;
    if (uart_dev) {
        atomic_set(&uart_dev->opened, 0);
    }
    uart_dev->tx_busy = 0;
	uart_irq_tx_disable(uart_dev->dev);
	uart_irq_rx_disable(uart_dev->dev);
    return 0;
}

static ssize_t uartdev_read(struct fs_file_t *zfp, void *buffer, size_t size)
{
    struct uart_device_info *uart_dev = zfp->filep;
    int readsize = ring_buf_get(&uart_dev->rx_ringbuf, buffer, size);
    uint8_t * receiveBuffer = (uint8_t *)buffer;
    // printk("%s:%d,uart_irq_rx_enable!regVal(0x40088004)=0x%x\n",__func__, __LINE__, *((int*)(0x40088004)) );
    // printk("%s:%d size=%d , readsize=%d\n", __func__, __LINE__, size, readsize);
    // for (int i=0; i<readsize; i++) {
    //     printk("%d ", receiveBuffer[i]);
    // }
    // printk("\n");
	return readsize;
}

static ssize_t uartdev_write(struct fs_file_t *zfp, const void *buffer, size_t size)
{
    struct uart_device_info *uart_dev = zfp->filep;

    const uint8_t *sendBuffer = (const uint8_t *)buffer;
    size_t total_written = 0;

    // for (int i=0; i<size; i++) {
    //     printk("%d ", sendBuffer[i]);
    // }
    // printk("\n");

    while (total_written < size) {
        size_t chunk = ring_buf_put(&uart_dev->tx_ringbuf,
                                    &sendBuffer[total_written],
                                    size - total_written);

        if (chunk == 0) {
            // Ring buffer 满了，但你应该等待它有空间
            // 在非阻塞模式下，可以返回已发送的字节数
            // 如果是阻塞模式，需要等待 TX 中断腾出空间
            k_sleep(K_MSEC(100));
            continue;
        }

        total_written += chunk;

        // 只有在首次写入 Ring Buffer 且 TX 未忙时，才启用 TX 中断
        if (atomic_get(&uart_dev->tx_busy) == 0) {
            if (atomic_set(&uart_dev->tx_busy, 1) == 0) {
                uart_irq_tx_enable(uart_dev->dev);
            }
        }
        // printk("%s:%d, wittensize=%d\n", __func__, __LINE__, total_written);
    }

    return total_written;
}

static int uart_poll_prepare_ctx(struct uart_device_info *uartDev,
				  struct zvfs_pollfd *pfd,
				  struct k_poll_event **pev,
				  struct k_poll_event *pev_end)
{
	if (pfd->events & ZVFS_POLLIN) {

		if (*pev == pev_end) {
			return -ENOMEM;
		}
		(*pev)->obj = &uartDev->poll_sig;
		(*pev)->type = K_POLL_TYPE_SIGNAL;
		(*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
		(*pev)->state = K_POLL_STATE_NOT_READY;
		(*pev)++;
	}
    return 0;
}


static int uart_poll_update_ctx(struct uart_device_info *uartDev,
				 struct zvfs_pollfd *pfd,
				 struct k_poll_event **pev)
{
    if ((pfd->events & ZVFS_POLLIN ) && ((*pev)->signal->result == 1)) {
        if ((*pev)->state != K_POLL_STATE_NOT_READY ) {
            pfd->revents |= ZVFS_POLLIN;
            k_poll_signal_reset(&uartDev->poll_sig);
            (*pev)->state = K_POLL_STATE_NOT_READY;
		}
		(*pev)++;
	}
    return 0;
}
static int uartdev_ioctl(struct fs_file_t *zfp, unsigned long cmd, va_list args)
{
    struct uart_device_info *dev = zfp->filep;
    int ret = 0;
    // printk("%s:%d cmd=%d\n", __func__, __LINE__, cmd);
    switch (cmd) {
    case TCGETATTR: {
        struct uart_config *uart_cfg = va_arg(args, struct uart_config*);
        uart_config_get(dev->dev, uart_cfg);
        break;
    }
    case TCSETATTR: {
        struct uart_config *uart_cfg  = va_arg(args, struct uart_config *);
        ret = uart_configure(dev->dev, uart_cfg);
        break;
    }
    // case FIONREAD: {
    //     int *avail = va_arg(args, int*);
    //     uint32_t count;
    //     uart_fifo_read(dev->dev, &count);
    //     *avail = count;
    //     break;
    // }
    case F_GETFL: // 获取文件状态标志
        return O_RDWR; // 假设 UART 设备总是可读可写

    case F_SETFL: { // 设置文件状态标志
        int flags = va_arg(args, int);
        // printk("flags:%d\n", flags);
        // 设置非阻塞模式（如果支持）
        dev->non_block = (flags & O_NONBLOCK) != 0;
        break;
    }
	case ZFD_IOCTL_POLL_PREPARE: {
		struct zvfs_pollfd *pfd;
		struct k_poll_event **pev;
		struct k_poll_event *pev_end;
		pfd = va_arg(args, struct zvfs_pollfd *);
		pev = va_arg(args, struct k_poll_event **);
		pev_end = va_arg(args, struct k_poll_event *);
		ret = uart_poll_prepare_ctx(dev, pfd, pev, pev_end);
        break;
	}
	case ZFD_IOCTL_POLL_UPDATE: {
		struct zvfs_pollfd *pfd;
		struct k_poll_event **pev;
		pfd = va_arg(args, struct zvfs_pollfd *);
		pev = va_arg(args, struct k_poll_event **);
		ret = uart_poll_update_ctx(dev, pfd, pev);
        break;
	}
    default:
        ret = -ENOTTY;
    }
    return ret;
}

/* 目录操作 */
static int uartdev_opendir(struct fs_dir_t *dp, const char *path)
{
    // printk("%s:%d \n", __func__, __LINE__);
    if (strcmp(path, UART_MNT_POINT) != 0) {
        LOG_ERROR("uartdev_opendir %s failed!\n", UART_MNT_POINT);
        return -ENOENT;
    }
    dp->dirp = &uart_index;
    return 0;
}

static int uartdev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
    int *index = dp->dirp;
    // printk("%s:%d \n", __func__, __LINE__);
    if (*index >= UART_NUM) {
        // 没有更多条目
	    memset(entry, 0, sizeof(struct fs_dirent));
        return 0; // 结束遍历
    }

    // 构造文件条目信息
    snprintf(entry->name, sizeof(entry->name), "%s", uart_devices[*index].label);
    entry->type = FS_DIR_ENTRY_FILE;
    entry->size = 0;
    (*index)++;
    
    return 0;
}

static int uartdev_closedir(struct fs_dir_t *dp)
{
    // printk("%s:%d \n", __func__, __LINE__);
    uart_index = 0;
    dp->dirp = NULL;
    return 0;
}

static int uartdev_stat(struct fs_mount_t *mountp,
                       const char *path, struct fs_dirent *entry)
{
    // printk("%s:%d \n", __func__, __LINE__);
    if (strcmp(path, UART_MNT_POINT) == 0) {
        entry->type = FS_DIR_ENTRY_DIR;
    } else {
        entry->type = FS_DIR_ENTRY_FILE;
    }
    entry->size = 0;
    return 0;
}

/* 文件系统挂载 */
static int uartdev_mount(struct fs_mount_t *mountp)
{
    // printk("%s:%d \n", __func__, __LINE__);
	// 检查挂载点是否为空
	if (mountp->mnt_point == NULL)
	{
		LOG_ERROR("Invalid mount point\n");
		return -EINVAL;
	}

    for (int i = 0; i < UART_NUM; i++) {
        char path[32];
        snprintf(path, sizeof(path), "%s/ttyS%d", UART_MNT_POINT, i);
        LOG_ERROR("%s mounted successfully!\n", path);
    }

	LOG_INF("UART device mounted successfully!\n");
    return 0;
}

static int uartdev_unmount(struct fs_mount_t *mountp)
{
    return 0;
}

/* 文件系统注册 */
static const struct fs_file_system_t uart_fs = {
    .mount = uartdev_mount,
    .unmount = uartdev_unmount,
    .open = uartdev_open,
    .close = uartdev_close,
    .read = uartdev_read,
    .write = uartdev_write,
    .lseek = NULL,
    .ioctl = uartdev_ioctl,
    .opendir = uartdev_opendir, // fs ls访问
    .readdir = uartdev_readdir, // fs ls访问
    .closedir = uartdev_closedir, // fs ls访问
    .stat = uartdev_stat, // fs cd访问
};

static int uart_dev_init(void)
{
    LOG_INF("uart_num = %d\n", UART_NUM);
    /* 检查所有UART设备 */
    for (int i = 0; i < UART_NUM; i++) {
        if (!device_is_ready(uart_devices[i].dev)) {
            LOG_ERROR("UART device %s not ready", uart_devices[i].label);
            return -ENODEV;
        }
    }

    /* 挂载文件系统 */
    int ret = fs_mount(&uartfs_mnt);
    if (ret != 0) {
        LOG_ERROR("Failed to mount UART FS: %d", ret);
        return ret;
    }
    LOG_INF("%s:%d mount success!\n", __func__, __LINE__);
    return 0;
}

static int uart_fs_init(void)
{
    LOG_INF("%s:%d", __func__, __LINE__);
    int ret = fs_register(FS_UART, &uart_fs);
    if (ret < 0) {
        LOG_ERROR("Failed to register UART FS: %d", ret);
        return ret;
    }

    return uart_dev_init();
}

#ifndef  CONFIG_UART_TEST
SYS_INIT(uart_fs_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
#else
void uart_main_test()
{
    printk("%s:%d\n", __func__, __LINE__);
    uart_fs_init();
}
#endif