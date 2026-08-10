#define _POSIX_C_SOURCE 200809L

#include "../src/controller/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int make_client_fd(void)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        perror("socketpair");
        return -1;
    }
    close(pair[1]);
    return pair[0];
}

static int fd_is_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0 || errno != EBADF;
}

static int assert_attached_client_survives(control_client_kind_t kind)
{
    control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS];
    controller_state_t state;
    initialize_control_clients(clients);
    initialize_empty_controller_state(&state);

    int fd = make_client_fd();
    if (fd < 0) {
        return 1;
    }
    if (set_nonblocking(fd) < 0) {
        perror("set_nonblocking");
        close(fd);
        return 1;
    }

    clients[0].fd = fd;
    clients[0].kind = kind;
    clients[0].last_activity_ms = 1;

    reap_idle_control_clients(clients, &state);
    if (clients[0].kind != kind || clients[0].fd != fd || !fd_is_open(fd)) {
        fprintf(stderr, "attached client kind %d was reaped\n", (int)kind);
        close_all_control_clients(clients, &state);
        return 1;
    }

    close_all_control_clients(clients, &state);
    return 0;
}

static int assert_stale_request_client_is_reaped(void)
{
    control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS];
    controller_state_t state;
    initialize_control_clients(clients);
    initialize_empty_controller_state(&state);

    int fd = make_client_fd();
    if (fd < 0) {
        return 1;
    }

    clients[0].fd = fd;
    clients[0].kind = CONTROL_CLIENT_READING;
    clients[0].last_activity_ms = 1;

    reap_idle_control_clients(clients, &state);
    if (clients[0].kind != CONTROL_CLIENT_EMPTY || clients[0].fd != -1) {
        fprintf(stderr, "stale request client was not reaped\n");
        close_all_control_clients(clients, &state);
        return 1;
    }

    return 0;
}

int main(void)
{
    if (assert_attached_client_survives(CONTROL_CLIENT_ATTACHED_STDOUT) != 0 ||
        assert_attached_client_survives(CONTROL_CLIENT_ATTACHED_STDERR) != 0 ||
        assert_attached_client_survives(CONTROL_CLIENT_ATTACHED_STDIN) != 0 ||
        assert_stale_request_client_is_reaped() != 0) {
        return 1;
    }
    return 0;
}
