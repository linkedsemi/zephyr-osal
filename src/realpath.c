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
        // 获取当前工作目录（假设为根目录 "/"）
        strcpy(resolved_path, "/");
    }

    // 复制路径到临时缓冲区
    strncpy(temp_path, path, PATH_MAX - 1);
    temp_path[PATH_MAX - 1] = '\0';

    // 使用 strtok 分割路径
    token = strtok(temp_path, "/");
    while (token) {
        if (strcmp(token, ".") == 0) {
            // 忽略 "."
        } else if (strcmp(token, "..") == 0) {
            // 处理 ".."
            last_slash = strrchr(resolved_path, '/');
            if (last_slash && last_slash != resolved_path) {
                *last_slash = '\0';
            }
        } else {
            // 检查路径是否存在
            snprintf(temp_path, PATH_MAX, "%s/%s", resolved_path, token);
            if (fs_stat(temp_path, &entry) < 0) {
                //return -ENOENT;   // 路径不存在  暂时注释掉 WGP20250529
            }

            // 添加路径段
            size_t len = strlen(resolved_path);
            if (len > 0 && resolved_path[len - 1] != '/') {
                strncat(resolved_path, "/", PATH_MAX - len - 1);
            }
            if(len==0)
            strcpy(resolved_path, "/");
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

    // 如果 resolved_path 为 NULL，则动态分配内存
    char* allocated_path = NULL;
    if (!resolved_path) {
        resolved_path = malloc(PATH_MAX);
        if (!resolved_path) {
            // 内存分配失败
            errno = ENOMEM;
            return NULL;
        }
        allocated_path = resolved_path; // 记录动态分配的内存
    } else {
        // 确保 resolved_path 至少有 PATH_MAX 的空间
        if (strlen(resolved_path) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }
    }

    if (resolve_path(path, resolved_path) < 0) {
        // 解析失败，释放动态分配的内存（如果有）
        if (allocated_path) {
            free(allocated_path);
        }
        return NULL;
    }

    // 如果 resolved_path 是动态分配的，返回时需要告知调用者
    return resolved_path;
}
