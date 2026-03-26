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
    assert(!"pipe() should NEVER be called on Zephyr!");

    return -1;
}
