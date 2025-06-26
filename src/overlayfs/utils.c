#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/logging/log.h>

#include "overlay_fs.h"

LOG_MODULE_DECLARE(overlay_fs);

overlay_dir_t *head;

int is_dir_exists(const char *path)
{
    overlay_dir_t *cur = head;
    
    while(cur != NULL) {
        if (strcmp(cur->entry.name, path) == 0) {
            return 1;
        }
        cur = cur->next;
    }

    return 0;
}

void add_dir(const struct fs_dirent entry)
{
    if (is_dir_exists(entry.name)) {
        return;
    }

    overlay_dir_t *new_dir = (overlay_dir_t *)malloc(sizeof(overlay_dir_t));
    if (new_dir == NULL) {
        LOG_ERROR("malloc failed");
        return;
    }

    new_dir->entry = entry;
    new_dir->next = head;
    head = new_dir;

}

void free_dir() {
    overlay_dir_t *cur = head;

    while (cur != NULL) {
        overlay_dir_t *temp = cur;
        cur = cur->next;
        free(temp);
    }

    head = NULL;
}