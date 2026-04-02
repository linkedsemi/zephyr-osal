#pragma once

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#ifdef __cplusplus
extern "C" {
#endif


void printk_thread(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
