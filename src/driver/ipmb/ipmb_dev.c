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
#include <zephyr/drivers/ipmb.h> 
#include <driver/ipmb/ipmb_dev.h>
#include <zephyr/drivers/i2c.h>

LOG_MODULE_REGISTER(ipmb_dev, LOG_LEVEL_INF);

#define DT_DRV_COMPAT linkedsemi_ls_ipmb_target

#define IPMB_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)
#define MAX_IPMB_DEVICES 1

BUILD_ASSERT(IPMB_NUM <= MAX_IPMB_DEVICES, "Exceeded maximum IPMB device count");

#define RX_BUF_SIZE     240  /* 固定 Buffer 大小 */
#define RX_BUF_COUNT    5    /* Buffer 个数 */

/* * 静态内存池
 * 存放实际的数据。[设备数][Buffer数][Buffer大小] 
 */
static uint8_t ipmb_raw_buffers[MAX_IPMB_DEVICES][RX_BUF_COUNT][RX_BUF_SIZE];

struct ipmb_rx_packet 
{
    void *fifo_reserved;    /* Zephyr k_fifo 需要的保留字段 */
    uint8_t *data_ptr;      /* 指向 ipmb_raw_buffers 中的某一块 */
    uint16_t len;           /* 当前包的有效数据长度 */
    uint32_t packet_id;     /* debug */
};

/* -------------------- 设备定义 -------------------- */

/* 在初始化宏中记录实例索引 (inst)，用于查找内存池 */
#define IPMB_DEVICE_PROCESS(inst)                                    \
    {                                                                \
        /* 获取该实例对应的 device 结构体 */                          \
        .ipmb_dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)),  \
        .label = DT_INST_PROP(inst, label),                          \
        .ref_count = ATOMIC_INIT(0),                                 \
        .dev_idx = inst,                                                 \
        /* 自旋锁无需显式初始化宏，默认零值即可 */                      \
    },

 
struct ipmb_device_info 
{
    const struct device *ipmb_dev;
    const char *label;            
    atomic_t ref_count;           
    struct k_poll_signal poll_sig;
    
    struct k_fifo empty_queue; /* 存放空闲的 ipmb_rx_packet */
    struct k_fifo data_queue;  /* 存放已接收数据的 ipmb_rx_packet */
    
    /* Packet 管理对象池 */
    struct ipmb_rx_packet packet_pool[RX_BUF_COUNT];

    /* 当前正在读取的包 */
    struct ipmb_rx_packet *current_read_packet;
    size_t current_read_offset;

    /* 设备索引，用于定位 ipmb_raw_buffers */
    int dev_idx; 
};


static struct ipmb_device_info ipmb_devices[] = 
{
    DT_INST_FOREACH_STATUS_OKAY(IPMB_DEVICE_PROCESS)
};

static int ipmb_index = 0;

static struct fs_mount_t ipmbfs_mnt = 
{
    .type = FS_IPMB,
    .fs_data = &ipmb_devices,
};

/* global Packet ID counter */
static uint32_t global_packet_id = 0;

static void ipmb_queue_init(struct ipmb_device_info *dev)
{
    k_fifo_init(&dev->empty_queue);
    k_fifo_init(&dev->data_queue);
    dev->current_read_packet = NULL;
    dev->current_read_offset = 0;

    /* *初始化指针绑定
     * 将 packet 指向静态内存池，并放入空闲队列
     */
    for (int i = 0; i < RX_BUF_COUNT; i++) {
        /* 指向该设备对应的第 i 块内存 */
        dev->packet_pool[i].data_ptr = ipmb_raw_buffers[dev->dev_idx][i];
        dev->packet_pool[i].packet_id = global_packet_id++;  /* Assign a unique ID */
        
        /* 放入空闲队列 */
        k_fifo_put(&dev->empty_queue, &dev->packet_pool[i]);
    }
}

/* =========================================================================
 * IPMB 接收回调 (中断上下文) - 零拷贝实现
 * ========================================================================= */
static void ipmb_receive_callback(void *param, uint8_t **ptr)
{
    /* disable reentry for debug*/
    static bool in_callback = false;
    if (in_callback) {
        LOG_WRN("RX_CB: Reentrant call detected!");
        return;
    }
    in_callback = true;

    struct ipmb_device_info *dev = (struct ipmb_device_info *)param;
    struct ipmb_rx_packet *packet;
    uint8_t *filled_buf = *ptr; /* 获取底层驱动当前的 buffer 地址 */
    uint16_t data_len = 0;

    if (!dev || !filled_buf) 
    {
        in_callback = false;
        return;
    }

    /* 1. 解析长度 (假设第一个字节是长度) */
    data_len = filled_buf[0]; 

    LOG_DBG("RX_CB: filled_buf=%p, data_len=%d", filled_buf, data_len);

    if (data_len == 0 || data_len > RX_BUF_SIZE) 
    {
        /* 数据异常，不交换，让底层继续用这个 buffer */
        LOG_ERROR("IPMB_DEV: IPMB RX Drop: data_len abnormal (%d)", data_len);
        in_callback = false;
        return;
    }

    /* 2. 从空闲队列获取一个新的 Packet (里面包含一个干净的 buffer 指针) */
    packet = k_fifo_get(&dev->empty_queue, K_NO_WAIT);

    if (!packet) 
    {
        /* 队列满，丢包。
         * 不做任何操作，底层会复用 filled_buf，旧数据会被覆盖
         */
        LOG_ERROR("IPMB_DEV: IPMB RX Drop: No buffers");
        in_callback = false;
        return; 
    }

    LOG_DBG("RX_CB: Swapping buf %p -> %p (pkt_id=%u)", 
            filled_buf, packet->data_ptr, packet->packet_id);

    /* * 零拷贝交换 (Pointer Swap) 
     */
    /* A. 把干净的 Buffer 给底层驱动，供其下次接收使用 */
    *ptr = packet->data_ptr;
    /* B. 把底层填满数据的 Buffer 挂到的 Packet 上 */
    packet->data_ptr = filled_buf;
    /* 3. 更新长度并放入数据队列 */
    packet->len = data_len + 1; // +1 包含长度字节本身
    k_fifo_put(&dev->data_queue, packet);
    /* 4. 唤醒上层 */
    k_poll_signal_raise(&dev->poll_sig, 1);
    
    LOG_DBG("RX_CB: Packet queued (id=%u, len=%u)", packet->packet_id, packet->len);

    in_callback = false;
}

static const char *get_filename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}


static int ipmbdev_open(struct fs_file_t *zfp, const char *path, fs_mode_t mode)
{
    const char *name = get_filename(path);
    struct ipmb_device_info *dev = NULL;
    int ret;

    for (int i = 0; i < IPMB_NUM; i++) 
    {
        if (strcmp(ipmb_devices[i].label, name) == 0) 
        {
            dev = &ipmb_devices[i];
            break;
        }
    }

    if (!dev)
    {
        return -ENODEV;
    } 

    atomic_val_t old_count = atomic_inc(&dev->ref_count);

    if (old_count == 0) 
    {
        if (!device_is_ready(dev->ipmb_dev)) 
        {
            atomic_dec(&dev->ref_count);
            return -ENODEV;
        }
        /* 初始化队列和信号 */
        ipmb_queue_init(dev);
        k_poll_signal_init(&dev->poll_sig);
   
        ret = ipmb_register(dev->ipmb_dev);
        if (ret < 0) 
        {
            atomic_dec(&dev->ref_count);
            return ret;
        }

        /* 1. 从空闲队列中【取出】一个 Packet */
        struct ipmb_rx_packet *init_packet = k_fifo_get(&dev->empty_queue, K_NO_WAIT);
        if (!init_packet) 
        {
            /* 理论上刚 init 完肯定有，除非内存极度匮乏 */
            LOG_ERROR("IPMB_DEV:No buffer for initial setup");
            ipmb_unregister(dev->ipmb_dev);
            atomic_dec(&dev->ref_count);
            return -ENOMEM;
        }
        /* 2. 拿到 Buffer 指针，移交给底层硬件 */
        /* 这个 init_packet 结构体本身就被“消耗”掉了，
         * 它代表了当前正在硬件手中的那个 buffer。
         * 当数据回来时，我们会从队列拿一个新的 packet。
         */
        uint8_t *initial_buf = init_packet->data_ptr; 
        ret = ipmb_set_rx_callback(dev->ipmb_dev, 
                                   ipmb_receive_callback, 
                                   dev,          
                                   initial_buf,  
                                   RX_BUF_SIZE);
        
        if (ret < 0) 
        {
            ipmb_unregister(dev->ipmb_dev);
            atomic_dec(&dev->ref_count);
            return ret;
        }
        LOG_INF("IPMB_DEV: IPMB Device %s opened", name);
    }

    zfp->filep = dev;
    return 0;
}

/* =========================================================================
 * Close
 * ========================================================================= */
static int ipmbdev_close(struct fs_file_t *zfp)
{
    struct ipmb_device_info *dev = zfp->filep;
    if (!dev) 
    {
        LOG_INF("IPMB_DEV: no Device need closed");
        return 0;
    }

    atomic_val_t old_count = atomic_dec(&dev->ref_count);
  
    if (old_count == 1) 
    {
        ipmb_unregister(dev->ipmb_dev);
        LOG_INF("IPMB_DEV: IPMB Device %s closed", dev->label);
    }

    zfp->filep = NULL;
    return 0;
}

/* =========================================================================
 * Read: 从数据队列取数据
 * ========================================================================= */
static ssize_t ipmbdev_read(struct fs_file_t *zfp, void *buffer, size_t size)
{
    struct ipmb_device_info *dev = zfp->filep;
    size_t copy_len;

    if (!dev || size == 0)
    {
        return -EINVAL;
    } 
    
    /* 1. 检查是否有上次没读完的 packet */
    if (dev->current_read_packet == NULL) 
    {
        dev->current_read_packet = k_fifo_get(&dev->data_queue, K_NO_WAIT);
        dev->current_read_offset = 0;
        
        if (dev->current_read_packet) {
            LOG_DBG("READ: Got new packet (id=%u, len=%u, queue_empty=%d)", 
                    dev->current_read_packet->packet_id,
                    dev->current_read_packet->len,
                    k_fifo_is_empty(&dev->data_queue));
        }
    }

    if (dev->current_read_packet == NULL) 
    {
        errno = EAGAIN;
        return -1;
    }

    struct ipmb_rx_packet *pkt = dev->current_read_packet;
    size_t remain_len = pkt->len - dev->current_read_offset;
    
    copy_len = MIN(size, remain_len);
    
    /* *使用 data_ptr 指针进行拷贝 
     */
    memcpy(buffer, pkt->data_ptr + dev->current_read_offset, copy_len);
    
    LOG_DBG("READ: Copied %zu bytes (offset=%zu/%u, pkt_id=%u)", 
            copy_len, dev->current_read_offset, pkt->len, pkt->packet_id);
    if (copy_len > 0) {
        LOG_HEXDUMP_DBG(buffer, copy_len, "ME RX Data:");
    }
    dev->current_read_offset += copy_len;

    if (dev->current_read_offset >= pkt->len) 
    {
        LOG_DBG("READ: Completed packet (id=%u), returning to empty_queue", pkt->packet_id);
        /* 放回空闲队列（归还 packet 结构体及其携带的 buffer） */
        k_fifo_put(&dev->empty_queue, pkt);
        
        dev->current_read_packet = NULL;
        dev->current_read_offset = 0;
        
        // if (!k_fifo_is_empty(&dev->data_queue)) 
        // {
        //      k_poll_signal_raise(&dev->poll_sig, 1);
        // } 
        // else 
        // {
        //      k_poll_signal_reset(&dev->poll_sig);
        // }
    }

    return copy_len;
}


/* =========================================================================
 * Write:直接调用ipmb_write接口
 * ========================================================================= */
static ssize_t ipmbdev_write(struct fs_file_t *zfp, const void *buffer, size_t size)
{
    struct ipmb_device_info *dev = zfp->filep;
    int ret;

    if (!dev || size < 1) 
    {
        return -EINVAL;
    }

    LOG_DBG("IPMB_DEV: Writing %zu bytes to IPMB:", size);
    LOG_HEXDUMP_DBG(buffer, size, "TX Data");

    ret = ipmb_write(dev->ipmb_dev, (uint8_t *)buffer, size);

    if (ret == 0) 
    {
        return size; 
    } 
    else 
    {
        LOG_ERROR("IPMB_DEV:ipmb_write err %d", ret);
        errno = EIO;
        return -1;
    }
}
#if 0
/* =========================================================================
 * Write: 软件模拟回环版本 (Software Loopback)
 * 用于无硬件环境下验证 Buffer 交换、FIFO 队列和 Poll 机制.
 * 此函数仅用于测试，测试完成请使用上面的真实写函数。
 * ========================================================================= */
static ssize_t ipmbdev_write(struct fs_file_t *zfp, const void *buffer, size_t size)
{
    struct ipmb_device_info *dev = zfp->filep;
    LOG_ERROR("IPMB_DEV:loopback write");
    if (!dev || size < 1) {
        return -EINVAL;
    }
    static uint8_t mock_hw_buffer[RX_BUF_SIZE]; 

    if (size > (RX_BUF_SIZE - 1)) {
        LOG_WRN("Sim write: truncated data");
        size = RX_BUF_SIZE - 1;
    }

    mock_hw_buffer[0] = (uint8_t)size;
    memcpy(&mock_hw_buffer[1], buffer, size);
    uint8_t *sim_ptr = mock_hw_buffer;

    LOG_ERROR("Simulating RX interrupt via Loopback...");
    ipmb_receive_callback(dev, &sim_ptr);
    return size;
}
#endif

static int ipmb_poll_prepare_ctx(struct ipmb_device_info *dev,
                                 struct zvfs_pollfd *pfd,
                                 struct k_poll_event **pev,
                                 struct k_poll_event *pev_end)
{
    if (pfd->events & ZVFS_POLLIN) 
    {
        if (*pev == pev_end) 
        {
            return -ENOMEM;
        }
        (*pev)->obj = &dev->poll_sig;
        (*pev)->type = K_POLL_TYPE_SIGNAL;
        (*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
        (*pev)->state = K_POLL_STATE_NOT_READY;
        (*pev)++;

        k_poll_signal_reset(&dev->poll_sig);
    }
    return 0;
}

static int ipmb_poll_update_ctx(struct ipmb_device_info *dev,
                                struct zvfs_pollfd *pfd,
                                struct k_poll_event **pev)
{
    bool has_data = !k_fifo_is_empty(&dev->data_queue);
    
    if ((pfd->events & ZVFS_POLLIN) && has_data) {
        pfd->revents |= ZVFS_POLLIN;
    }
    LOG_DBG("POLL_UPDATE: data_queue_empty=%d", !has_data);
    return 0;
}

static int ipmbdev_ioctl(struct fs_file_t *zfp, unsigned long cmd, va_list args)
{
    struct ipmb_device_info *dev = zfp->filep;
	// LOG_DBG("ipmbdev ioctl: 0x%lx", cmd);

    switch (cmd)
     {
        case F_GETFL: return O_RDWR;
        case F_SETFL: return 0;
        case ZFD_IOCTL_POLL_PREPARE: 
        {
            struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
            struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
            struct k_poll_event *pev_end = va_arg(args, struct k_poll_event *);
            return ipmb_poll_prepare_ctx(dev, pfd, pev, pev_end);
        }
        case ZFD_IOCTL_POLL_UPDATE: 
        {
            struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
            struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
            return ipmb_poll_update_ctx(dev, pfd, pev);
        }
        case ZFD_IOCTL_SET_LOCK:
		    return 0;
        default: 
            return -ENOTTY;
    }
}

static int ipmbdev_opendir(struct fs_dir_t *dp, const char *path) 
{
    if (strcmp(path, IPMB_MNT_POINT) != 0)
    {
        LOG_ERROR("IPMB_DEV:path err %s", path);
        return -ENOENT;
    } 
    dp->dirp = &ipmb_index;
    return 0;
}

static int ipmbdev_readdir(struct fs_dir_t *dp, struct fs_dirent *entry) 
{
    int *index = dp->dirp;
    if (*index >= IPMB_NUM) 
    { 
        memset(entry, 0, sizeof(*entry));
        return 0; 
    }
    snprintf(entry->name, sizeof(entry->name), "%s", ipmb_devices[*index].label);
    entry->type = FS_DIR_ENTRY_FILE; entry->size = 0;
    (*index)++;
    return 0;
}

static int ipmbdev_closedir(struct fs_dir_t *dp) 
{ 
    ipmb_index = 0;
    dp->dirp = NULL;
    return 0; 
}

static int ipmbdev_stat(struct fs_mount_t *mountp, const char *path, struct fs_dirent *entry) 
{
    if (strcmp(path, IPMB_MNT_POINT) == 0) 
    {
        entry->type = FS_DIR_ENTRY_DIR;
    }
    else 
    {
        entry->type = FS_DIR_ENTRY_FILE;
    }
    entry->size = 0;
    return 0;
}

/* =========================================================================
* IPMB 是流设备，物理上不支持 Seek。
* 为了兼容性ipmbbridged应用，防止返回 -ENOTSUP 导致应用崩溃,实现此接口，
* 但是此接口不做任何操作。
* ========================================================================= */
static int ipmbdev_lseek(struct fs_file_t *zfp, off_t offset, int whence) 
{ 
    return 0; 
}

static off_t ipmbdev_tell(struct fs_file_t *zfp) 
{ 
    return 0; 
}

static int ipmbdev_mount(struct fs_mount_t *mountp)
{
    if (mountp->mnt_point == NULL) 
    {
        return -EINVAL;
    }
    LOG_INF("IPMB_DEV: IPMB FS mounted at %s", mountp->mnt_point);
    return 0;
}

static int ipmbdev_unmount(struct fs_mount_t *mountp)
{
    if (mountp->mnt_point == NULL)
    { 
        return -EINVAL;
    }
    mountp->mnt_point = NULL;
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

SYS_INIT(ipmb_fs_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);