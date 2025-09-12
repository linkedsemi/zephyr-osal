#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/unistd.h>
#include <zephyr/fs/fs.h>

#define PATH_MAX 256

char* realpath(const char* path, char* resolved_path)
{
    char temp_path[PATH_MAX];
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (path[0] != '/') {
        /* it is an relative path */
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            return NULL;
        }
        
        /* concatenate the current directory and relative path*/
        int cwd_len = strlen(cwd);
        int path_len = strlen(path);
        
        /* ensure that it does not exceed the buffer area */
        if (cwd_len + 1 + path_len >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        
        /* construct an absolute path */
        strcpy(temp_path, cwd);
        if (cwd[cwd_len - 1] != '/') {
            strcat(temp_path, "/");
        }
        strcat(temp_path, path);
    } else {
        /* it is an absolute path */
        strncpy(temp_path, path, PATH_MAX - 1);
        temp_path[PATH_MAX - 1] = '\0';
    }

    char* out_path = resolved_path;
    if (out_path == NULL) {
        out_path = (char *)malloc(PATH_MAX);
        if (out_path == NULL) {
            errno = ENOMEM;
            return NULL;
        }
    }

    /* canonical path starts with / */
    strlcpy(out_path, "/", PATH_MAX);

    /* pointers moving over the input and output path buffers */
    const char* in_ptr = temp_path;
    char* out_ptr = out_path + 1;
    /* number of path components in the output buffer */
    size_t out_depth = 0;

    while (*in_ptr) {
        /* "path component" is the part between two '/' path separators.
         * locate the next path component in the input path:
         */
        const char* end_of_path_component = strchrnul(in_ptr, '/');
        size_t path_component_len = end_of_path_component - in_ptr;

        if (path_component_len == 0 ||
            (path_component_len == 1 && in_ptr[0] == '.')) {
            /* empty path component or '.' - nothing to do */
        } else if (path_component_len == 2 && in_ptr[0] == '.' && in_ptr[1] == '.') {
            /* '..' - remove one path component from the output */
            if (out_depth == 0) {
                /* nothing to remove */
            } else if (out_depth == 1) {
                /* there is only one path component in output;
                 * remove it, but keep the leading separator
                 */
                out_ptr = out_path + 1;
                *out_ptr = '\0';
                out_depth = 0;
            } else {
                /* remove last path component and the separator preceding it */
                char * prev_sep = strrchr(out_path, '/');
                assert(prev_sep > out_path);  /* this shouldn't be the leading separator */
                out_ptr = prev_sep;
                *out_ptr = '\0';
                --out_depth;
            }
        } else {
            /* copy path component to output; +1 is for the separator  */
            if (out_ptr - out_path + 1 + path_component_len > PATH_MAX - 1) {
                /* output buffer insufficient */
                errno = E2BIG;
                goto fail;
            } else {
                /* add separator if necessary */
                if (out_depth > 0) {
                    *out_ptr = '/';
                    ++out_ptr;
                }
                memcpy(out_ptr, in_ptr, path_component_len);
                out_ptr += path_component_len;
                *out_ptr = '\0';
                ++out_depth;
            }
        }
        /* move input pointer to separator right after this path component */
        in_ptr += path_component_len;
        if (*in_ptr != '\0') {
            /* move past it unless already at the end of the input string */
            ++in_ptr;
        }
    }
    return out_path;

fail:
    if (resolved_path == NULL) {
        /* out_path was allocated, free it */
        free(out_path);
    }
    return NULL;
}

__weak_symbol char* getcwd(char* __buf, size_t __size)
{
    return NULL;
}
