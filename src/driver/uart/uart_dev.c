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

LOG_MODULE_REGISTER(uart_dev, LOG_LEVEL_DBG);

#define CONFIG_UART_DEV_MAX_SIZE 1024
#define MAX_UART_DEVICES 5

#define DT_DRV_COMPAT linkedsemi_ls_uart
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

static void zephyr_cfmakeraw(struct termios *termios_p)
{
    termios_p->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL | IXON);
    termios_p->c_oflag &= ~OPOST;
    termios_p->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    termios_p->c_cflag &= ~(CSIZE | PARENB);
    termios_p->c_cflag |= CS8;
    
    termios_p->c_cc[VMIN] = 1;  // 最小读取字符数
    termios_p->c_cc[VTIME] = 0; // 超时时间（十分之一秒）
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
			LOG_WRN("RX ring buffer full.");

			rd_len = uart_fifo_read(dev, &dummy, 1);
		}
	} while (rd_len && (rd_len == len));

    // int read;
    // uint8_t *buffer =  uart_dev->rx_buffer;
    // int size = uart_dev->rx_size;

    // // 循环读取数据，直到所有数据都接收完毕
    // while (size > 0) {
    //     read = uart_fifo_read(dev, buffer, size);
    //     buffer += read;
    //     size -= read;

    //     // 如果 FIFO 为空，等待一段时间再重试
    //     if (size > 0) {
    //         k_sleep(K_MSEC(1)); // 使用 Zephyr 的延迟函数
    //     }
    // }
}

static void uart_tx_handle(const struct device *dev, struct uart_device_info *uart_dev)
{
	uint32_t len;
	const uint8_t *data;

	len = ring_buf_get_claim(&uart_dev->tx_ringbuf, (uint8_t **)&data,
				 uart_dev->tx_ringbuf.size);
	if (len) {
		int err;

		len = uart_fifo_fill(dev, data, len);
		err = ring_buf_get_finish(&uart_dev->tx_ringbuf, len);
		__ASSERT_NO_MSG(err == 0);
		ARG_UNUSED(err);
	} else {
		uart_irq_tx_disable(dev);
		uart_dev->tx_busy = 0;
	}

	// uint32_t size;
    // int written;
	// const uint8_t *data;

	// size = uart_dev->tx_size;
    // data = uart_dev->tx_buffer;

    // // 循环写入数据，直到所有数据都发送完毕
    // while (size > 0) {
    //     written = uart_fifo_fill(uart_dev->dev, data, size);
    //     data += written;
    //     size -= written;
    //     // 如果 FIFO 已满，等待一段时间再重试
    //     if (size > 0) {
    //         k_sleep(K_MSEC(1)); // 使用 Zephyr 的延迟函数
    //     }
    // }

    // uart_irq_tx_disable(uart_dev->dev);
    // uart_dev->tx_busy = 0;
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

static int uartdev_open(struct fs_file_t *zfp, const char *path, fs_mode_t mode)
{
    const char *name = get_filename(path);
    struct uart_device_info *uart_dev = NULL;
    //struct uart_device_info * uart_devices = zfp->mp->fs_data;

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

	ring_buf_init(&uart_dev->rx_ringbuf, CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE,
		      uart_dev->rx_buf);
	ring_buf_init(&uart_dev->tx_ringbuf, CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE,
		      uart_dev->tx_buf);
    //注册uart设备读写回调函数
	uart_dev->tx_busy = 0;
	uart_irq_callback_user_data_set(uart_dev->dev, uart_callback, (void *)uart_dev);
	uart_irq_rx_enable(uart_dev->dev);
    /* 初始化终端属性 */
    zephyr_cfmakeraw(&uart_dev->termios);
    //k_poll_signal_init(&uart_dev->poll_sig);
    
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
	return readsize;
    // uint32_t partial_size = 0;
	// uint32_t total_size = 0U;
    // uint8_t *data = (uint8_t *)buffer;
    // struct uart_device_info *uart_dev = zfp->filep;
	// do {
    //     partial_size = size > uart_dev->rx_size ? uart_dev->rx_size : size;
	// 	if (data) {
	// 		memcpy(data, uart_dev->rx_buffer, partial_size);
	// 		data += partial_size;
	// 	}
	// 	total_size += partial_size;
	// 	size -= partial_size;
	// } while (size && partial_size);

	// return total_size;
}

static ssize_t uartdev_write(struct fs_file_t *zfp, const void *buffer, size_t size)
{
    struct uart_device_info *uart_dev = zfp->filep;
	ssize_t writtensize = ring_buf_put(&uart_dev->tx_ringbuf, buffer, size);

	if (atomic_set(&uart_dev->tx_busy, 1) == 0) {
		uart_irq_tx_enable(uart_dev->dev);
	}

	return writtensize;

    // struct uart_device_info *uart_dev = zfp->filep;

	// uart_dev->tx_size = size;
    // uart_dev->tx_buffer = (uint8_t *)buffer;

	// if (atomic_set(&uart_dev->tx_busy, 1) == 0) {
	// 	uart_irq_tx_enable(uart_dev->dev);
	// }
    // return size;
}

static int uartdev_ioctl(struct fs_file_t *zfp, unsigned long cmd, va_list args)
{
    struct uart_device_info *dev = zfp->filep;
    int ret = 0;

    switch (cmd) {
    case TCGETATTR: {
        struct termios *tio = va_arg(args, struct termios*);
        *tio = dev->termios;
        break;
    }
    case TCSETATTR: {
        struct termios *tio = va_arg(args, struct termios*);
        dev->termios = *tio;
        
        struct uart_config uart_cfg = {
            //.baudrate = cfgetospeed(tio),
            .parity = (tio->c_cflag & PARENB) ? 
                     ((tio->c_cflag & PARODD) ? UART_CFG_PARITY_ODD : UART_CFG_PARITY_EVEN) :
                     UART_CFG_PARITY_NONE,
            .stop_bits = (tio->c_cflag & CSTOPB) ? UART_CFG_STOP_BITS_2 : UART_CFG_STOP_BITS_1,
            .data_bits = (tio->c_cflag & CSIZE) == CS8 ? UART_CFG_DATA_BITS_8 :
                        (tio->c_cflag & CSIZE) == CS7 ? UART_CFG_DATA_BITS_7 :
                        UART_CFG_DATA_BITS_6,
            .flow_ctrl = (tio->c_cflag & CRTSCTS) ? UART_CFG_FLOW_CTRL_RTS_CTS : 
                        UART_CFG_FLOW_CTRL_NONE
        };
        
        ret = uart_configure(dev->dev, &uart_cfg);
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
        // 设置非阻塞模式（如果支持）
        dev->non_block = (flags & O_NONBLOCK) != 0;
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
    if (strcmp(path, UART_MNT_POINT) != 0) {
        return -ENOENT;
    }
    dp->dirp = &uart_index;
    return 0;
}

static int uartdev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
    int *index = dp->dirp;

    if (*index >= UART_NUM) {
        return 0; // 结束遍历
    }

    snprintf(entry->name, sizeof(entry->name), "uart%d", *index);
    entry->type = FS_DIR_ENTRY_FILE;
    entry->size = 0;
    (*index)++;
    
    return 0;
}

static int uartdev_closedir(struct fs_dir_t *dp)
{
    uart_index = 0;
    dp->dirp = NULL;
    return 0;
}

static int uartdev_stat(struct fs_mount_t *mountp,
                       const char *path, struct fs_dirent *entry)
{
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
	// 检查挂载点是否为空
	if (mountp->mnt_point == NULL)
	{
		LOG_ERROR("Invalid mount point\n");
		return -EINVAL;
	}

    for (int i = 0; i < UART_NUM; i++) {
        char path[32];
        snprintf(path, sizeof(path), "%s/uart%d", UART_MNT_POINT, i);
        LOG_ERROR("%s mounted successfully!\n", path);
    }

	LOG_ERROR("UART device mounted successfully!\n");
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
    .ioctl = uartdev_ioctl,
    .opendir = uartdev_opendir, // fs ls访问
    .readdir = uartdev_readdir, // fs ls访问
    .closedir = uartdev_closedir, // fs ls访问
    .stat = uartdev_stat, // fs cd访问
};

static int uart_dev_init(void)
{
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

    return 0;
}

static int uart_init(void)
{
    int ret = fs_register(FS_UART, &uart_fs);
    if (ret < 0) {
        LOG_ERROR("Failed to register UART FS: %d", ret);
        return ret;
    }

    return uart_dev_init();
}

SYS_INIT(uart_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);