#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

void close_if_open(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int write_best_effort(int fd, const char *buffer, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(fd, buffer + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                errno = EAGAIN;
                return -1;
            }

            return -1;
        }

        written += (size_t)result;
    }

    return 0;
}
