#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/kcs.h>
#include <zephyr/drivers/ipmi.h>
#include <stdio.h>

#include <linux/types.h>

#define KCS_MNT_POINT "/dev/kcs"

struct kcs_device_info
{
   const struct device *dev;
   const char *label;
   atomic_t opened; // 原子操作
   uint8_t flags;
};