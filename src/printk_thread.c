#include "printk_thread.h"

void printk_thread(const char *fmt, ...)
{
    uint64_t ts = k_uptime_get();
    const char *tname = k_thread_name_get(k_current_get());
    if (!tname)
    {
        tname = "unknown";
    }

    printk("[%5llu.%03llu][%s][%s:%u] ",
           ts / 1000, ts % 1000,
           tname,
           __func__, __LINE__);


    printk(fmt);
}
