#include <stdio.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/sys/ioctl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/dirent.h>
#include <zephyr/sys/printk.h>
#include <poll.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(uart_test, LOG_LEVEL_ERR); // 注册日志模块
char buf[1024] = {0};
void uart_main(char *devPath)
{
    int tty_fd = open(devPath, O_RDWR);
    int writeLen = 0;
    // printk("%s:%d *********open ttyS0***************\n", __func__, __LINE__);

    //fcntl(tty_fd, F_SETFL, FNDELAY);//设置文件状态为非阻塞模式
    int flags = fcntl(tty_fd, F_GETFL, 0);
    fcntl(tty_fd, F_SETFL, flags | O_NONBLOCK);
    // printk("%s:%d **********fcntl ttyS0**************\n", __func__, __LINE__);
    // printk("%s:%d,uart_irq_rx_enable!regVal(0x40088004)=0x%x\n",__func__, __LINE__, *((int*)(0x40088004)) );

    int timeout = 10000;//10s
    struct pollfd pollfds[1] = {0};
    int n_pollers = 0;
    pollfds[n_pollers].fd = tty_fd;
	pollfds[n_pollers].events = POLLIN;
    n_pollers++;
    // LOG_ERROR("poll start,revents=%d\n",pollfds[0].revents);
    poll(pollfds, n_pollers, timeout);
    // LOG_ERROR("poll endrevents=%d\n",pollfds[0].revents);

    int len = read(tty_fd, buf, sizeof(buf));
    printk("%s:%d **********read %d bytes***\n", __func__, __LINE__, len);
    for(int i=0; i<len; i++) {
        printk("%c", buf[i]);
    }
    printk("\n");

    buf[0] = '1';
    buf[1] = '2';
    buf[2] = '3';
    buf[3] = '4';
    buf[4] = '5';
    buf[6] = '6';
    buf[7] = '7';
    buf[8] = '8';
    buf[9] = '9';
    buf[10] = 'A';
    buf[11] = '\0';

    writeLen = 12;

    write(tty_fd, buf, writeLen);

    close(tty_fd);
}

static int cmd_uart_test(const struct shell *sh, size_t argc, char **argv)
{
	//shell_print(sh, "%s", cwd);
    uart_main(argv[1]);

	return 0;
}


SHELL_CMD_ARG_REGISTER(uartTest, NULL, "uart dev file system test", cmd_uart_test, 1, 3);