#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include "../common/json.h"
#include "../common/rpc_internal.h"

#include "cubicle/attachment.h"
#include "cubicle/rpc.h"
#include "cubicle/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

    if (set_cloexec(fd) < 0 ||
        set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
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
    if (length > CUBICLE_RESPONSE_MAX ||
        client->response_length > CUBICLE_RESPONSE_MAX - length) {
        errno = ENOBUFS;
        return -1;
    }

    size_t required = client->response_length + length;
    if (required > client->response_capacity) {
        size_t capacity = client->response_capacity == 0 ? 4096
                                                         : client->response_capacity;
        while (capacity < required) {
            if (capacity > CUBICLE_RESPONSE_MAX / 2) {
                capacity = CUBICLE_RESPONSE_MAX;
            } else {
                capacity *= 2;
            }
        }
        char *response = realloc(client->response, capacity);
        if (response == NULL) {
            return -1;
        }
        client->response = response;
        client->response_capacity = capacity;
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

static int enqueue_api_frame(control_client_t *client, const char *json)
{
    size_t length = strlen(json);
    if (length > UINT32_MAX ||
        length > CUBICLE_RESPONSE_MAX - sizeof(uint32_t)) {
        errno = ENOBUFS;
        return -1;
    }

    uint32_t length_network = htonl((uint32_t)length);
    client->kind = CONTROL_CLIENT_RESPONDING;
    return enqueue_response(client, (const char *)&length_network,
                            sizeof(length_network)) == 0 &&
                   enqueue_response(client, json, length) == 0
               ? 0
               : -1;
}

static int enqueue_api_error(control_client_t *client,
                             const char *request_id,
                             cubicle_error_code_t code,
                             const char *message,
                             int retryable,
                             int system_errno)
{
    char response[2048];
    if (cubicle_rpc_error(response, sizeof(response), request_id, code,
                          message, retryable, system_errno) < 0) {
        return -1;
    }
    return enqueue_api_frame(client, response);
}

static int enqueue_api_success(control_client_t *client,
                               const char *request_id,
                               const char *result)
{
    size_t response_size = strlen(request_id) + strlen(result) + 128;
    char *response = malloc(response_size);
    if (response == NULL) {
        return -1;
    }
    if (cubicle_rpc_success(response, response_size, request_id,
                            result) < 0) {
        free(response);
        return -1;
    }
    int enqueue_result = enqueue_api_frame(client, response);
    free(response);
    return enqueue_result;
}

void initialize_control_clients(control_client_t clients[CUBICLE_MAX_CONTROL_CLIENTS])
{
    for (size_t i = 0; i < CUBICLE_MAX_CONTROL_CLIENTS; ++i) {
        clients[i].fd = -1;
        clients[i].kind = CONTROL_CLIENT_EMPTY;
        clients[i].request_length = 0;
        clients[i].framed_request = 0;
        clients[i].framed_length = 0;
        clients[i].response_length = 0;
        clients[i].response_offset = 0;
        clients[i].response_capacity = 0;
        clients[i].response = NULL;
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
    client->framed_request = 0;
    client->framed_length = 0;
    client->response_length = 0;
    client->response_offset = 0;
    client->response_capacity = 0;
    free(client->response);
    client->response = NULL;
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

    if (strcmp(stream, "tty") == 0) {
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

    if (strcmp(stream, "tty") == 0) {
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
    if (make_log_file_path(path, state, file_name) < 0) {
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
    if (make_log_file_path(path, state, file_name) < 0) {
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
    if (make_log_file_path(path, state, file_name) < 0) {
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
    if (make_log_file_path(path, state, "events.log") < 0) {
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

static int read_stream_json(const controller_state_t *state,
                            const char *stream,
                            uint64_t offset,
                            uint64_t maximum_length,
                            char *result,
                            size_t result_size)
{
    if (maximum_length > 8192) {
        errno = EMSGSIZE;
        return -1;
    }

    const char *file_name = stream_file_name(stream);
    long long available_offset = stream_available_offset(state, stream);
    if (file_name == NULL || available_offset < 0 ||
        offset > (uint64_t)available_offset) {
        errno = EINVAL;
        return -1;
    }

    char path[PATH_MAX];
    if (make_log_file_path(path, state, file_name) < 0) {
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    uint64_t available = (uint64_t)available_offset - offset;
    size_t read_length = available < maximum_length ? (size_t)available
                                                    : (size_t)maximum_length;
    char data[8193];
    size_t total = 0;
    while (total < read_length) {
        ssize_t nread = pread(fd, data + total, read_length - total,
                              (off_t)(offset + total));
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (nread == 0) {
            break;
        }
        total += (size_t)nread;
    }
    close(fd);
    data[total] = '\0';

    char escaped_data[65536];
    if (cubicle_json_escape(escaped_data, sizeof(escaped_data), data) < 0) {
        return -1;
    }

    int length = snprintf(result, result_size,
                          "{\"start_offset\":%llu,\"next_offset\":%llu,\"end_of_stream\":%s,\"data\":\"%s\",\"length\":%zu}",
                          (unsigned long long)offset,
                          (unsigned long long)(offset + total),
                          offset + total >= (uint64_t)available_offset
                              ? "true"
                              : "false",
                          escaped_data, total);
    if (length < 0 || (size_t)length >= result_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static int append_json_text(char *buffer, size_t buffer_size, size_t *used,
                            const char *text)
{
    char escaped[512];
    if (cubicle_json_escape(escaped, sizeof(escaped), text) < 0) {
        return -1;
    }
    int length = snprintf(buffer + *used, buffer_size - *used, "\"%s\"",
                          escaped);
    if (length < 0 || (size_t)length >= buffer_size - *used) {
        errno = ENOSPC;
        return -1;
    }
    *used += (size_t)length;
    return 0;
}

static int append_snapshot_json_cell(char *buffer,
                                     size_t buffer_size,
                                     size_t *used,
                                     const cubicle_terminal_cell_t *cell)
{
    int length = snprintf(buffer + *used, buffer_size - *used, "{\"t\":");
    if (length < 0 || (size_t)length >= buffer_size - *used) {
        errno = ENOSPC;
        return -1;
    }
    *used += (size_t)length;
    if (append_json_text(buffer, buffer_size, used, cell->text) < 0) {
        return -1;
    }
    length = snprintf(buffer + *used, buffer_size - *used, ",\"sgr\":");
    if (length < 0 || (size_t)length >= buffer_size - *used) {
        errno = ENOSPC;
        return -1;
    }
    *used += (size_t)length;
    if (append_json_text(buffer, buffer_size, used, cell->sgr) < 0) {
        return -1;
    }
    length = snprintf(buffer + *used, buffer_size - *used, "}");
    if (length < 0 || (size_t)length >= buffer_size - *used) {
        errno = ENOSPC;
        return -1;
    }
    *used += (size_t)length;
    return 0;
}

static int snapshot_json(cubicle_terminal_model_t *terminal_model,
                         uint64_t offset,
                         char *result,
                         size_t result_size)
{
    cubicle_terminal_snapshot_t snapshot;
    if (cubicle_terminal_model_snapshot(terminal_model, offset, &snapshot) < 0) {
        return -1;
    }

    int length = snprintf(
        result, result_size,
        "{\"rows\":%u,\"columns\":%u,\"cursor_row\":%u,\"cursor_column\":%u,\"cursor_visible\":%s,\"offset\":%llu,\"cells\":[",
        snapshot.rows, snapshot.cols, snapshot.cursor_row,
        snapshot.cursor_col, snapshot.cursor_visible ? "true" : "false",
        (unsigned long long)snapshot.offset);
    if (length < 0 || (size_t)length >= result_size) {
        cubicle_terminal_snapshot_cleanup(&snapshot);
        errno = ENOSPC;
        return -1;
    }
    size_t used = (size_t)length;

    size_t cell_count = (size_t)snapshot.rows * (size_t)snapshot.cols;
    for (size_t i = 0; i < cell_count; ++i) {
        if (i > 0) {
            if (used + 1 >= result_size) {
                cubicle_terminal_snapshot_cleanup(&snapshot);
                errno = ENOSPC;
                return -1;
            }
            result[used++] = ',';
            result[used] = '\0';
        }
        if (append_snapshot_json_cell(result, result_size, &used,
                                      &snapshot.cells[i]) < 0) {
            cubicle_terminal_snapshot_cleanup(&snapshot);
            return -1;
        }
    }

    if (used + 2 >= result_size) {
        cubicle_terminal_snapshot_cleanup(&snapshot);
        errno = ENOSPC;
        return -1;
    }
    result[used++] = ']';
    result[used++] = '}';
    result[used] = '\0';
    cubicle_terminal_snapshot_cleanup(&snapshot);
    return 0;
}

static int dispatch_api_request(control_client_t *client,
                                controller_state_t *state,
                                pid_t child_pid,
                                int child_stdin_fd,
                                int resize_fd,
                                int stderr_resize_fd,
                                terminal_size_state_t *terminal_size,
                                cubicle_terminal_model_t *terminal_model,
                                int process_completed,
                                int child_result)
{
    cubicle_rpc_request_envelope_t envelope;
    char request_id[64] = "";
    if (cubicle_rpc_decode_request(&envelope, client->request) < 0) {
        return enqueue_api_error(client, request_id, CUBICLE_ERR_PROTOCOL,
                                 "invalid request envelope", false, 0);
    }
    snprintf(request_id, sizeof(request_id), "%s", envelope.request_id);
    yyjson_val *params = envelope.params;

#define CONTROLLER_API_RETURN(expression) do { \
        int cubicle_controller_result__ = (expression); \
        cubicle_rpc_request_envelope_cleanup(&envelope); \
        return cubicle_controller_result__; \
    } while (0)

    if (strcmp(envelope.method, "controller.status") == 0) {
        char result[512];
        int length;
        if (process_completed) {
            length = snprintf(
                result, sizeof(result),
                "{\"state\":\"completed\",\"pid\":%ld,\"pgid\":%ld,\"result\":%d,\"stdout_offset\":%lld,\"stderr_offset\":%lld,\"tty_offset\":%lld}",
                (long)child_pid, (long)child_pid, child_result,
                state->stdout_offset, state->stderr_offset,
                state->stdout_offset);
        } else {
            length = snprintf(
                result, sizeof(result),
                "{\"state\":\"running\",\"pid\":%ld,\"pgid\":%ld,\"stdout_offset\":%lld,\"stderr_offset\":%lld,\"tty_offset\":%lld}",
                (long)child_pid, (long)child_pid, state->stdout_offset,
                state->stderr_offset, state->stdout_offset);
        }
        if (length < 0 || (size_t)length >= sizeof(result)) {
            CONTROLLER_API_RETURN(-1);
        }
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, result));
    }

    if (strcmp(envelope.method, "controller.read") == 0 ||
        strcmp(envelope.method, "controller.read_output") == 0) {
        char stream[32];
        uint64_t offset = 0;
        uint64_t maximum_length = 0;
        cubicle_validation_error_t error;
        if (cubicle_json_get_required_string(params, "stream", stream,
                                             sizeof(stream), &error) < 0 ||
            cubicle_json_get_required_u64(params, "offset", &offset,
                                          &error) < 0 ||
            cubicle_json_get_required_u64(params, "maximum_length",
                                          &maximum_length, &error) < 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_ARGUMENT,
                "invalid read request", false, 0));
        }
        char result[131072];
        if (read_stream_json(state, stream, offset, maximum_length, result,
                             sizeof(result)) < 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_ARGUMENT,
                "read failed", false, errno));
        }
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, result));
    }

    if (strcmp(envelope.method, "controller.snapshot") == 0) {
        if (terminal_model == NULL) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_STATE,
                "terminal snapshot is unavailable", false, 0));
        }
        char *result = malloc(CUBICLE_RESPONSE_MAX);
        if (result == NULL) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INTERNAL,
                "snapshot allocation failed", false, ENOMEM));
        }
        if (snapshot_json(terminal_model, (uint64_t)state->stdout_offset,
                          result, CUBICLE_RESPONSE_MAX) < 0) {
            free(result);
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                "snapshot failed", false, errno));
        }
        int snapshot_result = enqueue_api_success(client, request_id, result);
        free(result);
        CONTROLLER_API_RETURN(snapshot_result);
    }

    if (strcmp(envelope.method, "controller.write") == 0) {
        char data[4096];
        cubicle_validation_error_t error;
        if (process_completed) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_STATE,
                "process completed", false, 0));
        }
        if (child_stdin_fd < 0 ||
            cubicle_json_get_required_string(params, "data", data,
                                             sizeof(data), &error) < 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_ARGUMENT,
                "invalid write request", false, 0));
        }
        if (write_best_effort(child_stdin_fd, data, strlen(data)) < 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_IO, "write failed", true,
                errno));
        }
        char event[128];
        int event_length = snprintf(event, sizeof(event),
                                    "type=input length=%zu", strlen(data));
        if (event_length >= 0 && (size_t)event_length < sizeof(event)) {
            append_event(state, event);
        }
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, "{}"));
    }

    if (strcmp(envelope.method, "controller.resize") == 0) {
        uint64_t rows = 0;
        uint64_t columns = 0;
        cubicle_validation_error_t error;
        if (process_completed) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_STATE,
                "process completed", false, 0));
        }
        if (resize_fd < 0 ||
            cubicle_json_get_required_u64(params, "rows", &rows, &error) < 0 ||
            cubicle_json_get_required_u64(params, "columns", &columns,
                                          &error) < 0 ||
            rows == 0 || rows > UINT16_MAX ||
            columns == 0 || columns > UINT16_MAX) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_ARGUMENT,
                "invalid resize request", false, 0));
        }
        if (terminal_size != NULL && terminal_size->known &&
            terminal_size->rows == (unsigned short)rows &&
            terminal_size->columns == (unsigned short)columns) {
            CONTROLLER_API_RETURN(enqueue_api_success(client, request_id,
                                                      "{}"));
        }
        struct winsize size;
        memset(&size, 0, sizeof(size));
        size.ws_row = (unsigned short)rows;
        size.ws_col = (unsigned short)columns;
        if (ioctl(resize_fd, TIOCSWINSZ, &size) < 0 ||
            (stderr_resize_fd >= 0 &&
             ioctl(stderr_resize_fd, TIOCSWINSZ, &size) < 0)) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_IO, "resize failed", true,
                errno));
        }
        if (terminal_model != NULL &&
            cubicle_terminal_model_resize(terminal_model, (unsigned int)rows,
                                          (unsigned int)columns) < 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_IO, "resize failed", true,
                errno));
        }
        if (terminal_size != NULL) {
            terminal_size->rows = (unsigned short)rows;
            terminal_size->columns = (unsigned short)columns;
            terminal_size->known = 1;
        }
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, "{}"));
    }

    if (strcmp(envelope.method, "controller.attach") == 0) {
        char token[CUBICLE_TOKEN_MAX];
        uint64_t channels = 0;
        char mode[32];
        cubicle_validation_error_t error;
        if (cubicle_json_get_required_string(params, "token", token,
                                             sizeof(token), &error) < 0 ||
            strncmp(token, "local:", 6) != 0 ||
            cubicle_json_get_required_u64(params, "channels", &channels,
                                          &error) < 0 ||
            cubicle_json_get_required_string(params, "mode", mode,
                                             sizeof(mode), &error) < 0 ||
            channels == 0 ||
            (channels & ~(uint64_t)(CUBICLE_CHANNEL_STDIN |
                                    CUBICLE_CHANNEL_STDOUT |
                                    CUBICLE_CHANNEL_STDERR |
                                    CUBICLE_CHANNEL_TTY)) != 0 ||
            (strcmp(mode, "observer") != 0 &&
             strcmp(mode, "interactive") != 0)) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_PERMISSION_DENIED,
                "invalid attachment token", false, 0));
        }
        if (strcmp(mode, "observer") == 0) {
            channels &= ~(uint64_t)CUBICLE_CHANNEL_STDIN;
        }
        if ((channels & CUBICLE_CHANNEL_STDIN) != 0 &&
            (process_completed || child_stdin_fd < 0)) {
            channels &= ~(uint64_t)CUBICLE_CHANNEL_STDIN;
        }
        if (channels == 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_PERMISSION_DENIED,
                "no attachment channels accepted", false, 0));
        }
        if ((channels & CUBICLE_CHANNEL_STDOUT) != 0) {
            append_event(state, "type=client_attached stream=stdout");
        }
        if ((channels & CUBICLE_CHANNEL_STDERR) != 0) {
            append_event(state, "type=client_attached stream=stderr");
        }
        if ((channels & CUBICLE_CHANNEL_STDIN) != 0) {
            append_event(state, "type=client_attached stream=stdin");
        }
        if ((channels & CUBICLE_CHANNEL_TTY) != 0) {
            append_event(state, "type=client_attached stream=tty");
        }
        char result[512];
        int length = snprintf(
            result, sizeof(result),
            "{\"accepted_channels\":%llu,\"stdout_offset\":%lld,\"stderr_offset\":%lld,\"tty_offset\":%lld}",
            (unsigned long long)channels, state->stdout_offset,
            state->stderr_offset, state->stdout_offset);
        if (length < 0 || (size_t)length >= sizeof(result)) {
            CONTROLLER_API_RETURN(-1);
        }
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, result));
    }

    if (strcmp(envelope.method, "controller.detach") == 0) {
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, "{}"));
    }

    if (strcmp(envelope.method, "controller.signal") == 0) {
        uint64_t signal_number = 0;
        cubicle_validation_error_t error;
        if (process_completed ||
            cubicle_json_get_required_u64(params, "signal_number",
                                          &signal_number, &error) < 0 ||
            signal_number == 0 ||
            signal_number >= CUBICLE_MAX_SIGNAL_NUMBER) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_ARGUMENT,
                "invalid signal request", false, 0));
        }
        if (kill(-child_pid, (int)signal_number) < 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_IO, "signal failed", true,
                errno));
        }
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, "{}"));
    }

    if (strcmp(envelope.method, "controller.terminate") == 0) {
        if (process_completed) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_STATE,
                "process completed", false, 0));
        }
        if (kill(-child_pid, SIGTERM) < 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_IO, "terminate failed", true,
                errno));
        }
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, "{}"));
    }

    if (strcmp(envelope.method, "controller.events_after") == 0) {
        uint64_t after_sequence = 0;
        uint64_t limit = 0;
        cubicle_validation_error_t error;
        if (cubicle_json_get_required_u64(params, "after_sequence",
                                          &after_sequence, &error) < 0 ||
            cubicle_json_get_required_u64(params, "limit", &limit,
                                          &error) < 0 ||
            limit > 1024) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_INVALID_ARGUMENT,
                "invalid events request", false, 0));
        }
        char path[PATH_MAX];
        if (make_log_file_path(path, state, "events.log") < 0) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_IO, "events path failed",
                true, errno));
        }
        FILE *file = fopen(path, "r");
        char result[8192];
        size_t used = 0;
        int written = snprintf(result, sizeof(result), "{\"events\":[");
        if (written < 0 || (size_t)written >= sizeof(result)) {
            if (file != NULL) {
                fclose(file);
            }
            CONTROLLER_API_RETURN(-1);
        }
        used = (size_t)written;
        size_t count = 0;
        if (file != NULL) {
            char line[1024];
            while (fgets(line, sizeof(line), file) != NULL && count < limit) {
                long long sequence = -1;
                if (sscanf(line, "seq=%lld", &sequence) != 1 ||
                    sequence <= (long long)after_sequence) {
                    continue;
                }
                line[strcspn(line, "\n")] = '\0';
                char escaped[2048];
                if (cubicle_json_escape(escaped, sizeof(escaped), line) < 0) {
                    continue;
                }
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s{\"sequence\":%lld,\"payload\":\"%s\"}",
                                   count == 0 ? "" : ",", sequence, escaped);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    fclose(file);
                    CONTROLLER_API_RETURN(enqueue_api_error(
                        client, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                        "events response too large", false, 0));
                }
                used += (size_t)written;
                ++count;
            }
            fclose(file);
        }
        written = snprintf(result + used, sizeof(result) - used,
                           "],\"count\":%zu}", count);
        if (written < 0 || (size_t)written >= sizeof(result) - used) {
            CONTROLLER_API_RETURN(enqueue_api_error(
                client, request_id, CUBICLE_ERR_RESOURCE_LIMIT,
                "events response too large", false, 0));
        }
        CONTROLLER_API_RETURN(enqueue_api_success(client, request_id, result));
    }

    CONTROLLER_API_RETURN(enqueue_api_error(client, request_id,
                                           CUBICLE_ERR_UNSUPPORTED,
                                           "method is not implemented",
                                           false, 0));
#undef CONTROLLER_API_RETURN
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
                                    int stderr_resize_fd,
                                    terminal_size_state_t *terminal_size,
                                    cubicle_terminal_model_t *terminal_model,
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
            if (ioctl(resize_fd, TIOCSWINSZ, &size) < 0 ||
                (stderr_resize_fd >= 0 &&
                 ioctl(stderr_resize_fd, TIOCSWINSZ, &size) < 0)) {
                result = enqueue_error_response(client, "resize_failed");
            } else {
                if (terminal_model != NULL &&
                    cubicle_terminal_model_resize(terminal_model, rows,
                                                  columns) < 0) {
                    result = enqueue_error_response(client, "resize_failed");
                    goto finish;
                }
                if (terminal_size != NULL) {
                    terminal_size->rows = (unsigned short)rows;
                    terminal_size->columns = (unsigned short)columns;
                    terminal_size->known = 1;
                }
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

finish:
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
                                  int stderr_resize_fd,
                                  terminal_size_state_t *terminal_size,
                                  cubicle_terminal_model_t *terminal_model,
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
                             stderr_resize_fd,
                             terminal_size,
                             terminal_model,
                             process_completed, child_result);
    return 0;
}

static int finish_api_request(control_client_t *client,
                              controller_state_t *state,
                              pid_t child_pid,
                              int child_stdin_fd,
                              int resize_fd,
                              int stderr_resize_fd,
                              terminal_size_state_t *terminal_size,
                              cubicle_terminal_model_t *terminal_model,
                              int process_completed,
                              int child_result)
{
    memmove(client->request, client->request + sizeof(uint32_t),
            client->framed_length);
    client->request[client->framed_length] = '\0';
    client->request_length = client->framed_length;
    return dispatch_api_request(client, state, child_pid, child_stdin_fd,
                                resize_fd, stderr_resize_fd, terminal_size,
                                terminal_model, process_completed,
                                child_result);
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
        if (set_cloexec(client_fd) < 0) {
            close(client_fd);
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
        clients[slot].framed_request = 0;
        clients[slot].framed_length = 0;
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
                                int stderr_resize_fd,
                                terminal_size_state_t *terminal_size,
                                cubicle_terminal_model_t *terminal_model,
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
            if (client->framed_request) {
                close_control_client(client, state);
                return 0;
            }

            if (client->request_length == 0) {
                close_control_client(client, state);
                return 0;
            }

            client->request[client->request_length] = '\0';
            return finish_control_request(client, state, child_pid,
                                          child_stdin_fd, resize_fd,
                                          stderr_resize_fd,
                                          terminal_size,
                                          terminal_model,
                                          process_completed,
                                          child_result);
        }

        for (ssize_t i = 0; i < result; ++i) {
            if (client->request_length == 0 && buffer[i] == '\0') {
                client->framed_request = 1;
            }

            if (client->framed_request) {
                if (client->request_length + 1 >= sizeof(client->request)) {
                    if (enqueue_api_error(client, "",
                                          CUBICLE_ERR_RESOURCE_LIMIT,
                                          "request too large", false,
                                          0) < 0) {
                        close_control_client(client, state);
                    }
                    return 0;
                }
                client->request[client->request_length++] = buffer[i];

                if (client->request_length == sizeof(uint32_t)) {
                    uint32_t length_network = 0;
                    memcpy(&length_network, client->request,
                           sizeof(length_network));
                    client->framed_length = ntohl(length_network);
                    if (client->framed_length == 0 ||
                        client->framed_length >=
                            sizeof(client->request) - sizeof(uint32_t)) {
                        if (enqueue_api_error(client, "",
                                              CUBICLE_ERR_RESOURCE_LIMIT,
                                              "invalid frame length", false,
                                              0) < 0) {
                            close_control_client(client, state);
                        }
                        return 0;
                    }
                }

                if (client->framed_length > 0 &&
                    client->request_length ==
                        sizeof(uint32_t) + client->framed_length) {
                    return finish_api_request(client, state, child_pid,
                                              child_stdin_fd, resize_fd,
                                              stderr_resize_fd,
                                              terminal_size,
                                              terminal_model,
                                              process_completed,
                                              child_result);
                }
                continue;
            }

            if (buffer[i] == '\n') {
                return finish_control_request(client, state, child_pid,
                                              child_stdin_fd, resize_fd,
                                              stderr_resize_fd,
                                              terminal_size,
                                              terminal_model,
                                              process_completed,
                                              child_result);
            }

            if (buffer[i] == '\0') {
                if (enqueue_error_response(client, "bad_request") < 0) {
                    close_control_client(client, state);
                }
                return 0;
            }

            if (client->request_length + 1 >= CUBICLE_LINE_REQUEST_MAX) {
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
