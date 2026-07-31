#define _POSIX_C_SOURCE 200809L

#include "cubicle/util.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int cubicle_write_all(int fd, const char *buffer, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(fd, buffer + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        written += (size_t)result;
    }

    return 0;
}

static int mkdir_if_needed(const char *path)
{
    if (mkdir(path, 0700) == 0) {
        return 0;
    }

    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
    }

    return -1;
}

int cubicle_mkdir_p(const char *path)
{
    char current[CUBICLE_PATH_MAX];
    size_t length = strlen(path);

    if (length == 0 || length >= sizeof(current)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(current, path, length + 1);

    for (char *p = current + 1; *p != '\0'; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        if (mkdir_if_needed(current) < 0) {
            return -1;
        }
        *p = '/';
    }

    return mkdir_if_needed(current);
}

int cubicle_generate_hex_id(char *id, size_t id_size)
{
    if (id_size != 33) {
        errno = EINVAL;
        return -1;
    }

    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        ssize_t result = read(fd, bytes + offset, sizeof(bytes) - offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(fd);
            return -1;
        }

        if (result == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }

        offset += (size_t)result;
    }

    close(fd);

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        id[i * 2] = hex[bytes[i] >> 4];
        id[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    id[32] = '\0';
    return 0;
}
