#pragma once

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int timespec_get(struct timespec *ts, int base);

#ifdef __cplusplus
}
#endif
