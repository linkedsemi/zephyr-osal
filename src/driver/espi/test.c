#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/posix/sys/ioctl.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/poll.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>

static int espi_wait_for_event(int fd, int timeout_ms)
{
    struct pollfd fds;
    int ret;

    fds.fd = fd;
    fds.events = POLLIN; /* Wait for data to read */

    ret = poll(&fds, 1, timeout_ms);

    if (ret < 0) {
        printf("Poll error\n");
        return -1;
    }

    if (ret == 0) {
        return 0; /* Timeout */
    }

    if (fds.revents & POLLIN) {
        return 1; /* Data ready */
    }

    return 0;
}

static int espi_fs_monitor(const struct shell *sh, size_t argc, char **argv)
{
    char *dev_name = "/dev/espi_pltrstn";
    uint32_t total_monitor_sec = 20; 
    int poll_timeout_ms = 1000;      
    
    int fd;
    uint8_t signal_level = 0;
    int ret;

    if (argc > 1) {
        dev_name = argv[1];
    }
    
    if (argc > 2) {
        total_monitor_sec = strtoul(argv[2], NULL, 0);
    }

    if (argc > 3) {
        poll_timeout_ms = strtoul(argv[3], NULL, 0);
    }

    printf("Opening device: %s\n", dev_name);
    printf("Monitoring for %d seconds (Poll interval: %d ms)...\n", total_monitor_sec, poll_timeout_ms);

    fd = open(dev_name, O_RDONLY);
    if (fd < 0) {
        printf("Failed to open %s. Error: %d\n", dev_name, fd);
        return -ENODEV;
    }

    int64_t start_time = k_uptime_get();
    int64_t end_time = start_time + (total_monitor_sec * 1000);

    while (k_uptime_get() < end_time) {

        ret = espi_wait_for_event(fd, poll_timeout_ms);

        if (ret > 0) {
            int bytes = read(fd, &signal_level, 1);
            
            if (bytes == 1) {
                uint32_t elapsed_sec = (uint32_t)((k_uptime_get() - start_time) / 1000);
                printf("[%d s] PLTRST Signal Level Changed: %c\n", elapsed_sec, signal_level);
            } else if (bytes < 0) {
                printf("Read error: %d\n", bytes);
                break;
            }
        } else if (ret == 0) {

             printf("."); 
        }

        fflush(stdout);
    }

    printf("\nClosing device.\n");
    close(fd);

    return 0;
}

SHELL_CMD_REGISTER(espi_fs_monitor, NULL, "Usage: espi_fs_monitor <dev> <total_sec> <poll_ms>", espi_fs_monitor);