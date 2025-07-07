#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define PATH_MAX 256

static int resolve_path(const char *path, char *resolved_path) {
    struct fs_dirent entry;
    char *token;
    char temp_path[PATH_MAX];
    char *last_slash;

    if (!path || !resolved_path) {
        return -EINVAL;
    }

    // 初始化绝对路径
    resolved_path[0] = '\0';

    // 处理相对路径
    if (path[0] != '/') {
        resolved_path[0] = '/';
        resolved_path[1] = '\0';
    }

    // 复制路径到临时缓冲区
    strncpy(temp_path, path, sizeof(temp_path) - 1);
    temp_path[sizeof(temp_path) - 1] = '\0';

    // 使用 strtok 分割路径
    token = strtok(temp_path, "/");
    while (token) {
        if (strcmp(token, ".") == 0) {
            // 忽略 "." 当前目录
        } else if (strcmp(token, "..") == 0) {
            // 处理 ".." 上级目录
            last_slash = strrchr(resolved_path, '/');
            if (last_slash && last_slash != resolved_path) {
                *last_slash = '\0';
            }
        } else {
            // 构造临时路径进行检查
            size_t len = strlen(resolved_path);
            if (len + strlen(token) + 2 > PATH_MAX) {
                return -ENAMETOOLONG; // 路径过长
            }

            snprintf(temp_path, sizeof(temp_path), "%s/%s", resolved_path, token);
            if (fs_stat(temp_path, &entry) < 0) {
                //return -ENOENT; // 路径不存在（暂时注释）
            }

            // 添加路径段
            if (len > 0 && resolved_path[len - 1] != '/') {
                strncat(resolved_path, "/", PATH_MAX - len - 1);
            }
            if (len == 0) {
                resolved_path[0] = '/';
                resolved_path[1] = '\0';
            }
            strncat(resolved_path, token, PATH_MAX - strlen(resolved_path) - 1);
        }
        token = strtok(NULL, "/");
    }

    return 0;
}

char* realpath(const char* path, char* resolved_path) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    char* allocated_path = NULL;
    if (!resolved_path) {
        resolved_path = malloc(PATH_MAX);
        if (!resolved_path) {
            errno = ENOMEM;
            return NULL;
        }
        allocated_path = resolved_path;
    } else {
        // 确保传入的 resolved_path 有足够空间
        if (strlen(resolved_path) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }
    }

    if (resolve_path(path, resolved_path) < 0) {
        if (allocated_path) {
            free(allocated_path);
        }
        return NULL;
    }

    return resolved_path;
}