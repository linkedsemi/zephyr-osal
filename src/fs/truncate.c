#include <errno.h>
#include <zephyr/fs/fs.h>

int truncate (const char *path, off_t length)
{
    int ret;
    struct fs_file_t zf;

    if (path == NULL || length < 0) {
        return -EINVAL;
    }

    fs_file_t_init(&zf);
    ret = fs_open(&zf, path, FS_O_RDWR);
    if (ret < 0)
    {
        return ret;
    }

    ret = fs_truncate(&zf, length);

    fs_close(&zf);
    return ret;
}