#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/espi.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/fs/devfs.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/posix/fcntl.h>
#include <driver/espi/espi_dev.h>
#include <zephyr/sys/fdtable.h>
#include <soc.h>
#include <stdio.h>
#include <string.h>
LOG_MODULE_REGISTER(espi_fs, LOG_LEVEL_INF);
#define DT_DRV_COMPAT linkedsemi_ls_espi_bmc

#define ESPI_DEVICE_NUM DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)

#define ESPI_VWRITE_3 3


#define ESPI_DEVICE_PROCESS(inst)                                    \
    {                                                                \
        .dev = DEVICE_DT_GET(DT_INST(inst, DT_DRV_COMPAT)),          \
        .label = DT_INST_PROP_OR(inst, label, "espi_pltrstn"),       \
        .is_open = ATOMIC_INIT(0)                                    \
    },


static struct espi_fs_device espi_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(ESPI_DEVICE_PROCESS)
};

static void espi_callback_handler(const struct device *dev, struct espi_callback *cb, struct espi_event event)
{

    struct espi_fs_device *espi_dev = CONTAINER_OF(cb, struct espi_fs_device, cb);
    if (event.evt_type == ESPI_BUS_EVENT_VWIRE_RECEIVED) {
        if (event.evt_details == ESPI_VWRITE_3) {
            uint8_t current_pltrst;
            int ret = espi_receive_vwire(dev, ESPI_VWIRE_SIGNAL_PLTRST, &current_pltrst);
            if (current_pltrst != espi_dev->last_status) {
                espi_dev->last_status = current_pltrst;
                k_poll_signal_raise(&espi_dev->signal, 1);
            }
        }
        
    }
}


static int espi_fs_open(struct fs_file_t *zfp, const char *name, fs_mode_t mode)
{
    struct espi_fs_device *espi_dev = NULL;
    const char *target_name = name;
    if (strncmp(target_name, "/dev/", 5) == 0) {
        target_name += 5; 
    }


    for (int i = 0; i < ESPI_DEVICE_NUM; i++) {
        if (strcmp(espi_devices[i].label, target_name) == 0) {
            espi_dev = &espi_devices[i];
            break;
        }
    }

    if (!espi_dev || !device_is_ready(espi_dev->dev)) {
        LOG_ERR("Device not found. Request: %s (target: %s)", name, target_name);
        return -ENODEV;
    }

    if (!atomic_cas(&espi_dev->is_open, 0, 1)) {
        LOG_ERR("Device %s already opened", name);
        return -EBUSY;
    }

    k_poll_signal_init(&espi_dev->signal);


    espi_dev->fs_flags = mode;
    zfp->filep = espi_dev;

    uint8_t initial_pltrst;
    int ret = espi_receive_vwire(espi_dev->dev, ESPI_VWIRE_SIGNAL_PLTRST, &initial_pltrst);
    if (ret == 0) {
        espi_dev->last_status = initial_pltrst;
        if (initial_pltrst == 1) {
            k_poll_signal_raise(&espi_dev->signal, 1);
        }
    } else {
        espi_dev->last_status = 0; 
    }

    espi_init_callback(&espi_dev->cb, espi_callback_handler, ESPI_BUS_EVENT_VWIRE_RECEIVED);
    if (espi_add_callback(espi_dev->dev, &espi_dev->cb) != 0) {
        LOG_ERR("Failed to add callback for %s", name);
        atomic_set(&espi_dev->is_open, 0);
        return -EIO;
    }

    return 0;
}
static int espi_fs_close(struct fs_file_t *zfp)
{
    struct espi_fs_device *espi_dev = zfp->filep;

    espi_remove_callback(espi_dev->dev, &espi_dev->cb);
    k_poll_signal_reset(&espi_dev->signal);
    atomic_set(&espi_dev->is_open, 0);
    
    return 0;
}

static ssize_t espi_fs_read(struct fs_file_t *zfp, void *dest, size_t nbytes)
{
    struct espi_fs_device *espi_dev = zfp->filep;
    int ret;
    uint8_t level = 0;

    if (nbytes != 1) {
        return -EINVAL; 
    }

    ret = espi_receive_vwire(espi_dev->dev, ESPI_VWIRE_SIGNAL_PLTRST, &level);
    
    if (ret != 0) {
        LOG_ERR("Failed to read vwire: %d", ret);
        return -EIO; 
    }

   *((uint8_t *)dest) = level ? '1' : '0';

    return 1; 
}
static int espi_fs_ioctl(struct fs_file_t *zfp, unsigned long request, va_list args)
{
    struct espi_fs_device *espi_dev = zfp->filep;

    switch (request) {
    case F_GETFL:
        return espi_dev->fs_flags;
    case F_SETFL:
        espi_dev->fs_flags = va_arg(args, int);
        return 0;

    case ZFD_IOCTL_POLL_PREPARE: {
        struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
        struct k_poll_event **pev = va_arg(args, struct k_poll_event **);
        struct k_poll_event *pev_end = va_arg(args, struct k_poll_event *);

        if (pfd->events & ZVFS_POLLIN) {
            if (*pev == pev_end) {
                return -ENOMEM;
            }

            (*pev)->obj = &espi_dev->signal;
            (*pev)->type = K_POLL_TYPE_SIGNAL;
            (*pev)->mode = K_POLL_MODE_NOTIFY_ONLY;
            (*pev)->state = K_POLL_STATE_NOT_READY;
            (*pev)++;

        }
        return 0;
    }

    case ZFD_IOCTL_POLL_UPDATE: {
        struct zvfs_pollfd *pfd = va_arg(args, struct zvfs_pollfd *);
        struct k_poll_event **pev = va_arg(args, struct k_poll_event **);

        if (pfd->events & ZVFS_POLLIN) {
            if ((*pev)->state == K_POLL_STATE_SIGNALED) {
                pfd->revents |= ZVFS_POLLIN;
                k_poll_signal_reset(&espi_dev->signal);
                (*pev)->state = K_POLL_STATE_NOT_READY;
            }
            (*pev)++;
        }
        return 0;
    }

    default:
        return -ENOTTY;
    }
}


static const struct fs_file_system_t espi_fs_ops = {
    .open = espi_fs_open,
    .close = espi_fs_close,
    .read = espi_fs_read,
    .ioctl = espi_fs_ioctl,
};


static int espi_fs_init(void)
{
    char name[32];
    int ret;

    for (int i = 0; i < ESPI_DEVICE_NUM; i++) {
        snprintf(name, sizeof(name), "/dev/%s", espi_devices[i].label);
        ret = devfs_register(name, &espi_fs_ops);
        if (ret < 0) {
            LOG_ERR("Failed to register /dev/%s: %d", name, ret);
        } else {
            LOG_INF("Registered %s", name);
        }
    }
    return 0;
}


SYS_INIT(espi_fs_init, APPLICATION, CONFIG_FILE_SYSTEM_INIT_PRIORITY);