#include <stdio.h>
#include <fcntl.h>
#include <zephyr/posix/sys/ioctl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/dirent.h>
#include <zephyr/sys/printk.h>


void main(void) {
    int fd = open("/dev/uart0/config", O_RDWR);
    struct termios tio;
    
    // 获取当前配置
    ioctl(fd, TCGETS, &tio); 
    
    // 修改波特率为115200
    cfsetospeed(&tio, B115200);
    ioctl(fd, TCSETS, &tio);
    
    // 非阻塞读取数据
    fcntl(fd, F_SETFL, O_NONBLOCK);
    read(fd, buf, sizeof(buf));
}
