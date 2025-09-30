/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <driver/kcs/kcs_dev.h>
#include <linux/ipmi_bmc.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/drivers/ipmi.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(kcs_dev, LOG_LEVEL_INF); // 注册日志模块

#define MAX_KCS_DEVICES 14 /* The maximum number of KCS devices */

#define KCS_ZERO_DATA 0

#define KCS_POLL_INTERVAL_MS 100 // 轮询周期

#define DT_DRV_COMPAT linkedsemi_ls_ipmi_kcs

#define KCS_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(KCS_NUM <= MAX_KCS_DEVICES, "Exceeded maximum KCS device count");

#define KCS_DEVICE_PROCESS(inst)                                               \
    {.dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)),                       \
     .label = DT_INST_PROP(inst, label),                                       \
     .opened = ATOMIC_INIT(0)},

struct kcs_device_info kcs_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(KCS_DEVICE_PROCESS)};

// struct kcs_device_info kcs_devices[KCS_NUM];

static int kcs_index = 0; // 当前遍历的 KCS 设备索引

static struct fs_mount_t kcsfs_mnt = {
    .type = FS_KCS,
    .fs_data = &kcs_devices,
};

static const char* getFileName(const char* filePath)
{
    const char* lastSlash = strrchr(filePath, '/');
    if (lastSlash != NULL)
    {
        return lastSlash + 1;
    }
    return filePath;
}

static int kcsdev_open(struct fs_file_t* zfp, const char* file_name,
                       fs_mode_t mode)
{
    LOG_ERROR("kcsdev_open start----\n");
    struct kcs_device_info* client = NULL;

    // 遍历所有 KCS 设备实例，找到对应的设备
    for (size_t i = 0; i < KCS_NUM; i++)
    {
        if (strcmp(kcs_devices[i].label, getFileName(file_name)) == 0)
        {
            client = &kcs_devices[i];
            break;
        }
    }

    if (client == NULL)
    {
        LOG_ERROR("Error: KCS device with name '%s' not found\n", file_name);
        return -ENODEV;
    }

    if (!atomic_cas(&client->opened, 0, 1))
    {
        LOG_ERROR("device name is opened");
        return -EINVAL;
    }

    if (!device_is_ready(client->dev))
    {
        LOG_ERROR("Error: KCS device with name '%s' is not ready\n", file_name);
        atomic_set(&client->opened, 0);
        return -ENODEV;
    }

    if (zfp->mp == NULL)
    {
        LOG_ERROR("Error: File system mount point is NULL\n");
        atomic_set(&client->opened, 0);
        return -EINVAL;
    }

    zfp->filep = client;
    zfp->mp = &kcsfs_mnt;

    LOG_ERROR("KCS device with name '%s' opened successfully\n", file_name);
    return 0;
}

static int kcsdev_close(struct fs_file_t* zfp)
{
    LOG_ERROR("kcsdev_close start----\n");
    if (zfp->filep)
    {
        struct kcs_device_info* client = zfp->filep;
        atomic_set(&client->opened, 0);
    }
    if (zfp->mp)
    {
        zfp->mp = NULL;
        LOG_ERROR("Closed kcsfs mount");
    }

    return 0;
}

// 轮询处理函数：尝试从底层读取一条请求，读到就缓存并唤醒 POLLIN
static void kcs_rx_poll_work(struct k_work* work)
{
    struct k_work_delayable* dwork = CONTAINER_OF(work, struct k_work_delayable,
                                                  work);
    // 遍历所有设备，逐个轮询
    for (int i = 0; i < KCS_NUM; i++)
    {
        struct kcs_device_info* client = &kcs_devices[i];

        // 若已有待取消息，跳过本轮（等用户态 read 取走）
        if (atomic_get(&client->has_msg))
        {
            continue;
        }

        // 直接调用 ipmi_read尝试读取消息
        int ret = ipmi_read(client->dev, client->rx_buf,
                            sizeof(client->rx_buf));

        LOG_DBG("kcs_rx_poll_work ret:%d\n", ret);

        if (ret > 0)
        {
            client->rx_len = (size_t)ret;
            atomic_set(&client->has_msg, 1);
            // 唤醒等 POLLIN 的用户
            k_poll_signal_raise(&client->poll_sig, 1);
        }
    }

    // 重新安排下一轮
    k_work_schedule(dwork, K_MSEC(KCS_POLL_INTERVAL_MS));
}

// poll 的 prepare/update：使用 k_poll_signal 通知 POLLIN
static int kcs_poll_prepare_ctx(struct kcs_device_info* kcsDev,
                                struct zvfs_pollfd* pfd,
                                struct k_poll_event** pev,
                                struct k_poll_event* pev_end)
{
    if (pfd->events & ZVFS_POLLIN)
    {
        if (*pev == pev_end)
        {
            return -ENOMEM;
        }
        (*pev)->signal = &kcsDev->poll_sig;
        (*pev)->type = K_POLL_TYPE_SIGNAL;
        (*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
        (*pev)->state = K_POLL_STATE_NOT_READY;

        // 如果此时已有待取消息，直接让 poll 立刻返回
        if (atomic_get(&kcsDev->has_msg))
        {
            k_poll_signal_raise(&kcsDev->poll_sig, 1);
        }

        (*pev)++;
    }
    return 0;
}

static int kcs_poll_update_ctx(struct kcs_device_info* kcsDev,
                               struct zvfs_pollfd* pfd,
                               struct k_poll_event** pev)
{
    if (pfd->events & ZVFS_POLLIN)
    {
        unsigned int signaled = 0;
        int result = 0;
        k_poll_signal_check(&kcsDev->poll_sig, &signaled, &result);
        if (signaled)
        {
            pfd->revents |= ZVFS_POLLIN;
            k_poll_signal_reset(&kcsDev->poll_sig);
        }
        (*pev)++;
    }
    return 0;
}

// kcsdev_read：优先从设备文件"待取缓冲"返回，若没有消息则调用api读取
static ssize_t kcsdev_read(struct fs_file_t* zfp, void* ptr, size_t size)
{
    LOG_ERROR("kcsdev_read start----\n");
    struct kcs_device_info* client = zfp->filep;

    if (client == NULL)
    {
        LOG_ERROR("No client context");
        return -EINVAL;
    }

    if (size > CONFIG_KCS_DEV_MAX_SIZE)
    {
        size = CONFIG_KCS_DEV_MAX_SIZE;
    }

    // 先检查设备文件是否有缓存的待取消息
    if (atomic_get(&client->has_msg))
    {
        size_t n = client->rx_len;
        if (n > size)
        {
            // 缓冲不够，返回 -ENOSPC，保持缓存不动，方便重试
            return -ENOSPC;
        }
        memcpy(ptr, client->rx_buf, n);
        // 清空"待取"消息
        client->rx_len = 0;
        atomic_set(&client->has_msg, 0);
        LOG_ERROR("Read %zu bytes data (from devfile cache)", n);
        return (ssize_t)n;
    }

    // 没有缓存，则直接请求api读取
    int ret = ipmi_read(client->dev, (uint8_t*)ptr, size);
    if (ret != 0)
    {
        LOG_ERROR("Read failed: %d", ret);
        return ret;
    }

    LOG_ERROR("Read %zu bytes data %s", size, ptr);
    return size;
}

static ssize_t kcsdev_write(struct fs_file_t* zfp, const void* ptr, size_t size)
{
    LOG_ERROR("kcsdev_write start----\n");
    struct kcs_device_info* client = zfp->filep;
    const uint8_t* write_buf = (const uint8_t*)ptr;

    if (client == NULL)
    {
        LOG_ERROR("No client context");
        return -EINVAL;
    }

    if (ptr == NULL)
    {
        LOG_ERROR("Null buffer pointer");
        return -EFAULT;
    }

    if (size > CONFIG_KCS_DEV_MAX_SIZE)
    {
        size = CONFIG_KCS_DEV_MAX_SIZE;
        LOG_ERROR("Truncated write size to %d", CONFIG_KCS_DEV_MAX_SIZE);
    }

    int ret = ipmi_write(client->dev, write_buf, size);
    if (ret != 0)
    {
        LOG_ERROR("write failed: %d", ret);
        return ret;
    }

    LOG_ERROR("KCS writing %zu bytes data %s", size, write_buf);
    return size;
}

/* IPMI 2.0 - Table 9-1, KCS Interface Status Register Bits */
#define KCS_STATUS_STATE(state) (state << 6)
#define KCS_STATUS_STATE_MASK GENMASK(7, 6)
#define KCS_STATUS_CMD_DAT BIT(3)
#define KCS_STATUS_SMS_ATN BIT(2)
#define KCS_STATUS_IBF BIT(1)
#define KCS_STATUS_OBF BIT(0)

static int kcsdev_ioctl(struct fs_file_t* zfp, unsigned long request,
                        va_list args)
{
    LOG_DBG("kcsdev_ioctl start----\n");
    struct kcs_device_info* client = zfp->filep;
    int ret = 0;

    if (!client)
        return -EINVAL;

    switch (request)
    {
        case ZFD_IOCTL_SET_LOCK:
            LOG_ERROR("ioctl ZFD_IOCTL_SET_LOCK----\n");
            return 0;
        case IPMI_BMC_IOCTL_SET_SMS_ATN:
            ipmi_update_status(client->dev, KCS_STATUS_SMS_ATN,
                               KCS_STATUS_SMS_ATN);
            break;
        case IPMI_BMC_IOCTL_CLEAR_SMS_ATN:
            ipmi_update_status(client->dev, KCS_STATUS_SMS_ATN, 0);
            break;
        case IPMI_BMC_IOCTL_FORCE_ABORT:
            ipmi_force_abort(client->dev);
            break;
        case ZFD_IOCTL_POLL_PREPARE:
        {
            LOG_DBG("ioctl ZFD_IOCTL_POLL_PREPARE----\n");
            struct zvfs_pollfd* pfd;
            struct k_poll_event** pev;
            struct k_poll_event* pev_end;
            pfd = va_arg(args, struct zvfs_pollfd*);
            pev = va_arg(args, struct k_poll_event**);
            pev_end = va_arg(args, struct k_poll_event*);
            ret = kcs_poll_prepare_ctx(client, pfd, pev, pev_end);
            break;
        }
        case ZFD_IOCTL_POLL_UPDATE:
        {
            LOG_DBG("ioctl ZFD_IOCTL_POLL_UPDATE----\n");
            struct zvfs_pollfd* pfd;
            struct k_poll_event** pev;
            pfd = va_arg(args, struct zvfs_pollfd*);
            pev = va_arg(args, struct k_poll_event**);
            ret = kcs_poll_update_ctx(client, pfd, pev);
            break;
        }
        default:
            ret = -EINVAL;
            break;
    }
    return ret;
}

static int kcsdev_mount(struct fs_mount_t* mountp)
{
    // 检查挂载点是否为空
    if (mountp->mnt_point == NULL)
    {
        LOG_ERROR("Invalid mount point\n");
        return -EINVAL;
    }

    // 检查 KCS 设备是否就绪
    for (int i = 0; i < KCS_NUM; i++)
    {
        if (!device_is_ready(kcs_devices[i].dev))
        {
            LOG_ERROR("KCS device %d not ready\n", i);
            return -ENODEV;
        }
    }

    LOG_ERROR("KCS device mounted successfully\n");
    return 0;
}
static int kcsdev_unmount(struct fs_mount_t* mountp)
{
    if (mountp->mnt_point == NULL)
    {
        LOG_ERROR("Invalid mount point\n");
        return -EINVAL;
    }

    mountp->mnt_point = NULL;
    LOG_ERROR("KCS device unmounted successfully\n");
    return 0;
}

static int kcsdev_opendir(struct fs_dir_t* dp, const char* path)
{
    if (strcmp(path, KCS_MNT_POINT) != 0)
    {
        LOG_ERROR("Invalid directory path\n");
        return -EINVAL;
    }

    dp->dirp = &kcs_index;

    LOG_ERROR("KCS device opendir successfully\n");
    return 0;
}

static int kcsdev_readdir(struct fs_dir_t* dp, struct fs_dirent* entry)
{
    int* index = dp->dirp;

    if (*index < KCS_NUM)
    {
        // 构造文件条目信息
        snprintf(entry->name, sizeof(entry->name), "%s",
                 kcs_devices[*index].label);
        entry->type = FS_DIR_ENTRY_FILE;
        entry->size = 0; // KCS 设备没有文件大小
        (*index)++;
        return 0;
    }

    // 没有更多条目
    memset(entry, 0, sizeof(struct fs_dirent));
    LOG_ERROR("No more KCS devices to list\n");
    return 0;
}

static int kcsdev_closedir(struct fs_dir_t* dp)
{
    if (dp->dirp == NULL)
    {
        LOG_ERROR("Error: NULL pointer passed to kcsdev_closedir\n");
        return -EINVAL;
    }

    kcs_index = 0;
    dp->dirp = NULL;

    LOG_ERROR("KCS device closedir successfully\n");
    return 0;
}

static int kcsdev_stat(struct fs_mount_t* mountp, const char* path,
                       struct fs_dirent* entry)
{
    // 检查路径是否为挂载点
    if (strcmp(path, KCS_MNT_POINT) == 0)
    {
        // 挂载点是目录
        entry->type = FS_DIR_ENTRY_DIR;
        entry->size = 0;
    }
    else
    {
        // 检查是否为 KCS 设备路径（使用文件名与 label 匹配）
        const char* fname = getFileName(path);
        for (int i = 0; i < KCS_NUM; i++)
        {
            if (strcmp(fname, kcs_devices[i].label) == 0)
            {
                // 是 KCS 设备（按 label 匹配）
                entry->type = FS_DIR_ENTRY_FILE;
                entry->size = 0;
                LOG_ERROR("KCS device stat successfully\n");
                return 0;
            }
        }
        LOG_ERROR("Invalid path\n");
        return -EINVAL;
    }

    LOG_ERROR("KCS device stat successfully\n");
    return 0;
}

/* File system interface */
static const struct fs_file_system_t kcsdev_fs = {
    .mount = kcsdev_mount,
    .unmount = kcsdev_unmount,
    .open = kcsdev_open,         // kcs_bmc_ipmi_open
    .close = kcsdev_close,
    .read = kcsdev_read,         // kcs_bmc_ipmi_read
    .write = kcsdev_write,       // kcs_bmc_ipmi_write
    .lseek = NULL,
    .ioctl = kcsdev_ioctl,       // kcs_bmc_ipmi_ioctl
    .opendir = kcsdev_opendir,   // fs ls访问
    .readdir = kcsdev_readdir,   // fs ls访问
    .closedir = kcsdev_closedir, // fs ls访问
    .stat = kcsdev_stat,         // fs cd访问
};

static int kcs_dev_init(void)
{
    LOG_ERROR("kcs /dev entries driver\n");

    LOG_ERROR("kcs_num = %d\n", KCS_NUM);

    // 初始化每个设备的 poll 信号与轮询工作
    for (int i = 0; i < KCS_NUM; i++)
    {
        k_poll_signal_init(&kcs_devices[i].poll_sig);
        atomic_set(&kcs_devices[i].has_msg, 0);
        kcs_devices[i].rx_len = 0;
        k_work_init_delayable(&kcs_devices[i].rx_poller, kcs_rx_poll_work);
        // 启动轮询
        k_work_schedule(&kcs_devices[i].rx_poller,
                        K_MSEC(KCS_POLL_INTERVAL_MS));
    }

    // 输出kcs设备name和label信息
    for (int i = 0; i < KCS_NUM; i++)
    {
        if (kcs_devices[i].dev->name != NULL)
        {
            LOG_ERROR("kcs_devices[%d].dev->name = %s\n", i,
                      kcs_devices[i].dev->name);
        }
        if (kcs_devices[i].label != NULL)
        {
            LOG_ERROR("kcs_devices[%d].label = %s\n", i, kcs_devices[i].label);
        }
    }

    kcsfs_mnt.mnt_point = KCS_MNT_POINT;
    LOG_ERROR("kcsfs_mnt: type: %d, mnt_point: %s, fs_data: %p\n",
              kcsfs_mnt.type, kcsfs_mnt.mnt_point, kcsfs_mnt.fs_data);

    int rc = fs_mount(&kcsfs_mnt);
    if (rc != 0)
    {
        LOG_ERROR("Error mounting as kcsfs: %d", rc);
        kcsfs_mnt.mnt_point = NULL;
        return rc;
    }

    return 0;
}

static int kcs_init(void)
{
    LOG_ERROR("kcs_init----------start\n");

    int rc = fs_register(FS_KCS, &kcsdev_fs);
    if (rc < 0)
    {
        LOG_ERROR("kcsfs register error (%d)\n", rc);
    }
    else
    {
        LOG_ERROR("kcsfs fs registered\n");
    }

    rc = kcs_dev_init();
    if (rc != 0)
    {
        LOG_ERROR("kcsfs init failed\n");
        return rc;
    }

    LOG_ERROR("kcs_init----------end\n");
    return rc;
}

// 系统初始化
SYS_INIT(kcs_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
