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

#pragma once
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

#define PATH_MAX 256

struct overlay_mount_data {
    struct fs_mount_t *ro_mnt;
    struct fs_mount_t *rw_mnt;
    struct k_mutex mutex;
};

typedef struct overlay_dir {
    struct overlay_dir *next;
    struct fs_dirent entry;
}overlay_dir_t;

struct overlay_dirp {
    overlay_dir_t* head;
    overlay_dir_t* readed_dir;
};

// Utils for overlay_dir
void add_dir(struct overlay_dirp *dirp, const struct fs_dirent *entry);
int is_dir_exists(struct overlay_dirp *dirp, const char *path);
void free_dir(struct overlay_dirp *dirp);