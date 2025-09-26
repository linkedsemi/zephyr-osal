#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/kcs.h>
#include <zephyr/drivers/ipmi.h>
#include <stdio.h>

#include <linux/types.h>

#define KCS_MNT_POINT "/dev/kcs"


#define CONFIG_KCS_DEV_MAX_SIZE 512

struct kcs_device_info
{
   const struct device *dev;
   const char *label;
   atomic_t opened; // 原子操作
   uint8_t flags;
   struct k_poll_signal poll_sig;
   struct k_work_delayable rx_poller;
   uint8_t rx_buf[CONFIG_KCS_DEV_MAX_SIZE];
   size_t rx_len;
   atomic_t has_msg;
};