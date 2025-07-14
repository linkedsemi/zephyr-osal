#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

// 正确路径应为你实际挂载的 /sys/class/leds/...
#define LED_PATH "/sys/class/leds/LED0"

static int read_trigger(char *buf, size_t buf_size)
{
    char path[128];
    const char *full_path = path;

    snprintf(path, sizeof(path), "%s/%s", LED_PATH, "trigger");

    int fd = open(full_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        printk("[TEST] Failed to open %s for reading: errno=%d\n", full_path, errno);
        return -1;
    }

    ssize_t bytes_read = read(fd, buf, buf_size - 1);
    close(fd);

    if (bytes_read <= 0) {
        printk("[TEST] Failed to read from %s: errno=%d\n", full_path, errno);
        return -1;
    }

    // 确保字符串以空字符结尾
    buf[bytes_read] = '\0';
    
    // 移除换行符
    char *newline = strchr(buf, '\n');
    if (newline) {
        *newline = '\0';
    }

    printk("[TEST] Current trigger: %s\n", buf);
    return 0;
}

static int write_file(const char *node, const char *value)
{
    char path[128];
    const char *full_path = path;

    snprintf(path, sizeof(path), "%s/%s", LED_PATH, node);

    int fd = open(full_path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        printk("[TEST] Failed to open %s: errno=%d\n", full_path, errno);
        return -1;
    }

    ssize_t ret = write(fd, value, strlen(value));
    close(fd);

    if (ret < 0) {
        printk("[TEST] Failed to write to %s: errno=%d\n", full_path, errno);
        return -1;
    }

    printk("[TEST] Successfully wrote to %s: %s\n", full_path, value);
    return 0;
}

static int write_ulong(const char *node, unsigned long value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", value);
    return write_file(node, buf);
}

static int read_ulong(const char *node, unsigned long *out_value)
{
    char path[128];
    const char *full_path = path;

    snprintf(path, sizeof(path), "%s/%s", LED_PATH, node);

    int fd = open(full_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        printk("[TEST] Failed to open %s for reading: errno=%d\n", full_path, errno);
        return -1;
    }

    char buf[32];
    ssize_t bytes_read = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (bytes_read <= 0) {
        printk("[TEST] Failed to read from %s: errno=%d\n", full_path, errno);
        return -1;
    }

    buf[bytes_read] = '\0';
    *out_value = strtoul(buf, NULL, 10);

    printk("[TEST] Read from %s: %lu\n", full_path, *out_value);
    return 0;
}

static int read_delay_on(unsigned long *out_value)
{
    return read_ulong("delay_on", out_value);
}

static int read_delay_off(unsigned long *out_value)
{
    return read_ulong("delay_off", out_value);
}

int led_main_test(void)
{
    printk("[TEST] led_main_test start\n");
    unsigned long max_brightness = 0;
    unsigned long brightness = 0;
    unsigned long delay_on = 0;
    unsigned long delay_off = 0;
    char trigger_value[32];

    printk("[TEST] led.getMaxBrightness() start---------\n");
    // The led.getMaxBrightness() test directly accesses /sys/class/leds/max_brightness
    const char *file_name = "/sys/class/leds/LED0/max_brightness";
    int fd = open(file_name, O_RDWR | O_CLOEXEC);
    printk("[TEST] getMaxBrightness open file_name=[%s], fd=[%d]\n", file_name, fd);
    if (fd < 0) {
        printk("[TEST] ----Failed to open LED0 device: errno=%d\n", errno);
        return -1;
    }
    char buf[32];
    memset(buf, 0x00, sizeof(buf));
    ssize_t bytes_read = read(fd, buf, sizeof(buf) - 1);
    printk("[TEST] getMaxBrightness Read complete. Bytes read: %zd\n", bytes_read);
    close(fd);

    if (bytes_read <= 0) {
        printk("[TEST] getMaxBrightness Error reading from device: errno=%d\n", errno);
        return -1;
    }

    printk("[TEST] getMaxBrightness: Data received:\n");
    buf[bytes_read] = '\0';
    printk("[TEST] Read value: '%s'\n", buf);

    unsigned long out_value = strtoul(buf, NULL, 10);
    /* Save the maximum brightness */
    max_brightness = out_value;
    printk("[TEST] Converted max brightness buf = [%s], value: %lu\n", buf, out_value);
    printk("[TEST] led.getMaxBrightness() end---------\n");

    /* Save the maximum brightness */ 
    printk("[TEST] Now attempting to set brightness to max...start\n");
    if (write_ulong("brightness", out_value) != 0) {
        printk("[TEST] Failed to set brightness to %lu\n", out_value);
    } else {
        printk("[TEST] Brightness set successfully to %lu\n", out_value);
    }
    printk("[TEST] Now attempting to set brightness to max...end\n");

    /* Read the current firing mode */ 
    printk("[TEST] Reading initial trigger mode...\n");
    if (read_trigger(trigger_value, sizeof(trigger_value)) != 0) {
        printk("[TEST] Failed to read trigger mode\n");
        return -1;
    }
    printk("[TEST] Initial trigger mode: %s\n", trigger_value);

    printk("[TEST] read brightness start\n");
    /* Reading the current brightness */ 
    if (read_ulong("brightness", &brightness) != 0) {
        printk("[TEST] Failed to read current brightness\n");
        return -1;
    }
    printk("[TEST] Current brightness: %lu\n", brightness);

    /* Set blinking mode 10s*/ 
    printk("[TEST] Setting LED to blink mode...start\n");
    if (write_file("trigger", "timer") != 0 ||
        write_ulong("delay_on", 500) != 0 ||
        write_ulong("delay_off", 500) != 0) {
        printk("[TEST] Failed to set blink mode.\n");
    } else {
        printk("[TEST] Blink mode set: on=500ms, off=500ms\n");
    }

    /* The trigger mode verification is read again */ 
    printk("[TEST] Reading trigger mode after setting blink...\n");
    if (read_trigger(trigger_value, sizeof(trigger_value)) != 0) {
        printk("[TEST] Failed to read trigger mode\n");
        return -1;
    }
    printk("[TEST] Trigger mode after setting: %s\n", trigger_value);

    /* delay_on and delay_off are read and validated */ 
    printk("[TEST] Reading delay_on and delay_off...\n");
    if (read_delay_on(&delay_on) != 0 || read_delay_off(&delay_off) != 0) {
        printk("[TEST] Failed to read delay_on/delay_off\n");
        return -1;
    }
    printk("[TEST] Current delay_on: %lu ms, delay_off: %lu ms\n", delay_on, delay_off);
    
    /* Verify that the value is set correctly */
    if (delay_on != 500 || delay_off != 500) {
        printk("[TEST] Warning: delay_on/delay_off values do not match setting!\n");
    } else {
        printk("[TEST] delay_on/delay_off values verified successfully\n");
    }

    k_sleep(K_SECONDS(10));

    /* Turn off the LED 5s */ 
    printk("[TEST] Turning LED off...\n");
    if (write_ulong("brightness", 0) != 0 ||
        write_file("trigger", "none") != 0) {
        printk("[TEST] Failed to turn LED off.\n");
    } else {
        printk("[TEST] LED turned off successfully.\n");
    }
    k_sleep(K_SECONDS(5));

    /* Turn on the LED 5s */ 
    printk("[TEST] Turning LED on...\n");
    if (write_ulong("brightness", 100) != 0 ||
        write_file("trigger", "none") != 0) {
        printk("[TEST] Failed to turn LED on.\n");
    } else {
        printk("[TEST] LED turned on successfully.\n");
    }
    k_sleep(K_SECONDS(5));

    /* Turn off the LED 5s */ 
    printk("[TEST] Turning LED off...\n");
    if (write_ulong("brightness", 0) != 0 ||
        write_file("trigger", "none") != 0) {
        printk("[TEST] Failed to turn LED off.\n");
    } else {
        printk("[TEST] LED turned off successfully.\n");
    }
    k_sleep(K_SECONDS(5));
    
    printk("[TEST]----------end: LED sysfs test\n");
    
    return 0;
}