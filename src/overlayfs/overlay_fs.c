/******************************************************************************
 * Copyright 2024 Alibaba Group Corporation
 *
 * This software and the related documents are Alibaba copyrighted materials,
 * and your use of them is governed by an express license which must be granted
 * to you pursuant to a fully executed Agreement with Alibaba ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Alibaba's prior written permission.
 ******************************************************************************/

#include "overlay_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#define MAX_BUF_LEN 128

static char* overlayfs_mnt_point = NULL;
static char* rofs_mnt_point = NULL;
static char* rwfs_mnt_point = NULL;

extern overlay_dir_t* head;
overlay_dir_t* readed_dir;

const struct fs_dirent dummy_entry = {
    .type = FS_DIR_ENTRY_DIR, .name = {0}, .size = 0};
const overlay_dir_t dummy_dir = {.next = NULL, .entry = dummy_entry};

LOG_MODULE_REGISTER(overlay_fs);

static int resolve_path(const char* name, char* rw_path, char* ro_path,
                        size_t path_size)
{
    if (!name || name[0] != '/')
    {
        return -EINVAL;
    }

    size_t prefix_len = strlen(overlayfs_mnt_point);

    if (strncmp(name, overlayfs_mnt_point, prefix_len) == 0)
    {
        const char* subpath = name + prefix_len;

        int len = snprintf(rw_path, path_size, "%s%s", rwfs_mnt_point, subpath);
        if (len < 0 || len >= path_size)
        {
            return -ENOMEM;
        }

        len = snprintf(ro_path, path_size, "%s%s", rofs_mnt_point, subpath);
        if (len < 0 || len >= path_size)
        {
            return -ENOMEM;
        }

        return 0;
    }

    return -EINVAL;
}

static int check_file_exists(const char* path)
{
    struct fs_dirent entry;
    return fs_stat(path, &entry);
}

static inline void fs_lock(struct overlay_mount_data* fs)
{
    k_mutex_lock(&fs->mutex, K_FOREVER);
}

static inline void fs_unlock(struct overlay_mount_data* fs)
{
    k_mutex_unlock(&fs->mutex);
}

static void path_to_dir(char* path)
{
    char* last_dir = strrchr(path, '/');

    if (last_dir != NULL)
    {
        *last_dir = '\0';
    }
}

static int create_directories(const char* path)
{
    char buffer[PATH_MAX];
    char* token;
    char* saveptr;

    snprintf(buffer, sizeof(buffer), "%s", path);

    char current_path[PATH_MAX] = {0};

    // Process root path
    if (buffer[0] == '/')
    {
        strcat(current_path, "/");
        token = strtok_r(buffer + 1, "/", &saveptr); // Skip the first '/'
    }
    else
    {
        token = strtok_r(buffer, "/", &saveptr);
    }

    while (token != NULL)
    {
        // Joint current path
        if (current_path[strlen(current_path) - 1] != '/')
        {
            strcat(current_path, "/");
        }
        strcat(current_path, token);

        // Check path
        struct fs_dirent entry;
        int rc = fs_stat(current_path, &entry);
        if (rc != 0)
        {
            // If not exist, make it
            rc = fs_mkdir(current_path);
            if (rc != 0)
            {
                LOG_ERROR("Failed to create directory: %s(%d)", current_path,
                          rc);
                return rc;
            }
        }

        token = strtok_r(NULL, "/", &saveptr);
    }

    return 0;
}

static int fs_cp(const char* src_path, const char* dest_path)
{
    int err;
    int close_err;
    struct fs_dirent rw_entry;
    struct fs_file_t src_file, dest_file;
    uint8_t buf[MAX_BUF_LEN];
    ssize_t buf_len;
    ssize_t num_written;

    fs_file_t_init(&src_file);
    fs_file_t_init(&dest_file);

    err = fs_open(&src_file, src_path, FS_O_READ);
    if (err)
    {
        LOG_ERROR("Failed to open %s(%d)", src_path, err);
        err = -EIO;
        goto exit;
    }

    char* dest_dir = strdup(dest_path);
    path_to_dir(dest_dir);
    err = fs_stat(dest_dir, &rw_entry);
    if (err == -ENOENT)
    {
        create_directories(dest_dir);
        free(dest_dir);
    }

    err = fs_open(&dest_file, dest_path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (err)
    {
        LOG_ERROR("Failed to open %s(%d)", dest_path, err);
        err = -EIO;
        goto src_close;
    }

    while (1)
    {
        buf_len = fs_read(&src_file, buf, MAX_BUF_LEN);
        if (buf_len < 0)
        {
            LOG_ERROR("Failed to read %s(%d)", src_path, buf_len);
            err = -EIO;
            goto close;
        }
        if (buf_len == 0)
        {
            break;
        }

        num_written = fs_write(&dest_file, buf, buf_len);
        if (num_written < 0)
        {
            LOG_ERROR("Failed to write %s(%d)", dest_path, num_written);
            err = -EIO;
            goto close;
        }
        if (num_written != buf_len)
        {
            LOG_ERROR("Failed to write %s(%d)", dest_path, num_written);
            err = -EIO;
            goto close;
        }
    }

close:
    close_err = fs_close(&dest_file);
    if (close_err)
    {
        LOG_ERROR("Failed to close %s(%d)", dest_path, close_err);
        err = -EIO;
    }

src_close:
    close_err = fs_close(&src_file);
    if (close_err)
    {
        LOG_ERROR("Failed to close %s(%d)", src_path, close_err);
        err = -EIO;
    }

exit:
    return err;
}

/* File Opreation Function*/

static int overlay_open(struct fs_file_t* fp, const char* path,
                        fs_mode_t zflags)
{
    struct overlay_mount_data* data = fp->mp->fs_data;
    struct fs_dirent rw_entry;
    char rw_path[PATH_MAX], ro_path[PATH_MAX];
    int ret;

    fs_lock(data);

    ret = resolve_path(path, rw_path, ro_path, sizeof(rw_path));
    if (ret != 0)
    {
        fs_unlock(data);
        return ret;
    }

    fs_file_t_init(fp);

    // check rw first
    if (check_file_exists(rw_path) == 0)
    {
        ret = fs_open(fp, rw_path, zflags);
        if (ret)
        {
            fs_unlock(data);
            return ret;
        }
    }
    else if (check_file_exists(ro_path) == 0 &&
             check_file_exists(rw_path) != 0 &&
             (zflags & (FS_O_WRITE | FS_O_TRUNC)))
    {
        ret = fs_cp(ro_path, rw_path);
        if (ret)
        {
            LOG_ERROR("Failed to copy file from %s to %s(%d)", ro_path, rw_path,
                      ret);
            fs_unlock(data);
            return ret;
        }

        ret = fs_open(fp, rw_path, zflags);
        if (ret)
        {
            fs_unlock(data);
            return ret;
        }
    }
    else if (check_file_exists(ro_path) != 0 &&
             check_file_exists(rw_path) != 0 &&
             (zflags & (FS_O_WRITE | FS_O_TRUNC)))
    {
        char* dest_dir = strdup(rw_path);
        path_to_dir(dest_dir);
        ret = fs_stat(dest_dir, &rw_entry);
        if (ret == -ENOENT)
        {
            create_directories(dest_dir);
            free(dest_dir);
        }
        ret = fs_open(fp, rw_path, zflags);
        if (ret)
        {
            fs_unlock(data);
            return ret;
        }
    }
    else
    {
        ret = fs_open(fp, ro_path, zflags);
        if (ret)
        {
            fs_unlock(data);
            return ret;
        }
    }

    return ret;
}

static int overlay_close(struct fs_file_t* fp)
{
    return fs_close(fp);
}

static int overlay_unlink(struct fs_mount_t* mountp, const char* path)
{
    struct overlay_mount_data* data = mountp->fs_data;
    char rw_path[PATH_MAX], ro_path[PATH_MAX];
    int rc;

    fs_lock(data);

    rc = resolve_path(path, rw_path, ro_path, sizeof(rw_path));
    if (rc != 0)
    {
        fs_unlock(data);
        return rc;
    }

    rc = fs_unlink(rw_path);
    if (rc == -ENOENT)
    {
        rc = fs_unlink(ro_path);
    }

    fs_unlock(data);
    return rc;
}

static int overlay_rename(struct fs_mount_t* mountp, const char* from,
                          const char* to)
{
    struct overlay_mount_data* data = mountp->fs_data;
    char from_rw[PATH_MAX], from_ro[PATH_MAX];
    char to_rw[PATH_MAX], to_ro[PATH_MAX];
    int rc;

    fs_lock(data);

    rc = resolve_path(from, from_rw, from_ro, sizeof(from_rw));
    if (rc != 0)
    {
        fs_unlock(data);
        return rc;
    }

    rc = resolve_path(to, to_rw, to_ro, sizeof(to_rw));
    if (rc != 0)
    {
        fs_unlock(data);
        return rc;
    }

    rc = fs_rename(from_rw, to_rw);
    if (rc == -ENOENT)
    {
        rc = fs_rename(from_ro, to_ro);
    }

    fs_unlock(data);
    return rc;
}

static ssize_t overlay_read(struct fs_file_t* fp, void* ptr, size_t len)
{
    if (!fp->filep)
    {
        return -EBADF;
    }

    return fs_read(fp, ptr, len);
}

static ssize_t overlay_write(struct fs_file_t* fp, const void* ptr, size_t len)
{
    if (!fp->filep)
    {
        return -EBADF;
    }

    return fs_write(fp, ptr, len);
}

static int overlay_seek(struct fs_file_t* fp, off_t off, int whence)
{
    if (!fp->filep)
    {
        return -EBADF;
    }

    return fs_seek(fp, off, whence);
}

static off_t overlay_tell(struct fs_file_t* fp)
{
    if (!fp->filep)
    {
        return -EBADF;
    }

    return fs_tell(fp);
}

static int overlay_truncate(struct fs_file_t* fp, off_t length)
{
    if (!fp->filep)
    {
        return -EROFS;
    }

    return fs_truncate(fp, length);
}

static int overlay_sync(struct fs_file_t* fp)
{
    if (!fp->filep)
    {
        return -EROFS;
    }

    return fs_sync(fp);
}

static int overlay_mkdir(struct fs_mount_t* mountp, const char* path)
{
    struct overlay_mount_data* data = mountp->fs_data;
    char rw_path[PATH_MAX], ro_path[PATH_MAX];
    int rc;

    fs_lock(data);

    rc = resolve_path(path, rw_path, ro_path, sizeof(rw_path));
    if (rc != 0)
    {
        fs_unlock(data);
        return rc;
    }

    rc = fs_mkdir(rw_path);
    fs_unlock(data);
    return rc;
}

int fs_dir_read(struct fs_dir_t* dp, const char* part)
{
    while (1)
    {
        struct fs_dirent entry;

        int rc = fs_readdir(dp, &entry);
        if (rc)
        {
            LOG_ERROR("Failed to read %s dir(%d)", part, rc);
            return rc;
        }

        if (entry.name[0] == 0)
            break;

        if (strcmp(part, rwfs_mnt_point) == 0)
        {
            if (!is_dir_exists(entry.name))
            {
                add_dir(entry);
            }
        }
        else
        {
            add_dir(entry);
        }
    }
}

static int overlay_opendir(struct fs_dir_t* dp, const char* path)
{
    struct overlay_mount_data* data = dp->mp->fs_data;
    struct overlay_dir_t* ol_dir;
    char rw_path[PATH_MAX], ro_path[PATH_MAX];
    int rc, close_rc;

    fs_lock(data);

    rc = resolve_path(path, rw_path, ro_path, sizeof(rw_path));
    if (rc != 0)
    {
        fs_unlock(data);
        return rc;
    }

    struct fs_dir_t dir_ro;
    struct fs_dir_t dir_rw;
    struct fs_dirent ro_stat_entry, rw_stat_entry;

    fs_dir_t_init(&dir_ro);
    fs_dir_t_init(&dir_rw);

    if (fs_stat(rw_path, &rw_stat_entry) && fs_stat(ro_path, &ro_stat_entry))
    {
        return -ENOENT;
    }

    if (!fs_stat(ro_path, &ro_stat_entry))
    {
        rc = fs_opendir(&dir_ro, ro_path);
        if (rc)
        {
            LOG_ERROR("Failed to open ro dir(%d)", rc);
            goto ro_close;
        }

        rc = fs_dir_read(&dir_ro, rofs_mnt_point);
        if (rc)
            goto ro_close;
    }

    if (!fs_stat(rw_path, &rw_stat_entry))
    {
        rc = fs_opendir(&dir_rw, rw_path);
        if (rc)
        {
            LOG_ERROR("Failed to open rw dir(%d)", rc);
            goto rw_close;
        }

        rc = fs_dir_read(&dir_rw, rwfs_mnt_point);
        if (rc)
            goto rw_close;
    }

    if (head)
    {
        dp->dirp = head;
    }
    else
    {
        // dir is empty, return dummy
        dp->dirp = &dummy_dir;
    }

rw_close:
    close_rc = fs_closedir(&dir_rw);
    if (close_rc)
    {
        LOG_ERROR("Failed to close rw dir(%d)", close_rc);
        rc = -EIO;
    }

ro_close:
    close_rc = fs_closedir(&dir_ro);
    if (close_rc)
    {
        LOG_ERROR("Failed to close ro dir(%d)", close_rc);
        rc = -EIO;
    }

exit:
    fs_unlock(data);
    return rc;
}

static int overlay_readdir(struct fs_dir_t* dp, struct fs_dirent* entry)
{
    if (!dp->dirp)
    {
        return -EBADF;
    }

    if (readed_dir == NULL)
    {
        overlay_dir_t* dir = (overlay_dir_t*)dp->dirp;
        *entry = dir->entry;
        readed_dir = dir;
    }
    else
    {
        if (readed_dir->next != NULL)
        {
            *entry = readed_dir->next->entry;
            readed_dir = readed_dir->next;
        }
        else
        {
            entry->name[0] = 0;
        }
    }

    return 0;
}

static int overlay_closedir(struct fs_dir_t* dp)
{
    if (!dp->dirp)
    {
        return -EBADF;
    }

    free_dir();
    dp->dirp = NULL;
    readed_dir = NULL;

    return 0;
}

static int overlay_stat(struct fs_mount_t* mountp, const char* path,
                        struct fs_dirent* entry)
{
    struct overlay_mount_data* data = mountp->fs_data;
    char rw_path[PATH_MAX], ro_path[PATH_MAX];
    int rc;

    fs_lock(data);

    rc = resolve_path(path, rw_path, ro_path, sizeof(rw_path));
    if (rc != 0)
    {
        fs_unlock(data);
        return rc;
    }

    rc = fs_stat(rw_path, entry);
    if (rc == -ENOENT)
    {
        rc = fs_stat(ro_path, entry);
    }

    fs_unlock(data);
    return rc;
}

static int overlay_statvfs(struct fs_mount_t* mountp, const char* path,
                           struct fs_statvfs* stat)
{
    struct overlay_mount_data* data = mountp->fs_data;
    char rw_path[PATH_MAX], ro_path[PATH_MAX];
    int rc;

    fs_lock(data);

    rc = resolve_path(path, rw_path, ro_path, sizeof(rw_path));
    if (rc != 0)
    {
        fs_unlock(data);
        return rc;
    }

    rc = fs_statvfs(rw_path, stat);
    if (rc == -ENOENT)
    {
        rc = fs_statvfs(ro_path, stat);
    }

    fs_unlock(data);
    return rc;
}

static int overlay_mount(struct fs_mount_t* mountp)
{
    struct overlay_mount_data* data = mountp->fs_data;
    if (!data || !data->ro_mnt || !data->rw_mnt)
    {
        return -EINVAL;
    }

    k_mutex_init(&data->mutex);

    overlayfs_mnt_point = malloc(strlen(mountp->mnt_point) + 1);
    if (!overlayfs_mnt_point)
    {
        LOG_ERROR("Failed to allocate overlayfs_mnt_point");
        return -ENOMEM;
    }
    rofs_mnt_point = malloc(strlen(data->ro_mnt->mnt_point) + 1);
    if (!rofs_mnt_point)
    {
        free(overlayfs_mnt_point);
        LOG_ERROR("Failed to allocate rofs_mnt_point");
        return -ENOMEM;
    }
    rwfs_mnt_point = malloc(strlen(data->rw_mnt->mnt_point) + 1);
    if (!rwfs_mnt_point)
    {
        free(overlayfs_mnt_point);
        free(rofs_mnt_point);
        LOG_ERROR("Failed to allocate rwfs_mnt_point");
        return -ENOMEM;
    }

    strcpy(overlayfs_mnt_point, mountp->mnt_point);
    strcpy(rofs_mnt_point, data->ro_mnt->mnt_point);
    strcpy(rwfs_mnt_point, data->rw_mnt->mnt_point);

    head = NULL;
    readed_dir = NULL;

    LOG_INF("Overlay mounted at %s", mountp->mnt_point);
    return 0;
}

static int overlay_unmount(struct fs_mount_t* mountp)
{
    if (overlayfs_mnt_point)
    {
        free(overlayfs_mnt_point);
        overlayfs_mnt_point = NULL;
    }
    if (rofs_mnt_point)
    {
        free(rofs_mnt_point);
        rofs_mnt_point = NULL;
    }
    if (rwfs_mnt_point)
    {
        free(rwfs_mnt_point);
        rwfs_mnt_point = NULL;
    }

    LOG_INF("Overlay unmounted from %s", mountp->mnt_point);
    return 0;
}

/* File system interface */
static const struct fs_file_system_t overlay_fs = {
    .open = overlay_open,
    .close = overlay_close,
    .read = overlay_read,
    .write = overlay_write,
    .lseek = overlay_seek,
    .tell = overlay_tell,
    .truncate = overlay_truncate,
    .sync = overlay_sync,
    .opendir = overlay_opendir,
    .readdir = overlay_readdir,
    .closedir = overlay_closedir,
    .mount = overlay_mount,
    .unmount = overlay_unmount,
    .unlink = overlay_unlink,
    .rename = overlay_rename,
    .mkdir = overlay_mkdir,
    .stat = overlay_stat,
    .statvfs = overlay_statvfs,
};

int overlay_init(void)
{
    int rc;

    rc = fs_register(FS_OVERLAYFS, &overlay_fs);
    if (rc < 0)
    {
        LOG_ERROR("overlayfs register error (%d)\n", rc);
    }
    else
    {
        LOG_ERROR("overlayfs successfully registered\n");
    }

    return rc;
}

SYS_INIT(overlay_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
