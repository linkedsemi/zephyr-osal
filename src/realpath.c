#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/unistd.h>
#include <zephyr/fs/fs.h>

#define PATH_MAX 256

static int resolve_path(const char* path, char* resolved_path)
{
    struct fs_dirent entry;
    char* token;
    char temp_path[PATH_MAX];
    char* last_slash;

    if (!path || !resolved_path)
    {
        return -EINVAL;
    }

    if (path[0] != '/')
    {
        // 处理相对路径
        if (getcwd(resolved_path, PATH_MAX) == NULL)
        {
            return -errno;
        }
        // 确保路径以 '/' 结尾，便于后续处理
        size_t len = strlen(resolved_path);
        if (len > 0 && resolved_path[len - 1] != '/')
        {
            if (len + 1 < PATH_MAX)
            {
                resolved_path[len] = '/';
                resolved_path[len + 1] = '\0';
            }
        }
    }
    else
    {
        // 处理绝对路径
        resolved_path[0] = '/';
        resolved_path[1] = '\0';
    }

    // 复制路径到临时缓冲区
    strncpy(temp_path, path, sizeof(temp_path) - 1);
    temp_path[sizeof(temp_path) - 1] = '\0';

    // 使用 strtok 分割路径
    token = strtok(temp_path, "/");
    while (token)
    {
        if (strcmp(token, ".") == 0)
        {
            // 忽略 "." 当前目录
        }
        else if (strcmp(token, "..") == 0)
        {
            // 处理 ".." 上级目录
            last_slash = strrchr(resolved_path, '/');
            if (last_slash && last_slash != resolved_path)
            {
                *last_slash = '\0';
                last_slash = strrchr(resolved_path, '/');
                if (last_slash)
                {
                    *(last_slash + 1) = '\0';
                }
                else
                {
                    // 恢复
                    resolved_path[0] = '/';
                    resolved_path[1] = '\0';
                }
            }
        }
        else
        {
            // 处理普通目录名
            size_t resolved_len = strlen(resolved_path);
            size_t token_len = strlen(token);

            // 确保路径连接正确
            if (resolved_len > 0 && resolved_path[resolved_len - 1] != '/')
            {
                if (resolved_len + token_len + 2 <= PATH_MAX)
                {
                    strcat(resolved_path, "/");
                    strcat(resolved_path, token);
                }
                else
                {
                    return -ENAMETOOLONG;
                }
            }
            else
            {
                if (resolved_len + token_len + 1 <= PATH_MAX)
                {
                    strcat(resolved_path, token);
                }
                else
                {
                    return -ENAMETOOLONG;
                }
            }
        }
        token = strtok(NULL, "/");
    }

    // 确保路径以 null 结尾且格式正确
    size_t final_len = strlen(resolved_path);
    if (final_len > 1 && resolved_path[final_len - 1] == '/')
    {
        resolved_path[final_len - 1] = '\0';
    }

    return 0;
}

char* realpath(const char* path, char* resolved_path)
{
    if (!path)
    {
        errno = EINVAL;
        return NULL;
    }

    char* allocated_path = NULL;
    if (!resolved_path)
    {
        resolved_path = malloc(PATH_MAX);
        if (!resolved_path)
        {
            errno = ENOMEM;
            return NULL;
        }
        allocated_path = resolved_path;
    }
    else
    {
        // 确保传入的 resolved_path 有足够空间
        if (strlen(resolved_path) >= PATH_MAX)
        {
            errno = ENAMETOOLONG;
            return NULL;
        }
    }

    if (resolve_path(path, resolved_path) < 0)
    {
        if (allocated_path)
        {
            free(allocated_path);
        }
        return NULL;
    }

    return resolved_path;
}

__weak_symbol char* getcwd(char* __buf, size_t __size)
{
    return NULL;
}
