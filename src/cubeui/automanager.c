#define _POSIX_C_SOURCE 200809L

#include "cubeui.h"

#include "cubicle/util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int unix_socket_connectable(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t length = strlen(path);
    if (length >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, path, length + 1);

    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result;
}

static void sleep_for_manager_start(void)
{
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 50 * 1000000L,
    };
    nanosleep(&delay, NULL);
}

static int manager_binary_path(const cubicle_config_t *config,
                               char *path,
                               size_t path_size)
{
    const char *bindir = config != NULL ? config->bindir : "";
    if (bindir == NULL || bindir[0] == '\0') {
        int length = snprintf(path, path_size, "cubicle-manager");
        return length < 0 || (size_t)length >= path_size ? -1 : 0;
    }
    int length = snprintf(path, path_size, "%s/cubicle-manager", bindir);
    return length < 0 || (size_t)length >= path_size ? -1 : 0;
}

int cubeui_autostart_manager(const char *manager_uri,
                             const cubicle_config_t *config,
                             int enabled,
                             char *error,
                             size_t error_size)
{
    if (!enabled) {
        return 0;
    }

    char socket_path[CUBICLE_PATH_MAX];
    if (manager_uri != NULL && manager_uri[0] == '/') {
        int length = snprintf(socket_path, sizeof(socket_path), "%s",
                              manager_uri);
        if (length < 0 || (size_t)length >= sizeof(socket_path)) {
            if (error != NULL && error_size > 0) {
                snprintf(error, error_size, "manager socket path is too long");
            }
            return -1;
        }
    } else if (cubicle_config_unix_uri_path(manager_uri, socket_path,
                                            sizeof(socket_path)) < 0) {
        return 0;
    }
    if (unix_socket_connectable(socket_path) == 0) {
        return 0;
    }

    char listen_uri[CUBICLE_ENDPOINT_URI_MAX];
    int length = snprintf(listen_uri, sizeof(listen_uri), "unix://%s",
                          socket_path);
    if (length < 0 || (size_t)length >= sizeof(listen_uri)) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "manager socket path is too long");
        }
        return -1;
    }

    char manager_binary[CUBICLE_PATH_MAX];
    if (manager_binary_path(config, manager_binary,
                            sizeof(manager_binary)) < 0) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "manager binary path is too long");
        }
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "failed to start manager: %s",
                     strerror(errno));
        }
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        execl(manager_binary, manager_binary, "--listen", listen_uri,
              (char *)NULL);
        execlp("cubicle-manager", "cubicle-manager", "--listen", listen_uri,
               (char *)NULL);
        _exit(127);
    }

    int status = 0;
    (void)waitpid(pid, &status, 0);
    for (int i = 0; i < 40; ++i) {
        if (unix_socket_connectable(socket_path) == 0) {
            return 0;
        }
        sleep_for_manager_start();
    }

    if (error != NULL && error_size > 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            snprintf(error, error_size,
                     "failed to execute cubicle-manager for auto-start");
        } else {
            snprintf(error, error_size,
                     "manager auto-start did not create %s", socket_path);
        }
    }
    return -1;
}
