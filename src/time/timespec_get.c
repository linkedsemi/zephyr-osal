#include <zephyr/kernel.h>
#include <timespec_get.h>

#ifndef TIME_UTC
#define TIME_UTC 1
#endif

/* There is no other implemented value than TIME_UTC; all other values
 * are considered erroneous. */
int timespec_get(struct timespec * ts, int base)
{
    if (base != TIME_UTC || ts == NULL) return 0;
    int ret = clock_gettime(CLOCK_REALTIME, ts);
    return ret < 0 ? 0 : base;
}
