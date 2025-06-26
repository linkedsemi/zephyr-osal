#pragma once
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

#define PATH_MAX 256

struct overlay_mount_data {
    struct fs_mount_t *ro_mnt;
    struct fs_mount_t *rw_mnt;
    struct k_mutex mutex; 
};

struct overlay_file_data {
    struct fs_file_t *fp_ro;
    struct fs_file_t *fp_rw; 
};

typedef struct overlay_dir {
    struct overlay_dir *next;
    struct fs_dirent entry;
}overlay_dir_t;

// Utils for overlay_dir
void add_dir(const struct fs_dirent entry);
int is_dir_exists(const char *path);
void free_dir( );