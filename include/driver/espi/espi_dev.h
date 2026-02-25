#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/espi.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zvfs/eventfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


struct espi_fs_device {
    const struct device *dev;      
    const char *label;              
    struct espi_callback cb;        
    struct k_poll_signal signal;    
    uint8_t last_status;
    struct k_sem rx_sem;            
    atomic_t is_open; 
    int fs_flags; 
};
