#include <stdio.h>
#include <stdlib.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(random_dev, LOG_LEVEL_ERR); // 注册日志模块

#define RANDOM_MNT_POINT "/dev/urandom"

static struct fs_mount_t randomfs_mnt = {
	.type = FS_RANDOM,
	.mnt_point = RANDOM_MNT_POINT,
};

static int randomdev_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t mode)
{
	return 0;
}

static int randomdev_close(struct fs_file_t *zfp)
{
	return 0;
}

static ssize_t randomdev_read(struct fs_file_t *zfp, void *ptr, size_t size)
{
	sys_csrand_get(ptr, size);
	return size;
}

static int randomdev_mount(struct fs_mount_t *mountp)
{
	return 0;
}
static int randomdev_unmount(struct fs_mount_t *mountp)
{
	return 0;
}

static int randomdev_stat(struct fs_mount_t *mountp,
					   const char *path, struct fs_dirent *entry)
{
	// 检查路径是否为挂载点
	if (strcmp(path, RANDOM_MNT_POINT) != 0)
	{
		return -ENOENT;
	}

	// 挂载点是目录
	entry->type = FS_DIR_ENTRY_FILE;
	entry->size = 0;

	return 0;
}

/* File system interface */
static const struct fs_file_system_t randomdev_fs = {
	.mount = randomdev_mount,
	.unmount = randomdev_unmount,
	.open = randomdev_open,
	.close = randomdev_close,
	.read = randomdev_read,   // 从 I2C 设备中读取数据
	.write = NULL,
	.lseek = NULL,
	.ioctl = NULL,		 // 增加ioctl
	.opendir = NULL,	 // fs ls访问
	.readdir = NULL,	 // fs ls访问
	.closedir = NULL, 	// fs ls访问
	.stat = randomdev_stat,		 // fs cd访问
};

static int random_init(void)
{
	int ret;

	/* 注册文件系统 */
 	ret = fs_register(FS_RANDOM, &randomdev_fs);
    if (ret < 0) {
        printk("Failed to register random FS: %d", ret);
        return ret;
    }

	/* 挂载文件系统 */
    ret = fs_mount(&randomfs_mnt);
    if (ret != 0) {
        printk("Failed to mount random FS: %d", ret);
        return ret;
    }

	return 0;
}

// 系统初始化
SYS_INIT(random_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
