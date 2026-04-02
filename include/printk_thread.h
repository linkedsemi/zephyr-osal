#pragma once

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_THREAD_NAME

#define printk_thread(fmt, ...) \
do { \
    uint64_t ts = k_uptime_get(); \
    printk("[%02llu:%02llu:%02llu.%03llu,000][%s][%s: %u] " fmt "\n", \
        (ts / 1000) / 3600, ((ts / 1000) % 3600) / 60, \
        (ts / 1000) % 60, ts % 1000, \
        k_thread_name_get(k_current_get()), \
        __FUNCTION__, __LINE__, \
        ##__VA_ARGS__); \
} while (0)

#endif

#ifdef __cplusplus
}
#endif
