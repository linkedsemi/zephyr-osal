#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <zephyr/init.h>
#include <zephyr/fs/fs.h>
#include <zephyr/random/random.h>
#include <stdlib.h>

static int cmd_dd(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3) {
        shell_error(sh, "Usage: dd if=<src> of=<dst> [bs=<size>] [count=<n>]");
        return -EINVAL;
    }

    const char *input = NULL, *output = NULL;
    size_t bs = 512, count = 1;

    // 解析参数
    for (size_t i = 1; i < argc; i++) {
        if (strncmp(argv[i], "if=", 3) == 0) {
            input = argv[i] + 3;
        } else if (strncmp(argv[i], "of=", 3) == 0) {
            output = argv[i] + 3;
        } else if (strncmp(argv[i], "bs=", 3) == 0) {
            bs = strtoul(argv[i] + 3, NULL, 0);
        } else if (strncmp(argv[i], "count=", 6) == 0) {
            count = strtoul(argv[i] + 6, NULL, 0);
        } else {
            shell_error(sh, "Unknown arg: %s", argv[i]);
            return -EINVAL;
        }
    }

    if (!input || !output) {
        shell_error(sh, "Missing if= or of=");
        return -EINVAL;
    }

    if (bs == 0 || count == 0) {
        shell_error(sh, "bs and count must be > 0");
        return -EINVAL;
    }

    size_t total_size = bs * count;
    uint8_t *buffer = malloc(bs);
    if (!buffer) {
        shell_error(sh, "Failed to allocate buffer (bs=%zu)", bs);
        return -ENOMEM;
    }

    struct fs_file_t in_file;
    struct fs_file_t out_file;
    fs_file_t_init(&in_file);
    fs_file_t_init(&out_file);

    bool is_zero = (strcmp(input, "/dev/zero") == 0);
    bool is_urandom = (strcmp(input, "/dev/urandom") == 0);
    bool is_file = !(is_zero || is_urandom);

    int err = 0;

    // Open input if it's a real file
    if (is_file) {
        err = fs_open(&in_file, input, FS_O_READ);
        if (err) {
            shell_error(sh, "Failed to open input file %s (%d)", input, err);
            free(buffer);
            return -ENOENT;
        }
    }

    // Open output file
    err = fs_open(&out_file, output, FS_O_CREATE | FS_O_WRITE);
    if (err) {
        shell_error(sh, "Failed to open output %s (%d)", output, err);
        if (is_file) {
            fs_close(&in_file);
        }
        free(buffer);
        return -ENOEXEC;
    }

    // Main write loop
    for (size_t i = 0; i < count; i++) {
        if (is_zero) {
            memset(buffer, 0, bs);
        } else if (is_urandom) {
            sys_rand_get(buffer, bs);  // Zephyr's random generator
        } else {
            // Read from input file, pad with zeros if EOF reached
            ssize_t read_bytes = fs_read(&in_file, buffer, bs);
            if (read_bytes < 0) {
                shell_error(sh, "Read failed (%zd)", read_bytes);
                break;
            }
            if ((size_t)read_bytes < bs) {
                // Pad remaining with zeros (like standard dd)
                memset(buffer + read_bytes, 0, bs - read_bytes);
            }
        }

        ssize_t written = fs_write(&out_file, buffer, bs);
        if (written < 0) {
            shell_error(sh, "Write failed (%zd)", written);
            break;
        }
    }

    // Cleanup
    fs_close(&out_file);
    if (is_file) {
        fs_close(&in_file);
    }
    free(buffer);

    shell_print(sh, "%zu bytes written to %s", total_size, output);
    return 0;
}

SHELL_CMD_ARG_REGISTER(dd, NULL, 
    "Copy data with conversion and formatting\n"
    "Usage:\n"
    "  dd if=/dev/zero     of=<file> bs=<size> count=<n>  # Fill with zeros (erase)\n"
    "  dd if=/dev/urandom  of=<file> bs=<size> count=<n>  # Fill with random data\n"
    "  dd if=<src_file>    of=<file> bs=<size> count=<n>  # Copy from source file",
    cmd_dd, 3, 255);