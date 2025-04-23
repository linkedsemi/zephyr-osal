#ifndef _UART_DEV_H
#define _UART_DEV_H

#include <zephyr/sys/ring_buffer.h>

#define UART_MNT_POINT "/dev-uart"
#define RXBUF_SIZE 128

#ifndef CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE
#define CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE 128
#endif

#ifndef CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE
#define CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE 128
#endif

/* 设备上下文结构 */
struct uart_device_info {
    const struct device *dev;
    struct termios termios;
    atomic_t opened;
    struct k_poll_signal poll_sig;
    uint8_t non_block;
    char label[16];
    atomic_t tx_busy;
	struct ring_buf tx_ringbuf;
	struct ring_buf rx_ringbuf;
	uint8_t tx_buf[CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE];
	uint8_t rx_buf[CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE];
	// uint8_t *tx_buffer;
	// uint32_t tx_size;
	// uint8_t rx_buffer[RXBUF_SIZE];
	// uint32_t rx_size;
};

enum EnumUartIoctl {
    TCGETATTR,
    TCSETATTR,
    F_GETFL,
    F_SETFL,
};

#define O_NONBLOCK 1
#define O_RDWR 0


#endif /* _UART_DEV_H */