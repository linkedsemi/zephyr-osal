/*
 * Copyright (c) 2025 OpenBMC Shim Layer
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/posix/errno.h>
#include <zephyr/spinlock.h>
#include <driver/ipmb/ipmb_dev.h>
#include <zephyr/drivers/i2c.h>
LOG_MODULE_REGISTER(ipmb_dev, LOG_LEVEL_INF);

#define DT_DRV_COMPAT linkedsemi_ls_ipmb_dev

/* 获取设备树中状态为 okay 的实例数量 */
#define IPMB_NUM  1  // DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

/* 定义最大支持的设备数量（防止内存越界或过多实例） */
#define MAX_IPMB_DEVICES 14

BUILD_ASSERT(IPMB_NUM <= MAX_IPMB_DEVICES, "Exceeded maximum IPMB device count");

#ifndef CONFIG_IPMB_RX_BUF_SIZE
#define CONFIG_IPMB_RX_BUF_SIZE 512
#endif

/* =========================================================================
 * 设备定义宏 
 * ========================================================================= */

/* * 注意：这里假设 IPMB 驱动本身作为一个节点存在，且引用了底层的 i2c_bus。
 * 如果 linkedsemi_ls_ipmb_dev 就是驱动本身，那么 .ipmb_dev 应该获取的是
 * 该节点本身的设备指针，即 DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT))。
 * * 下面的宏实现了这一逻辑：
 */
#define IPMB_DEVICE_PROCESS(inst)                                    \
    {                                                                \
        /* 获取该实例对应的 device 结构体 */                          \
        .ipmb_dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)),  \
        .label = DT_INST_PROP(inst, label),                          \
        .ref_count = ATOMIC_INIT(0),                                 \
        .rx_len = 0,                                                 \
        /* 自旋锁无需显式初始化宏，默认零值即可 */                      \
    },

    
/* 设备上下文结构体 */
struct ipmb_device_info {
    const struct device *ipmb_dev; /* Zephyr IPMB驱动设备句柄 */
    const char *label;             /* 设备标签 */
    atomic_t ref_count;            /* 引用计数 */
    struct k_poll_signal poll_sig; /* Poll信号 */
    
    /* IPMB接收数据缓冲区 */
    uint8_t rx_buf[CONFIG_IPMB_RX_BUF_SIZE];
    size_t rx_len;                 /* 接收数据长度 */
    
    /* 接收缓冲区自旋锁 */
    struct k_spinlock rx_lock;     
};


static struct ipmb_device_info ipmb_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(IPMB_DEVICE_PROCESS)
};

/* 回调上下文 */
struct ipmb_callback_ctx {
    struct ipmb_device_info *dev;
};

/* 静态分配回调上下文数组 */
static struct ipmb_callback_ctx ipmb_cb_ctx[MAX_IPMB_DEVICES];

static int ipmb_index = 0;

/* --- Registration --- */
static struct fs_mount_t ipmbfs_mnt = {
    .type = FS_IPMB,
    .fs_data = &ipmb_devices,
};

/* =========================================================================
 * Zephyr IPMB驱动回调函数
 * ========================================================================= */
 #if 0
static void ipmb_receive_callback(const struct device *dev, void *user_data,
                                  const uint8_t *data, size_t len)
{
    struct ipmb_callback_ctx *ctx = (struct ipmb_callback_ctx *)user_data;
    struct ipmb_device_info *ipmb_dev = ctx->dev;
    k_spinlock_key_t key;

    if (!ipmb_dev || len == 0 || len > CONFIG_IPMB_RX_BUF_SIZE) {
        return;
    }

    key = k_spin_lock(&ipmb_dev->rx_lock);
    
    memcpy(ipmb_dev->rx_buf, data, len);
    ipmb_dev->rx_len = len;
    
    k_spin_unlock(&ipmb_dev->rx_lock, key);

    k_poll_signal_raise(&ipmb_dev->poll_sig, 1);
}
#endif
static const char *get_filename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static int ipmbdev_open(struct fs_file_t *zfp, const char *path, fs_mode_t mode)
{
    const char *name = get_filename(path);
    struct ipmb_device_info *dev = NULL;
    //int ret = 0;

    for (int i = 0; i < IPMB_NUM; i++) {
        if (strcmp(ipmb_devices[i].label, name) == 0) {
            dev = &ipmb_devices[i];
            /* 绑定回调上下文到对应的静态数组槽位 */
            ipmb_cb_ctx[i].dev = dev;
            break;
        }
    }

    if (!dev) {
        LOG_ERROR("IPMB_DEV: IPMB device %s not found", name);
        return -ENODEV;
    }

    atomic_val_t old_count = atomic_inc(&dev->ref_count);

    if (old_count == 0) {
        /* 检查硬件是否就绪 */
        if (!device_is_ready(dev->ipmb_dev)) {
            LOG_ERROR("IPMB_DEV: IPMB hardware for %s not ready", name);
            atomic_dec(&dev->ref_count);
            return -ENODEV;
        }
        k_poll_signal_init(&dev->poll_sig);

        /* 注册回调 */
        /* 注意：这里使用 dev - ipmb_devices 来计算索引 
        ret = ipmb_register_callback(dev->ipmb_dev, ipmb_receive_callback,
                                     &ipmb_cb_ctx[dev - ipmb_devices]);
        if (ret < 0) {
            LOG_ERROR("IPMB_DEV: IPMB %s failed to register callback: %d", name, ret);
            atomic_dec(&dev->ref_count);
            return ret;
        }*/

        LOG_INF("IPMB_DEV: IPMB Device %s initialized", name);
    }

    zfp->filep = dev;
    return 0;
}

static int ipmbdev_close(struct fs_file_t *zfp)
{
    struct ipmb_device_info *dev = zfp->filep;
    if (!dev) {
        return 0;
    }

    atomic_val_t old_count = atomic_dec(&dev->ref_count);

    if (old_count == 1) {
        /*ipmb_unregister_callback(dev->ipmb_dev, ipmb_receive_callback,
                                 &ipmb_cb_ctx[dev - ipmb_devices]);*/
        
        k_spinlock_key_t key = k_spin_lock(&dev->rx_lock);
        dev->rx_len = 0;
        k_spin_unlock(&dev->rx_lock, key);
        
        LOG_INF("IPMB_DEV: IPMB Device %s closed", dev->label);
    }

    zfp->filep = NULL;
    return 0;
}

static ssize_t ipmbdev_read(struct fs_file_t *zfp, void *buffer, size_t size)
{
    struct ipmb_device_info *dev = zfp->filep;
    k_spinlock_key_t key;
    size_t read_len = 0;

    if (!dev || size == 0) {
        errno = EINVAL;
        return -1;
    }

    key = k_spin_lock(&dev->rx_lock);
    
    if (dev->rx_len > 0) {
        read_len = MIN(size, dev->rx_len);
        memcpy(buffer, dev->rx_buf, read_len);
        dev->rx_len = 0; 
    }
    
    k_spin_unlock(&dev->rx_lock, key);

    if (read_len == 0) {
        errno = EAGAIN;
        return -1;
    }

    return read_len;
    /*struct ipmb_device_info *dev = zfp->filep;
    第二种读buffer的方法
    注意：如果有多个 fd 同时读同一个 RingBuf，数据会被竞态抢走。
     * 但在 IPMB 场景下，通常是一个 fd 读 (Slave Path)，另一个 fd 写 (Master Path)。
     * 即使开了两个 Reader，ring_buf_get 也是线程安全的(原子操作)，不会崩，只会分流数据。
     
    int readsize = ring_buf_get(&dev->rx_ringbuf, buffer, size);

    if (readsize == 0) {
        errno = EAGAIN;
        return -1;
    }
    
    return readsize;*/
}

static ssize_t ipmbdev_write(struct fs_file_t *zfp, const void *buffer, size_t size)
{
    struct ipmb_device_info *dev = zfp->filep;
    const uint8_t *data = (const uint8_t *)buffer;
    int ret;

    if (!dev || size < 1) {
        errno = EINVAL;
        return -1;
    }

    /*ret = ipmb_write(dev->ipmb_dev, data, size);
    if (ret == 0) {
        return size;
    } else {
        errno = EIO;
        return -1;
    }*/

    uint8_t target_addr = data[0] >> 1;
    
    struct i2c_msg msgs[1];
    msgs[0].buf = (uint8_t *)data + 1; 
    msgs[0].len = size - 1;
    msgs[0].flags = I2C_MSG_WRITE | I2C_MSG_STOP;

    /* 发送不受引用计数影响，只要 handle 还在就能发 */
    ret = i2c_transfer(dev->ipmb_dev, &msgs[0], 1, target_addr);

    if (ret == 0) {
        return size; 
    } else {
        errno = EIO;
        return -1;
    }

}

/* --- Poll Helpers --- */
static int ipmb_poll_prepare_ctx(struct ipmb_device_info *dev,
                                 struct zvfs_pollfd *pfd,
                                 struct k_poll_event **pev,
                                 struct k_poll_event *pev_end)
{
    if (pfd->events & ZVFS_POLLIN) {
        if (*pev == pev_end) {
            return -ENOMEM;
        }
        (*pev)->obj = &dev->poll_sig;
        (*pev)->type = K_POLL_TYPE_SIGNAL;
        (*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
        (*pev)->state = K_POLL_STATE_NOT_READY;
        (*pev)++;
    }
    return 0;
}

static int ipmb_poll_update_ctx(struct ipmb_device_info *dev,
                                struct zvfs_pollfd *pfd,
                                struct k_poll_event **pev)
{
    if ((pfd->events & ZVFS_POLLIN) && ((*pev)->signal->result == 1)) {
        pfd->revents |= ZVFS_POLLIN;
        k_poll_signal_reset(&dev->poll_sig);
        (*pev)->state = K_POLL_STATE_NOT_READY;
        (*pev)++;
    }
    return 0;
}

static int ipmbdev_ioctl(struct fs_file_t *zfp, unsigned long cmd, va_list args)
{
    struct ipmb_device_info *dev = zfp->filep;

    switch (cmd) {
    case F_GETFL:
        return O_RDWR;

    case F_SETFL:
        return 0;

    case ZFD_IOCTL_POLL_PREPARE: {
        struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
        struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
        struct k_poll_event *pev_end = va_arg(args, struct k_poll_event *);
        return ipmb_poll_prepare_ctx(dev, pfd, pev, pev_end);
    }

    case ZFD_IOCTL_POLL_UPDATE: {
        struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
        struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
        return ipmb_poll_update_ctx(dev, pfd, pev);
    }

    default:
        return -ENOTTY;
    }
}

/* --- Directory Operations --- */
static int ipmbdev_opendir(struct fs_dir_t *dp, const char *path)
{
    if (strcmp(path, IPMB_MNT_POINT) != 0) {
        return -ENOENT;
    }
    dp->dirp = &ipmb_index;
    return 0;
}

static int ipmbdev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry)
{
    int *index = dp->dirp;
    if (*index >= IPMB_NUM) {
        memset(entry, 0, sizeof(*entry));
        return 0;
    }
    snprintf(entry->name, sizeof(entry->name), "%s", ipmb_devices[*index].label);
    entry->type = FS_DIR_ENTRY_FILE;
    entry->size = 0;
    (*index)++;
    return 0;
}

static int ipmbdev_closedir(struct fs_dir_t *dp)
{
    ipmb_index = 0;
    dp->dirp = NULL;
    return 0;
}

static int ipmbdev_stat(struct fs_mount_t *mountp,
                        const char *path, struct fs_dirent *entry)
{
    if (strcmp(path, IPMB_MNT_POINT) == 0) {
        entry->type = FS_DIR_ENTRY_DIR;
    } else {
        entry->type = FS_DIR_ENTRY_FILE;
    }
    entry->size = 0;
    return 0;
}

/* IPMB 是流设备，实际上不支持 seek。
 * 但为 ipmbbridged，使用了lseek我们需要假装 seek 成功 (返回 0)。
 */
static int ipmbdev_lseek(struct fs_file_t *zfp, off_t offset, int whence)
{
    return 0; 
}

static int ipmbdev_mount(struct fs_mount_t *mountp)
{
    /* 1. 检查挂载点路径是否合法 */
    if (mountp->mnt_point == NULL) {
        LOG_ERROR("IPMB_DEV: Invalid mount point");
        return -EINVAL;
    }

    /* 2. 遍历所有注册的 IPMB 设备，检查底层硬件是否就绪 */
    for (int i = 0; i < IPMB_NUM; i++) {
        const struct device *hw_dev = ipmb_devices[i].ipmb_dev;
        
        if (!device_is_ready(hw_dev)) {
            LOG_ERROR("IPMB_DEV: IPMB Hardware device '%s' is not ready!", 
                      ipmb_devices[i].label);
            /* 只要有一个设备没准备好，就让整个挂载失败，
             * 这样系统日志里会立刻看到错误。
             */
            return -ENODEV;
        }
    }

    LOG_INF("IPMB_DEV: IPMB FS mounted successfully at %s", mountp->mnt_point);
    return 0;
}

static int ipmbdev_unmount(struct fs_mount_t *mountp)
{
    if (mountp->mnt_point == NULL) {
        return -EINVAL;
    }

    mountp->mnt_point = NULL;
    
    LOG_INF("IPMB_DEV: IPMB FS unmounted");
    return 0;
}

static off_t ipmbdev_tell(struct fs_file_t *zfp)
{
    return 0;
}

static const struct fs_file_system_t ipmb_fs = {
    .mount = ipmbdev_mount,
    .unmount = ipmbdev_unmount,
    .open = ipmbdev_open,
    .close = ipmbdev_close,
    .read = ipmbdev_read,
    .write = ipmbdev_write,
    .lseek = ipmbdev_lseek,
    .tell = ipmbdev_tell,
    .ioctl = ipmbdev_ioctl,
    .opendir = ipmbdev_opendir,
    .readdir = ipmbdev_readdir,
    .closedir = ipmbdev_closedir,
    .stat = ipmbdev_stat,
};

static int ipmb_dev_init(void)
{
    LOG_DBG("IPMB /dev entries driver initialization");

    LOG_DBG("Found IPMB devices count = %d", IPMB_NUM);

    /* 遍历并打印所有 IPMB 设备的信息 (调试用) */
    for (int i = 0; i < IPMB_NUM; i++) {
        /* 访问 ipmb_devices 数组，注意成员名称是 ipmb_dev 而不是 dev */
        if (ipmb_devices[i].ipmb_dev != NULL && ipmb_devices[i].ipmb_dev->name != NULL) {
            LOG_DBG("ipmb_devices[%d] Hardware Name = %s", i, ipmb_devices[i].ipmb_dev->name);
        }
        
        if (ipmb_devices[i].label != NULL) {
            LOG_DBG("ipmb_devices[%d] Label (Filename) = %s", i, ipmb_devices[i].label);
        }
    }

    /* 确保挂载点设置正确 */
    ipmbfs_mnt.mnt_point = IPMB_MNT_POINT;

    LOG_DBG("Mounting IPMB FS: type=%d, point=%s, data=%p", 
            ipmbfs_mnt.type, ipmbfs_mnt.mnt_point, ipmbfs_mnt.fs_data);

    /* 执行挂载 */
    int rc = fs_mount(&ipmbfs_mnt);
    if (rc != 0) {
        LOG_ERROR("Error mounting IPMB FS: %d", rc);
        /* 挂载失败时清空挂载点，防止残留状态 */
        ipmbfs_mnt.mnt_point = NULL;
        return rc;
    }

    LOG_INF("IPMB FS mounted successfully at %s", IPMB_MNT_POINT);
    return 0;
}

int ipmb_fs_init(void)
{
    int ret;

    if (IPMB_NUM == 0) {
        LOG_WRN("IPMB_DEV: No IPMB devices in Device Tree");
        return 0;
    }

    ret = fs_register(FS_IPMB, &ipmb_fs);
    if (ret < 0) {
        LOG_ERROR("IPMB_DEV: Failed to register FS_IPMB: %d", ret);
        return ret;
    }

    ipmb_dev_init();

    return 0;
}

SYS_INIT(ipmb_fs_init, POST_KERNEL, 99);