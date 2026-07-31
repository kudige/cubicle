#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include "cubicle/util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int make_control_socket_path(char path[PATH_MAX],
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

static int is_live_unix_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    int saved_errno = errno;
    close(fd);

    if (result == 0) {
        return 1;
    }

    errno = saved_errno;
    return 0;
}

static int prepare_control_socket_path(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT) {
            return 0;
        }

        return -1;
    }

    if (!S_ISSOCK(st.st_mode)) {
        errno = EEXIST;
        return -1;
    }

    int live = is_live_unix_socket(path);
    if (live < 0) {
        return -1;
    }

    if (live) {
        errno = EADDRINUSE;
        return -1;
    }

    return unlink(path);
}

int open_control_socket(const char *path)
{
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (prepare_control_socket_path(path) < 0) {
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

    if (set_nonblocking(fd) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        chmod(path, 0600) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int enqueue_response(control_client_t *client, const char *buffer,
                            size_t length)
{
    if (client->response_length + length > sizeof(client->response)) {
        errno = ENOBUFS;
        return -1;
    }

    memcpy(client->response + client->response_length, buffer, length);
    client->response_length += length;
    return 0;
}

static int enqueue_error_response(control_client_t *client, const char *message)
{
    char response[256];
    int length = snprintf(response, sizeof(response), "error %s\n", message);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    client->kind = CONTROL_CLIENT_RESPONDING;
    return enqueue_response(client, response, (size_t)length);
}

void initialize_control_clients(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS])
{
    for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
        clients[i].fd = -1;
        clients[i].kind = CONTROL_CLIENT_EMPTY;
        clients[i].request_length = 0;
        clients[i].response_length = 0;
        clients[i].response_offset = 0;
    }
}

void close_control_client(control_client_t *client, controller_state_t *state)
{
    if (client->fd >= 0) {
        if (client->kind == CONTROL_CLIENT_ATTACHED_STDOUT) {
            append_event(state, "type=client_detached stream=stdout");
        } else if (client->kind == CONTROL_CLIENT_ATTACHED_STDERR) {
            append_event(state, "type=client_detached stream=stderr");
        } else if (client->kind == CONTROL_CLIENT_ATTACHED_STDIN) {
            append_event(state, "type=client_detached stream=stdin");
        }
        close(client->fd);
    }

    client->fd = -1;
    client->kind = CONTROL_CLIENT_EMPTY;
    client->request_length = 0;
    client->response_length = 0;
    client->response_offset = 0;
}

void close_all_control_clients(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS],
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

static int read_stream_range(control_client_t *client,
                             const controller_state_t *state,
                             const char *stream, long long start,
                             long long requested_length)
{
    if (start < 0 || requested_length < 0 || requested_length > 65536) {
        return enqueue_error_response(client, "invalid_range");
    }

    const char *file_name = stream_file_name(stream);
    long long available = stream_available_offset(state, stream);
    if (file_name == NULL || available < 0) {
        return enqueue_error_response(client, "unknown_stream");
    }

    if (start > available) {
        return enqueue_error_response(client, "range_past_end");
    }

    long long length = requested_length;
    if (start + length > available) {
        length = available - start;
    }

    char path[PATH_MAX];
    if (make_state_file_path(path, state->dir, file_name) < 0) {
        return enqueue_error_response(client, "state_path_too_long");
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return enqueue_error_response(client, "open_failed");
    }

    char header[64];
    int header_length = snprintf(header, sizeof(header), "ok length=%lld\n", length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        enqueue_response(client, header, (size_t)header_length) < 0) {
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

        if (enqueue_response(client, buffer, (size_t)read_result) < 0) {
            close(fd);
            return -1;
        }

        remaining -= read_result;
    }

    close(fd);
    client->kind = CONTROL_CLIENT_RESPONDING;
    return 0;
}

static int enqueue_stream_bytes(control_client_t *client,
                                const controller_state_t *state,
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

        if (enqueue_response(client, buffer, (size_t)read_result) < 0) {
            close(fd);
            return -1;
        }

        remaining -= read_result;
    }

    close(fd);
    return 0;
}

static int write_file_response(control_client_t *client,
                               const controller_state_t *state,
                               const char *file_name)
{
    char path[PATH_MAX];
    if (make_state_file_path(path, state->dir, file_name) < 0) {
        return enqueue_error_response(client, "state_path_too_long");
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return enqueue_error_response(client, "open_failed");
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0 || st.st_size > 65536) {
        close(fd);
        return enqueue_error_response(client, "read_too_large");
    }

    char header[64];
    int header_length = snprintf(header, sizeof(header), "ok length=%lld\n",
                                 (long long)st.st_size);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        enqueue_response(client, header, (size_t)header_length) < 0) {
        close(fd);
        return -1;
    }

    char buffer[4096];
    for (;;) {
        ssize_t read_result = read(fd, buffer, sizeof(buffer));
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

        if (enqueue_response(client, buffer, (size_t)read_result) < 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    client->kind = CONTROL_CLIENT_RESPONDING;
    return 0;
}

static int write_events_after_response(control_client_t *client,
                                       const controller_state_t *state,
                                       long long after_sequence,
                                       long long limit)
{
    if (after_sequence < 0 || limit < 0 || limit > 1024) {
        return enqueue_error_response(client, "invalid_event_range");
    }

    char path[PATH_MAX];
    if (make_state_file_path(path, state->dir, "events.log") < 0) {
        return enqueue_error_response(client, "state_path_too_long");
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return enqueue_error_response(client, "open_failed");
    }

    char payload[65536];
    size_t used = 0;
    long long count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), file) != NULL && count < limit) {
        long long sequence = -1;
        if (sscanf(line, "seq=%lld", &sequence) != 1 ||
            sequence <= after_sequence) {
            continue;
        }

        size_t line_length = strlen(line);
        if (line_length > sizeof(payload) - used) {
            break;
        }

        memcpy(payload + used, line, line_length);
        used += line_length;
        ++count;
    }

    fclose(file);

    char header[96];
    int header_length = snprintf(header, sizeof(header),
                                 "ok count=%lld length=%zu\n", count, used);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        enqueue_response(client, header, (size_t)header_length) < 0) {
        return -1;
    }

    if (enqueue_response(client, payload, used) < 0) {
        return -1;
    }

    client->kind = CONTROL_CLIENT_RESPONDING;
    return 0;
}

static int send_attach_catchup(control_client_t *client,
                               const controller_state_t *state,
                               const char *stream, long long start)
{
    long long available = stream_available_offset(state, stream);
    if (available < 0) {
        return enqueue_error_response(client, "unknown_stream") < 0
                   ? -1
                   : 1;
    }

    if (start < 0 || start > available) {
        return enqueue_error_response(client, "invalid_attach_start") < 0
                   ? -1
                   : 1;
    }

    long long length = available - start;
    if (length > 65536) {
        return enqueue_error_response(client, "read_too_large") < 0
                   ? -1
                   : 1;
    }

    char header[96];
    int header_length = snprintf(header, sizeof(header),
                                 "ok attached stream=%s start=%lld length=%lld\n",
                                 stream, start, length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        enqueue_response(client, header, (size_t)header_length) < 0) {
        return -1;
    }

    return enqueue_stream_bytes(client, state, stream, start, length);
}

static int dispatch_control_request(control_client_t *client,
                                    controller_state_t *state,
                                    pid_t child_pid,
                                    int child_stdin_fd,
                                    int resize_fd,
                                    terminal_size_state_t *terminal_size,
                                    int process_completed,
                                    int child_result)
{
    char *request = client->request;
    int result = 0;
    if (strcmp(request, "status") == 0) {
        char response[256];
        int length;
        if (process_completed) {
            length = snprintf(response, sizeof(response),
                              "ok state=completed pid=%ld pgid=%ld result=%d stdout_offset=%lld stderr_offset=%lld\n",
                              (long)child_pid, (long)child_pid, child_result,
                              state->stdout_offset, state->stderr_offset);
        } else {
            length = snprintf(response, sizeof(response),
                              "ok state=running pid=%ld pgid=%ld stdout_offset=%lld stderr_offset=%lld\n",
                              (long)child_pid, (long)child_pid,
                              state->stdout_offset, state->stderr_offset);
        }
        if (length < 0 || (size_t)length >= sizeof(response)) {
            result = -1;
        } else {
            client->kind = CONTROL_CLIENT_RESPONDING;
            result = enqueue_response(client, response, (size_t)length);
        }
    } else if (strcmp(request, "metadata") == 0) {
        result = write_file_response(client, state, "metadata");
    } else if (strncmp(request, "events after ", 13) == 0) {
        long long after_sequence = 0;
        long long limit = 0;
        if (sscanf(request + 13, "%lld %lld", &after_sequence, &limit) == 2) {
            result = write_events_after_response(client, state,
                                                 after_sequence, limit);
        } else {
            result = enqueue_error_response(client, "bad_events_command");
        }
    } else if (strncmp(request, "read ", 5) == 0) {
        char stream[16];
        long long start = 0;
        long long length = 0;
        if (sscanf(request + 5, "%15s %lld %lld", stream, &start, &length) == 3) {
            result = read_stream_range(client, state, stream, start, length);
        } else {
            result = enqueue_error_response(client, "bad_read_command");
        }
    } else if (strcmp(request, "attach stdin") == 0 ||
               strcmp(request, "attach in") == 0) {
        if (process_completed) {
            result = enqueue_error_response(client, "process_completed");
        } else if (child_stdin_fd < 0) {
            result = enqueue_error_response(client, "stdin_closed");
        } else {
            client->kind = CONTROL_CLIENT_ATTACHING_STDIN;
            result = enqueue_response(client, "ok attached stream=stdin\n", 25);
        }
    } else if (strncmp(request, "resize ", 7) == 0) {
        unsigned int rows = 0;
        unsigned int columns = 0;
        if (process_completed) {
            result = enqueue_error_response(client, "process_completed");
        } else if (resize_fd < 0) {
            result = enqueue_error_response(client, "resize_unsupported");
        } else if (sscanf(request + 7, "%u %u", &rows, &columns) != 2 ||
                   rows == 0 || columns == 0) {
            result = enqueue_error_response(client, "bad_resize");
        } else if (terminal_size != NULL && terminal_size->known &&
                   terminal_size->rows == (unsigned short)rows &&
                   terminal_size->columns == (unsigned short)columns) {
            client->kind = CONTROL_CLIENT_RESPONDING;
            result = enqueue_response(client, "ok\n", 3);
        } else {
            struct winsize size;
            memset(&size, 0, sizeof(size));
            size.ws_row = (unsigned short)rows;
            size.ws_col = (unsigned short)columns;
            if (ioctl(resize_fd, TIOCSWINSZ, &size) < 0) {
                result = enqueue_error_response(client, "resize_failed");
            } else {
                if (terminal_size != NULL) {
                    terminal_size->rows = (unsigned short)rows;
                    terminal_size->columns = (unsigned short)columns;
                    terminal_size->known = 1;
                }
                kill(-child_pid, SIGWINCH);
                char event[128];
                int event_length = snprintf(event, sizeof(event),
                                            "type=terminal_resized rows=%u columns=%u",
                                            rows, columns);
                if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
                    append_event(state, event);
                }
                client->kind = CONTROL_CLIENT_RESPONDING;
                result = enqueue_response(client, "ok\n", 3);
            }
        }
    } else if (strncmp(request, "attach ", 7) == 0) {
        char stream[16];
        long long start = 0;
        if (process_completed) {
            result = enqueue_error_response(client, "process_completed");
        } else if (sscanf(request + 7, "%15s %lld", stream, &start) == 2) {
            const char *file_name = stream_file_name(stream);
            if (file_name == NULL) {
                result = enqueue_error_response(client, "unknown_stream");
            } else {
                result = send_attach_catchup(client, state, stream, start);
                if (result == 0) {
                    if (strcmp(file_name, "stdout.log") == 0) {
                        client->kind = CONTROL_CLIENT_ATTACHED_STDOUT;
                        append_event(state, "type=client_attached stream=stdout");
                    } else {
                        client->kind = CONTROL_CLIENT_ATTACHED_STDERR;
                        append_event(state, "type=client_attached stream=stderr");
                    }
                    return 0;
                } else if (result > 0) {
                    result = 0;
                }
            }
        } else {
            result = enqueue_error_response(client, "bad_attach_command");
        }
    } else if (strcmp(request, "terminate") == 0) {
        if (process_completed) {
            result = enqueue_error_response(client, "process_completed");
        } else if (kill(-child_pid, SIGTERM) == 0) {
            append_event(state, "type=signal_delivered signal=15");
            client->kind = CONTROL_CLIENT_RESPONDING;
            result = enqueue_response(client, "ok\n", 3);
        } else {
            result = enqueue_error_response(client, "signal_failed");
        }
    } else if (strncmp(request, "signal ", 7) == 0) {
        int signal_number = 0;
        if (process_completed) {
            result = enqueue_error_response(client, "process_completed");
        } else if (sscanf(request + 7, "%d", &signal_number) != 1 ||
            signal_number <= 0 || signal_number >= CUBICLE_MAX_SIGNAL_NUMBER) {
            result = enqueue_error_response(client, "bad_signal");
        } else if (kill(-child_pid, signal_number) == 0) {
            char event[128];
            int event_length = snprintf(event, sizeof(event),
                                        "type=signal_delivered signal=%d",
                                        signal_number);
            if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
                append_event(state, event);
            }
            client->kind = CONTROL_CLIENT_RESPONDING;
            result = enqueue_response(client, "ok\n", 3);
        } else {
            result = enqueue_error_response(client, "signal_failed");
        }
    } else {
        result = enqueue_error_response(client, "unknown_command");
    }

    if (result < 0) {
        close_control_client(client, state);
    }

    return 0;
}

static int finish_control_request(control_client_t *client,
                                  controller_state_t *state,
                                  pid_t child_pid,
                                  int child_stdin_fd,
                                  int resize_fd,
                                  terminal_size_state_t *terminal_size,
                                  int process_completed,
                                  int child_result)
{
    if (client->request_length > 0 &&
        client->request[client->request_length - 1] == '\r') {
        --client->request_length;
    }

    client->request[client->request_length] = '\0';
    dispatch_control_request(client, state, child_pid, child_stdin_fd,
                             resize_fd,
                             terminal_size,
                             process_completed, child_result);
    return 0;
}

int flush_control_client_response(control_client_t *client,
                                  controller_state_t *state)
{
    while (client->response_offset < client->response_length) {
        ssize_t result = write(client->fd,
                               client->response + client->response_offset,
                               client->response_length - client->response_offset);
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

        client->response_offset += (size_t)result;
    }

    client->response_length = 0;
    client->response_offset = 0;

    if (client->kind == CONTROL_CLIENT_RESPONDING) {
        close_control_client(client, state);
    } else if (client->kind == CONTROL_CLIENT_ATTACHING_STDIN) {
        client->kind = CONTROL_CLIENT_ATTACHED_STDIN;
        append_event(state, "type=client_attached stream=stdin");
    }

    return 0;
}

int accept_control_clients(int listen_fd,
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
            const char response[] = "error too_many_clients\n";
            cubicle_write_all(client_fd, response, sizeof(response) - 1);
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
        clients[slot].response_length = 0;
        clients[slot].response_offset = 0;
        clients[slot].request[0] = '\0';
        (void)state;
    }
}

int read_control_client_request(control_client_t *client,
                                controller_state_t *state,
                                pid_t child_pid,
                                int child_stdin_fd,
                                int resize_fd,
                                terminal_size_state_t *terminal_size,
                                int process_completed,
                                int child_result)
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
            return finish_control_request(client, state, child_pid,
                                          child_stdin_fd, resize_fd,
                                          terminal_size,
                                          process_completed,
                                          child_result);
        }

        for (ssize_t i = 0; i < result; ++i) {
            if (buffer[i] == '\n') {
                return finish_control_request(client, state, child_pid,
                                              child_stdin_fd, resize_fd,
                                              terminal_size,
                                              process_completed,
                                              child_result);
            }

            if (buffer[i] == '\0') {
                if (enqueue_error_response(client, "bad_request") < 0) {
                    close_control_client(client, state);
                }
                return 0;
            }

            if (client->request_length + 1 >= sizeof(client->request)) {
                if (enqueue_error_response(client, "request_too_long") < 0) {
                    close_control_client(client, state);
                }
                return 0;
            }

            client->request[client->request_length++] = buffer[i];
        }
    }
}

void broadcast_attached_output(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS],
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

        if (enqueue_response(&clients[i], buffer, length) < 0) {
            close_control_client(&clients[i], state);
        }
    }
}

int forward_attached_stdin(control_client_t *client,
                                  controller_state_t *state,
                                  int child_stdin_fd)
{
    char buffer[4096];

    for (;;) {
        ssize_t read_result = read(client->fd, buffer, sizeof(buffer));
        if (read_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }

            close_control_client(client, state);
            return 0;
        }

        if (read_result == 0) {
            close_control_client(client, state);
            return 0;
        }

        if (write_best_effort(child_stdin_fd, buffer, (size_t)read_result) < 0) {
            close_control_client(client, state);
            return 0;
        }

        char event[128];
        int event_length = snprintf(event, sizeof(event),
                                    "type=input length=%zd", read_result);
        if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
            append_event(state, event);
        }
    }
}
