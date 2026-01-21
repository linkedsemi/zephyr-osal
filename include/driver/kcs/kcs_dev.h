#include <linux/types.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/ipmi.h>
#include <zephyr/drivers/kcs.h>

#define KCS_MNT_POINT "/dev/kcs"

#define CONFIG_KCS_DEV_MAX_SIZE 512

#define MAX_KCS_DEVICES 1 /* The maximum number of KCS devices */

#define KCS_ZERO_DATA 0

#define RX_BUF_COUNT 4
#define RX_BUF_SIZE CONFIG_KCS_DEV_MAX_SIZE

/* Global static receive buffer pool (independent for each device) */
static uint8_t kcs_raw_buffers[MAX_KCS_DEVICES][RX_BUF_COUNT][RX_BUF_SIZE];

struct kcs_rx_packet
{
    void* fifo_reserved; /* for k_fifo */
    uint8_t* data_ptr;   /* points into kcs_raw_buffers[dev_idx][i] */
    size_t len;
};

struct kcs_device_info
{
    const struct device* dev;
    const char* label;
    atomic_t opened;

    /* Poll support */
    struct k_poll_signal poll_sig;

    /* buffer management */
    struct k_fifo empty_queue; /* free packets */
    struct k_fifo data_queue;  /* received packets */

    struct kcs_rx_packet packet_pool[RX_BUF_COUNT];
    int dev_idx; /* index in kcs_devices[] */
};
