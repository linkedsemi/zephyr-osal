/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <driver/gpio/gpio_dev.h>
#include <zephyr/posix/poll.h>

LOG_MODULE_DECLARE(gpio_dev, LOG_LEVEL_INF);

// 提取主设备号 暂时使用
int major(dev_t dev)
{
    return (int)((dev >> 8) & 0xfff);
}

// 提取次设备号 暂时使用
int minor(dev_t dev)
{
    return (int)(dev & 0xff);
}

int scandir(const char *dirpath, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **))
{
    DIR *dir;
    struct dirent *entry;
    struct dirent **list = NULL;
    size_t count = 0;
    size_t allocated = 0;

    // 打开目录
    dir = opendir(dirpath);
    if (!dir)
    {
        return -1; // 打开失败
    }

    // 遍历目录条目
    while ((entry = readdir(dir)) != NULL)
    {
        // 如果有筛选函数且条目不符合条件，则跳过
        if (filter && !filter(entry))
        {
            continue;
        }

        // 动态扩展数组以容纳新条目
        if (count >= allocated)
        {
            size_t new_allocated = (allocated == 0) ? 16 : allocated * 2;
            struct dirent **new_list = realloc(list, new_allocated * sizeof(struct dirent *));
            if (!new_list)
            {
                closedir(dir);
                free(list);
                return -1; // 内存分配失败
            }
            list = new_list;
            allocated = new_allocated;
        }

        // 分配内存并复制条目
        list[count] = malloc(sizeof(struct dirent));
        if (!list[count])
        {
            closedir(dir);
            for (size_t i = 0; i < count; i++)
            {
                free(list[i]);
            }
            free(list);
            return -1; // 内存分配失败
        }
        memcpy(list[count], entry, sizeof(struct dirent));
        count++;
    }

    closedir(dir);

    // 如果没有符合条件的条目
    if (count == 0)
    {
        free(list);
        return 0;
    }

    // 排序条目
    if (compar)
    {
        qsort(list, count, sizeof(struct dirent *), (int (*)(const void *, const void *))compar);
    }

    // 返回结果
    *namelist = list;
    return count;
}

// 定义排序函数
int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

#if 0
int getcwd(char *buf, size_t size)
{
    // 默认返回根目录 "/"
    const char default_cwd[] = "/";
    if (size < strlen(default_cwd) + 1)
    {
        errno = ERANGE; // 缓冲区太小
        return -1;
    }

    strncpy(buf, default_cwd, size);
    return 0;
}
#endif

/*
 *   access() checks whether the calling process can access the filepath.
 *   If path is a symbolic link, it is dereferenced.
 */
int access(const char* pathname, int mode)
{
    if (pathname == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    //Only supports F_OK, other modes return unsupported
    if (mode == F_OK)
    {
        struct fs_dirent entry;
        int rc = fs_stat(pathname, &entry);
        if (rc < 0)
        {
            LOG_ERROR("access check failed! rc=%d", rc);
            errno = -rc;
            return -1;
        }
        return 0;
    }

    // R_OK/W_OK/X_OK semantics not implemented
    LOG_ERROR("access mode not supported: %d", mode);
    errno = ENOTSUP;
    return -1;
}

int ppoll(struct pollfd *fds, unsigned int nfds, const struct timespec *timeout_ts, const sigset_t *sigmask)
{
    int ret;
    uint32_t timeout_ms;

    // 如果未提供超时时间，返回错误
    if (timeout_ts == NULL)
    {
        errno = EINVAL; // 无效参数
        return -1;
    }

    // 如果提供了超时时间，则将其转换为毫秒
    timeout_ms = timeout_ts->tv_sec * MSEC_PER_SEC + timeout_ts->tv_nsec / NSEC_PER_MSEC;

    // Zephyr 不支持信号掩码，忽略 sigmask 参数
    ARG_UNUSED(sigmask);

    // 使用 Zephyr 的 poll 实现
    ret = poll(fds, nfds, timeout_ms);
    if (ret < 0)
    {
        errno = -ret; // 设置 errno
        return -1;
    }

    return ret;
}
