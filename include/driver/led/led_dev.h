#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <linux/types.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/fdtable.h>

#include <zephyr/drivers/led.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>

#define LED_MNT_POINT "/sys/class/leds"