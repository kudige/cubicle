#define _POSIX_C_SOURCE 200809L

#include "cubicle/log.h"
#include "cubicle/process.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CUBICLE_MAX_SIGNAL_NUMBER 128
#define CUBICLE_MAX_CONTROL_CLIENTS 32
#define CUBICLE_REQUEST_MAX 256

typedef enum control_client_kind {
    CONTROL_CLIENT_EMPTY = 0,
    CONTROL_CLIENT_READING = 1,
    CONTROL_CLIENT_ATTACHED_STDOUT = 2,
    CONTROL_CLIENT_ATTACHED_STDERR = 3
} control_client_kind_t;

typedef struct stream_pipe {
    int fd;
    int output_fd;
    int log_fd;
    const char *name;
    long long *offset;
    int open;
} stream_pipe_t;

typedef struct controller_state {
    char dir[PATH_MAX];
    int events_fd;
    int stdout_fd;
    int stderr_fd;
    long long next_sequence;
    long long stdout_offset;
    long long stderr_offset;
} controller_state_t;

typedef struct control_client {
    int fd;
    control_client_kind_t kind;
    char request[CUBICLE_REQUEST_MAX];
    size_t request_length;
} control_client_t;

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--daemon] [--state-dir dir] [--control-socket path] --mode stream|tty|tty-captured-stderr -- command [args...]\n",
            program);
}

static int daemonize_controller(void)
{
    pid_t child_pid = fork();
    if (child_pid < 0) {
        return -1;
    }

    if (child_pid > 0) {
        _exit(0);
    }

    if (setsid() < 0) {
        return -1;
    }

    signal(SIGHUP, SIG_IGN);

    child_pid = fork();
    if (child_pid < 0) {
        return -1;
    }

    if (child_pid > 0) {
        _exit(0);
    }

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0) {
        return -1;
    }

    if (dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(null_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0) {
        close(null_fd);
        return -1;
    }

    if (null_fd > STDERR_FILENO) {
        close(null_fd);
    }

    return 0;
}

static int parse_mode(const char *name, cubicle_process_mode_t *mode)
{
    if (strcmp(name, "stream") == 0) {
        *mode = CUBICLE_PROCESS_STREAM;
        return 0;
    }

    if (strcmp(name, "tty") == 0) {
        *mode = CUBICLE_PROCESS_TTY;
        return 0;
    }

    if (strcmp(name, "tty-captured-stderr") == 0) {
        *mode = CUBICLE_PROCESS_TTY_CAPTURED_STDERR;
        return 0;
    }

    return -1;
}

static void close_if_open(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int write_all(int fd, const char *buffer, size_t length)
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

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int write_best_effort(int fd, const char *buffer, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(fd, buffer + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
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

static int mkdir_p(const char *path)
{
    char current[PATH_MAX];
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

static int open_state_file(const char *dir, const char *name, int flags)
{
    char path[PATH_MAX];
    int result = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (result < 0 || (size_t)result >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return open(path, flags, 0600);
}

static int make_state_file_path(char path[PATH_MAX], const char *dir,
                                const char *name)
{
    int result = snprintf(path, PATH_MAX, "%s/%s", dir, name);
    if (result < 0 || result >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static void close_controller_state(controller_state_t *state)
{
    close_if_open(&state->events_fd);
    close_if_open(&state->stdout_fd);
    close_if_open(&state->stderr_fd);
}

static int append_event(controller_state_t *state, const char *event)
{
    char line[1024];
    int length = snprintf(line, sizeof(line), "seq=%lld %s\n",
                          state->next_sequence++, event);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return write_all(state->events_fd, line, (size_t)length);
}

static int initialize_controller_state(controller_state_t *state,
                                       const char *requested_dir,
                                       pid_t child_pid,
                                       char **command)
{
    memset(state, 0, sizeof(*state));
    state->events_fd = -1;
    state->stdout_fd = -1;
    state->stderr_fd = -1;
    state->next_sequence = 1;

    if (requested_dir != NULL) {
        int result = snprintf(state->dir, sizeof(state->dir), "%s", requested_dir);
        if (result < 0 || (size_t)result >= sizeof(state->dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        int result = snprintf(state->dir, sizeof(state->dir),
                              ".cubicle/controllers/%ld", (long)child_pid);
        if (result < 0 || (size_t)result >= sizeof(state->dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    if (mkdir_p(state->dir) < 0) {
        return -1;
    }

    int metadata_fd = open_state_file(state->dir, "metadata",
                                      O_WRONLY | O_CREAT | O_TRUNC);
    if (metadata_fd < 0) {
        return -1;
    }

    char command_line[512] = "";
    size_t used = 0;
    for (int i = 0; command[i] != NULL; ++i) {
        int result = snprintf(command_line + used, sizeof(command_line) - used,
                              "%s%s", i == 0 ? "" : " ", command[i]);
        if (result < 0 || (size_t)result >= sizeof(command_line) - used) {
            break;
        }
        used += (size_t)result;
    }

    char metadata[1024];
    int metadata_length = snprintf(metadata, sizeof(metadata),
                                   "mode=stream\npid=%ld\npgid=%ld\ncommand=%s\n",
                                   (long)child_pid, (long)child_pid,
                                   command_line);
    if (metadata_length < 0 || (size_t)metadata_length >= sizeof(metadata) ||
        write_all(metadata_fd, metadata, (size_t)metadata_length) < 0) {
        close(metadata_fd);
        return -1;
    }
    close(metadata_fd);

    state->events_fd = open_state_file(state->dir, "events.log",
                                       O_WRONLY | O_CREAT | O_TRUNC);
    state->stdout_fd = open_state_file(state->dir, "stdout.log",
                                       O_WRONLY | O_CREAT | O_TRUNC);
    state->stderr_fd = open_state_file(state->dir, "stderr.log",
                                       O_WRONLY | O_CREAT | O_TRUNC);

    if (state->events_fd < 0 || state->stdout_fd < 0 || state->stderr_fd < 0) {
        return -1;
    }

    char event[256];
    int event_length = snprintf(event, sizeof(event),
                                "type=process_started pid=%ld pgid=%ld mode=stream",
                                (long)child_pid, (long)child_pid);
    if (event_length < 0 || (size_t)event_length >= sizeof(event)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return append_event(state, event);
}

static int create_pipe(int pipe_fds[2], const char *name)
{
    if (pipe(pipe_fds) == 0) {
        return 0;
    }

    char message[256];
    snprintf(message, sizeof(message), "failed to create %s pipe: %s", name,
             strerror(errno));
    cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
    return -1;
}

static int make_control_socket_path(char path[PATH_MAX],
                                    const char *requested_socket,
                                    const controller_state_t *state)
{
    if (requested_socket != NULL) {
        int result = snprintf(path, PATH_MAX, "%s", requested_socket);
        if (result < 0 || result >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }

        return 0;
    }

    return make_state_file_path(path, state->dir, "control.sock");
}

static int open_control_socket(const char *path)
{
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    unlink(path);
    if (set_nonblocking(fd) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int write_error_response(int fd, const char *message)
{
    char response[256];
    int length = snprintf(response, sizeof(response), "error %s\n", message);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return write_all(fd, response, (size_t)length);
}

static void initialize_control_clients(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS])
{
    for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
        clients[i].fd = -1;
        clients[i].kind = CONTROL_CLIENT_EMPTY;
        clients[i].request_length = 0;
    }
}

static void close_control_client(control_client_t *client, controller_state_t *state)
{
    if (client->fd >= 0) {
        if (client->kind == CONTROL_CLIENT_ATTACHED_STDOUT) {
            append_event(state, "type=client_detached stream=stdout");
        } else if (client->kind == CONTROL_CLIENT_ATTACHED_STDERR) {
            append_event(state, "type=client_detached stream=stderr");
        }
        close(client->fd);
    }

    client->fd = -1;
    client->kind = CONTROL_CLIENT_EMPTY;
    client->request_length = 0;
}

static void close_all_control_clients(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS],
                                      controller_state_t *state)
{
    for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
        close_control_client(&clients[i], state);
    }
}

static const char *stream_file_name(const char *stream)
{
    if (strcmp(stream, "stdout") == 0 || strcmp(stream, "out") == 0) {
        return "stdout.log";
    }

    if (strcmp(stream, "stderr") == 0 || strcmp(stream, "err") == 0) {
        return "stderr.log";
    }

    return NULL;
}

static long long stream_available_offset(const controller_state_t *state,
                                         const char *stream)
{
    if (strcmp(stream, "stdout") == 0 || strcmp(stream, "out") == 0) {
        return state->stdout_offset;
    }

    if (strcmp(stream, "stderr") == 0 || strcmp(stream, "err") == 0) {
        return state->stderr_offset;
    }

    return -1;
}

static int read_stream_range(int client_fd, const controller_state_t *state,
                             const char *stream, long long start,
                             long long requested_length)
{
    if (start < 0 || requested_length < 0 || requested_length > 65536) {
        return write_error_response(client_fd, "invalid_range");
    }

    const char *file_name = stream_file_name(stream);
    long long available = stream_available_offset(state, stream);
    if (file_name == NULL || available < 0) {
        return write_error_response(client_fd, "unknown_stream");
    }

    if (start > available) {
        return write_error_response(client_fd, "range_past_end");
    }

    long long length = requested_length;
    if (start + length > available) {
        length = available - start;
    }

    char path[PATH_MAX];
    if (make_state_file_path(path, state->dir, file_name) < 0) {
        return write_error_response(client_fd, "state_path_too_long");
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return write_error_response(client_fd, "open_failed");
    }

    char header[64];
    int header_length = snprintf(header, sizeof(header), "ok length=%lld\n", length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        write_all(client_fd, header, (size_t)header_length) < 0) {
        close(fd);
        return -1;
    }

    if (lseek(fd, (off_t)start, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }

    char buffer[4096];
    long long remaining = length;
    while (remaining > 0) {
        size_t chunk = remaining > (long long)sizeof(buffer)
                           ? sizeof(buffer)
                           : (size_t)remaining;
        ssize_t read_result = read(fd, buffer, chunk);
        if (read_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(fd);
            return -1;
        }

        if (read_result == 0) {
            break;
        }

        if (write_all(client_fd, buffer, (size_t)read_result) < 0) {
            close(fd);
            return -1;
        }

        remaining -= read_result;
    }

    close(fd);
    return 0;
}

static int write_stream_bytes(int client_fd, const controller_state_t *state,
                              const char *stream, long long start,
                              long long length)
{
    const char *file_name = stream_file_name(stream);
    if (file_name == NULL) {
        return -1;
    }

    char path[PATH_MAX];
    if (make_state_file_path(path, state->dir, file_name) < 0) {
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    if (lseek(fd, (off_t)start, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }

    char buffer[4096];
    long long remaining = length;
    while (remaining > 0) {
        size_t chunk = remaining > (long long)sizeof(buffer)
                           ? sizeof(buffer)
                           : (size_t)remaining;
        ssize_t read_result = read(fd, buffer, chunk);
        if (read_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(fd);
            return -1;
        }

        if (read_result == 0) {
            break;
        }

        if (write_all(client_fd, buffer, (size_t)read_result) < 0) {
            close(fd);
            return -1;
        }

        remaining -= read_result;
    }

    close(fd);
    return 0;
}

static int send_attach_catchup(int client_fd, const controller_state_t *state,
                               const char *stream, long long start)
{
    long long available = stream_available_offset(state, stream);
    if (available < 0) {
        return write_error_response(client_fd, "unknown_stream");
    }

    if (start < 0 || start > available) {
        return write_error_response(client_fd, "invalid_attach_start");
    }

    long long length = available - start;
    char header[96];
    int header_length = snprintf(header, sizeof(header),
                                 "ok attached stream=%s start=%lld length=%lld\n",
                                 stream, start, length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        write_all(client_fd, header, (size_t)header_length) < 0) {
        return -1;
    }

    return write_stream_bytes(client_fd, state, stream, start, length);
}

static int dispatch_control_request(control_client_t *client,
                                    controller_state_t *state,
                                    pid_t child_pid)
{
    char *request = client->request;
    int result = 0;
    if (strcmp(request, "status") == 0) {
        char response[256];
        int length = snprintf(response, sizeof(response),
                              "ok state=running pid=%ld pgid=%ld stdout_offset=%lld stderr_offset=%lld\n",
                              (long)child_pid, (long)child_pid,
                              state->stdout_offset, state->stderr_offset);
        if (length < 0 || (size_t)length >= sizeof(response)) {
            result = -1;
        } else {
            result = write_all(client->fd, response, (size_t)length);
        }
    } else if (strncmp(request, "read ", 5) == 0) {
        char stream[16];
        long long start = 0;
        long long length = 0;
        if (sscanf(request + 5, "%15s %lld %lld", stream, &start, &length) == 3) {
            result = read_stream_range(client->fd, state, stream, start, length);
        } else {
            result = write_error_response(client->fd, "bad_read_command");
        }
    } else if (strncmp(request, "attach ", 7) == 0) {
        char stream[16];
        long long start = 0;
        if (sscanf(request + 7, "%15s %lld", stream, &start) == 2) {
            const char *file_name = stream_file_name(stream);
            if (file_name == NULL) {
                result = write_error_response(client->fd, "unknown_stream");
            } else {
                result = send_attach_catchup(client->fd, state, stream, start);
                if (result == 0) {
                    if (strcmp(file_name, "stdout.log") == 0) {
                        client->kind = CONTROL_CLIENT_ATTACHED_STDOUT;
                        append_event(state, "type=client_attached stream=stdout");
                    } else {
                        client->kind = CONTROL_CLIENT_ATTACHED_STDERR;
                        append_event(state, "type=client_attached stream=stderr");
                    }
                    return 0;
                }
            }
        } else {
            result = write_error_response(client->fd, "bad_attach_command");
        }
    } else if (strcmp(request, "terminate") == 0) {
        if (kill(-child_pid, SIGTERM) == 0) {
            append_event(state, "type=signal_delivered signal=15");
            result = write_all(client->fd, "ok\n", 3);
        } else {
            result = write_error_response(client->fd, "signal_failed");
        }
    } else if (strncmp(request, "signal ", 7) == 0) {
        int signal_number = 0;
        if (sscanf(request + 7, "%d", &signal_number) != 1 ||
            signal_number <= 0 || signal_number >= CUBICLE_MAX_SIGNAL_NUMBER) {
            result = write_error_response(client->fd, "bad_signal");
        } else if (kill(-child_pid, signal_number) == 0) {
            char event[128];
            int event_length = snprintf(event, sizeof(event),
                                        "type=signal_delivered signal=%d",
                                        signal_number);
            if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
                append_event(state, event);
            }
            result = write_all(client->fd, "ok\n", 3);
        } else {
            result = write_error_response(client->fd, "signal_failed");
        }
    } else {
        result = write_error_response(client->fd, "unknown_command");
    }

    close_control_client(client, state);
    return result < 0 ? 0 : result;
}

static int accept_control_clients(int listen_fd,
                                  control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS],
                                  controller_state_t *state)
{
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }

            return -1;
        }

        size_t slot = CUBICLE_MAX_CONTROL_CLIENTS;
        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            if (clients[i].kind == CONTROL_CLIENT_EMPTY) {
                slot = i;
                break;
            }
        }

        if (slot == CUBICLE_MAX_CONTROL_CLIENTS) {
            write_error_response(client_fd, "too_many_clients");
            close(client_fd);
            continue;
        }

        if (set_nonblocking(client_fd) < 0) {
            close(client_fd);
            continue;
        }

        clients[slot].fd = client_fd;
        clients[slot].kind = CONTROL_CLIENT_READING;
        clients[slot].request_length = 0;
        clients[slot].request[0] = '\0';
        (void)state;
    }
}

static int read_control_client_request(control_client_t *client,
                                       controller_state_t *state,
                                       pid_t child_pid)
{
    for (;;) {
        char buffer[128];
        ssize_t result = read(client->fd, buffer, sizeof(buffer));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }

            close_control_client(client, state);
            return 0;
        }

        if (result == 0) {
            if (client->request_length == 0) {
                close_control_client(client, state);
                return 0;
            }

            client->request[client->request_length] = '\0';
            dispatch_control_request(client, state, child_pid);
            return 0;
        }

        for (ssize_t i = 0; i < result; ++i) {
            if (buffer[i] == '\n') {
                client->request[client->request_length] = '\0';
                dispatch_control_request(client, state, child_pid);
                return 0;
            }

            if (client->request_length + 1 >= sizeof(client->request)) {
                write_error_response(client->fd, "request_too_long");
                close_control_client(client, state);
                return 0;
            }

            client->request[client->request_length++] = buffer[i];
        }
    }
}

static void broadcast_attached_output(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS],
                                      controller_state_t *state,
                                      const char *stream,
                                      const char *buffer,
                                      size_t length)
{
    control_client_kind_t kind = strcmp(stream, "stdout") == 0
                                     ? CONTROL_CLIENT_ATTACHED_STDOUT
                                     : CONTROL_CLIENT_ATTACHED_STDERR;

    for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
        if (clients[i].kind != kind) {
            continue;
        }

        if (write_best_effort(clients[i].fd, buffer, length) < 0) {
            close_control_client(&clients[i], state);
        }
    }
}

static int stream_event_loop(stream_pipe_t pipes[2],
                             controller_state_t *state,
                             int control_fd,
                             pid_t child_pid)
{
    control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS];
    initialize_control_clients(clients);

    int open_count = 0;

    for (size_t i = 0; i < 2; ++i) {
        if (pipes[i].open) {
            ++open_count;
        }
    }

    while (open_count > 0) {
        struct pollfd poll_fds[3 + CUBICLE_MAX_CONTROL_CLIENTS];
        nfds_t poll_count = 0;
        nfds_t control_index = (nfds_t)-1;
        nfds_t client_indexes[CUBICLE_MAX_CONTROL_CLIENTS];

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            client_indexes[i] = (nfds_t)-1;
        }

        if (control_fd >= 0) {
            control_index = poll_count;
            poll_fds[poll_count].fd = control_fd;
            poll_fds[poll_count].events = POLLIN;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        nfds_t pipe_indexes[2] = {(nfds_t)-1, (nfds_t)-1};
        for (size_t i = 0; i < 2; ++i) {
            if (!pipes[i].open) {
                continue;
            }

            pipe_indexes[i] = poll_count;
            poll_fds[poll_count].fd = pipes[i].fd;
            poll_fds[poll_count].events = POLLIN | POLLHUP | POLLERR;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            if (clients[i].kind == CONTROL_CLIENT_EMPTY) {
                continue;
            }

            client_indexes[i] = poll_count;
            poll_fds[poll_count].fd = clients[i].fd;
            poll_fds[poll_count].events = POLLHUP | POLLERR;
            if (clients[i].kind == CONTROL_CLIENT_READING) {
                poll_fds[poll_count].events |= POLLIN;
            }
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        int poll_result = poll(poll_fds, poll_count, -1);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
            return -1;
        }

        if (control_index != (nfds_t)-1 &&
            (poll_fds[control_index].revents & POLLIN) != 0) {
            if (accept_control_clients(control_fd, clients, state) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed accepting control client");
                return -1;
            }
        }

        for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
            if (clients[i].kind == CONTROL_CLIENT_EMPTY ||
                client_indexes[i] == (nfds_t)-1) {
                continue;
            }

            short revents = poll_fds[client_indexes[i]].revents;
            if (clients[i].kind == CONTROL_CLIENT_READING &&
                (revents & POLLIN) != 0 &&
                read_control_client_request(&clients[i], state, child_pid) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed reading control request");
                return -1;
            }

            if (clients[i].kind != CONTROL_CLIENT_EMPTY &&
                (revents & (POLLHUP | POLLERR)) != 0) {
                close_control_client(&clients[i], state);
                continue;
            }
        }

        for (size_t i = 0; i < 2; ++i) {
            if (!pipes[i].open) {
                continue;
            }

            short revents = poll_fds[pipe_indexes[i]].revents;
            if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }

            char buffer[4096];
            ssize_t read_result = read(pipes[i].fd, buffer, sizeof(buffer));
            if (read_result < 0) {
                if (errno == EINTR) {
                    continue;
                }

                char message[256];
                snprintf(message, sizeof(message), "failed reading %s: %s",
                         pipes[i].name, strerror(errno));
                cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
                return -1;
            }

            if (read_result == 0) {
                close_if_open(&pipes[i].fd);
                pipes[i].open = 0;
                --open_count;
                continue;
            }

            if (write_all(pipes[i].output_fd, buffer, (size_t)read_result) < 0) {
                char message[256];
                snprintf(message, sizeof(message), "failed writing %s: %s",
                         pipes[i].name, strerror(errno));
                cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
                return -1;
            }

            long long start = *pipes[i].offset;
            if (write_all(pipes[i].log_fd, buffer, (size_t)read_result) < 0) {
                char message[256];
                snprintf(message, sizeof(message), "failed persisting %s: %s",
                         pipes[i].name, strerror(errno));
                cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
                return -1;
            }
            *pipes[i].offset += read_result;

            char event[256];
            int event_length = snprintf(event, sizeof(event),
                                        "type=output stream=%s start=%lld length=%zd",
                                        pipes[i].name, start, read_result);
            if (event_length < 0 || (size_t)event_length >= sizeof(event) ||
                append_event(state, event) < 0) {
                cubicle_log(CUBICLE_LOG_ERROR, "controller",
                            "failed appending output event");
                return -1;
            }

            broadcast_attached_output(clients, state, pipes[i].name, buffer,
                                      (size_t)read_result);
        }
    }

    close_all_control_clients(clients, state);
    return 0;
}

static int wait_for_child(pid_t child_pid, controller_state_t *state)
{
    int status = 0;

    for (;;) {
        if (waitpid(child_pid, &status, 0) >= 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return 1;
    }

    char event[256];
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        int event_length = snprintf(event, sizeof(event),
                                    "type=process_exited status=exited exit_code=%d",
                                    exit_code);
        if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
            append_event(state, event);
        }
        return exit_code;
    }

    if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        int event_length = snprintf(event, sizeof(event),
                                    "type=process_exited status=signaled signal=%d",
                                    signal_number);
        if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
            append_event(state, event);
        }
        return 128 + signal_number;
    }

    return 1;
}

static int run_stream(char **command, const char *state_dir,
                      const char *control_socket)
{
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (create_pipe(stdin_pipe, "stdin") < 0 ||
        create_pipe(stdout_pipe, "stdout") < 0 ||
        create_pipe(stderr_pipe, "stderr") < 0) {
        close_if_open(&stdin_pipe[0]);
        close_if_open(&stdin_pipe[1]);
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stdout_pipe[1]);
        close_if_open(&stderr_pipe[0]);
        close_if_open(&stderr_pipe[1]);
        return 1;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return 1;
    }

    if (child_pid == 0) {
        setpgid(0, 0);

        close_if_open(&stdin_pipe[1]);
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stderr_pipe[0]);

        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }

        close_if_open(&stdin_pipe[0]);
        close_if_open(&stdout_pipe[1]);
        close_if_open(&stderr_pipe[1]);

        execvp(command[0], command);
        _exit(errno == ENOENT ? 127 : 126);
    }

    setpgid(child_pid, child_pid);

    close_if_open(&stdin_pipe[0]);
    close_if_open(&stdout_pipe[1]);
    close_if_open(&stderr_pipe[1]);

    /* No attachment path exists yet, so the child sees EOF on stdin. */
    close_if_open(&stdin_pipe[1]);

    char message[256];
    snprintf(message, sizeof(message), "started pid %ld in stream mode",
             (long)child_pid);
    cubicle_log(CUBICLE_LOG_INFO, "controller", message);

    controller_state_t state;
    if (initialize_controller_state(&state, state_dir, child_pid, command) < 0) {
        snprintf(message, sizeof(message), "failed to initialize state: %s",
                 strerror(errno));
        cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
        kill(-child_pid, SIGTERM);
        wait_for_child(child_pid, &state);
        close_if_open(&stdout_pipe[0]);
        close_if_open(&stderr_pipe[0]);
        close_controller_state(&state);
        return 1;
    }

    cubicle_log(CUBICLE_LOG_INFO, "controller", "state directory initialized");

    char control_socket_path[PATH_MAX];
    int control_fd = -1;
    if (make_control_socket_path(control_socket_path, control_socket, &state) < 0 ||
        (control_fd = open_control_socket(control_socket_path)) < 0) {
        snprintf(message, sizeof(message), "failed to initialize control socket: %s",
                 strerror(errno));
        cubicle_log(CUBICLE_LOG_ERROR, "controller", message);
        kill(-child_pid, SIGTERM);
        wait_for_child(child_pid, &state);
        close_controller_state(&state);
        return 1;
    }
    cubicle_log(CUBICLE_LOG_INFO, "controller", "control socket initialized");

    stream_pipe_t pipes[2] = {
        {.fd = stdout_pipe[0],
         .output_fd = STDOUT_FILENO,
         .log_fd = state.stdout_fd,
         .name = "stdout",
         .offset = &state.stdout_offset,
         .open = 1},
        {.fd = stderr_pipe[0],
         .output_fd = STDERR_FILENO,
         .log_fd = state.stderr_fd,
         .name = "stderr",
         .offset = &state.stderr_offset,
         .open = 1},
    };

    int output_result = stream_event_loop(pipes, &state, control_fd, child_pid);
    close_if_open(&pipes[0].fd);
    close_if_open(&pipes[1].fd);
    close_if_open(&control_fd);
    unlink(control_socket_path);

    int child_result = wait_for_child(child_pid, &state);
    close_controller_state(&state);
    if (output_result < 0 && child_result == 0) {
        return 1;
    }

    return child_result;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    const char *mode = NULL;
    const char *state_dir = NULL;
    const char *control_socket = NULL;
    int daemon = 0;
    int command_index = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "--daemon") == 0) {
            daemon = 1;
            continue;
        }

        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            state_dir = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--control-socket") == 0 && i + 1 < argc) {
            control_socket = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--") == 0 && i + 1 < argc) {
            command_index = i + 1;
            break;
        }
    }

    if (mode == NULL || command_index < 0) {
        print_usage(argv[0]);
        return 2;
    }

    cubicle_process_mode_t process_mode = CUBICLE_PROCESS_STREAM;
    if (parse_mode(mode, &process_mode) < 0) {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return 2;
    }

    if (process_mode != CUBICLE_PROCESS_STREAM) {
        fprintf(stderr, "%s mode is parsed but not implemented yet\n",
                cubicle_process_mode_name(process_mode));
        return 3;
    }

    if (daemon && daemonize_controller() < 0) {
        cubicle_log(CUBICLE_LOG_ERROR, "controller", strerror(errno));
        return 1;
    }

    return run_stream(&argv[command_index], state_dir, control_socket);
}
