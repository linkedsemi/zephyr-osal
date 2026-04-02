#pragma once

#include <zephyr/posix/unistd.h>
#include <assert.h>

/*
 * 弱符号：防止和系统库冲突
 * 只为链接器通过
 */
__attribute__((weak))
int pipe(int __fildes[2])
{
    __ASSERT_UNREACHABLE;

    return -1;
}
