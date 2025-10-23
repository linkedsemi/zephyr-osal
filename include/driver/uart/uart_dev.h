#ifndef _UART_DEV_H
#define _UART_DEV_H

#include <zephyr/sys/ring_buffer.h>

#define UART_MNT_POINT "/dev/uart"
#define RXBUF_SIZE 128

#ifndef RX_RING_BUFFER_SIZE
#define RX_RING_BUFFER_SIZE 4096
#endif

#ifndef TX_RING_BUFFER_SIZE
#define TX_RING_BUFFER_SIZE 1024
#endif

/* 设备上下文结构 */
struct uart_device_info {
    const struct device *dev;
    // struct termios termios;
    atomic_t opened;
    struct k_poll_signal poll_sig;
    uint8_t non_block;
    char label[16];
    atomic_t tx_busy;
	struct ring_buf tx_ringbuf;
	struct ring_buf rx_ringbuf;
	uint8_t tx_buf[TX_RING_BUFFER_SIZE];
	uint8_t rx_buf[RX_RING_BUFFER_SIZE];
	// uint8_t *tx_buffer;
	// uint32_t tx_size;
	// uint8_t rx_buffer[RXBUF_SIZE];
	// uint32_t rx_size;
};

enum EnumUartIoctl {
    TCGETATTR,
    TCSETATTR,
};

#define O_NONBLOCK 1
#define O_RDWR 0


#endif /* _UART_DEV_H */