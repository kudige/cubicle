#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_FRAME (16U * 1024U * 1024U)

static int read_all(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = read(fd, cursor, length);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return -1;
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t result = write(fd, cursor, length);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int json_string_field(const char *json, const char *key,
                             char *buffer, size_t buffer_size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (start == NULL) return -1;
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (end == NULL) return -1;
    size_t length = (size_t)(end - start);
    if (length >= buffer_size) return -1;
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    return 0;
}

static int has_method(const char *request, const char *method)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"method\":\"%s\"", method);
    return strstr(request, pattern) != NULL;
}

static char *format_success(const char *request_id, const char *result)
{
    size_t length = strlen(request_id) + strlen(result) + 80;
    char *response = malloc(length);
    assert(response != NULL);
    snprintf(response, length,
             "{\"request_id\":\"%s\",\"success\":true,\"result\":%s}",
             request_id, result);
    return response;
}

static char *format_error(const char *request_id, const char *code,
                          const char *message)
{
    size_t length = strlen(request_id) + strlen(code) + strlen(message) + 160;
    char *response = malloc(length);
    assert(response != NULL);
    snprintf(response, length,
             "{\"request_id\":\"%s\",\"success\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"system_errno\":0,\"retryable\":false}}",
             request_id, code, message);
    return response;
}

static char *manager_response(const char *request, const char *request_id,
                              const char *controller_uri)
{
    if (has_method(request, "session.local_bootstrap")) {
        return format_success(request_id,
            "{\"session_id\":\"session-1\",\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"client_key_id\":\"local-key\",\"protocol_major\":0,\"protocol_minor\":1,\"negotiated_capabilities\":258,\"authenticated_at_ms\":10,\"expires_at_ms\":0}");
    }
    if (has_method(request, "manager.ping")) {
        return format_success(request_id,
            "{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"protocol_major\":0,\"protocol_minor\":1,\"server_time_ms\":100,\"uptime_ms\":7}");
    }
    if (has_method(request, "manager.status")) {
        return format_success(request_id,
            "{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"protocol_major\":0,\"protocol_minor\":1,\"capabilities\":258,\"started_at_ms\":10,\"server_time_ms\":20,\"workspace_count\":2,\"process_count\":3,\"controller_count\":4,\"active_client_sessions\":5}");
    }
    if (has_method(request, "workspace.create") ||
        has_method(request, "workspace.get")) {
        return format_success(request_id,
            "{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"name\":\"default\",\"created_at_ms\":1,\"updated_at_ms\":2,\"process_count\":3,\"running_process_count\":1}");
    }
    if (has_method(request, "workspace.list")) {
        return format_success(request_id,
            "{\"workspaces\":[{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"name\":\"default\",\"created_at_ms\":1,\"updated_at_ms\":2}],\"continuation_token\":\"next\",\"has_more\":true}");
    }
    if (has_method(request, "manager.cleanup")) {
        return format_success(request_id,
            "{\"removed_count\":2,\"skipped_live_count\":1,\"failed_count\":0}");
    }
    if (has_method(request, "process.start") ||
        has_method(request, "process.get") ||
        has_method(request, "process.wait")) {
        return format_success(request_id,
            "{\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"workspace_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"id\":\"cccccccccccccccccccccccccccccccc\",\"friendly_name\":\"build\",\"mode\":\"stream\",\"state\":\"running\",\"stdout_offset\":11,\"stderr_offset\":12,\"local_pid\":123,\"local_pgid\":123}");
    }
    if (has_method(request, "process.list")) {
        return format_success(request_id,
            "{\"processes\":[{\"id\":\"cccccccccccccccccccccccccccccccc\",\"friendly_name\":\"build\",\"mode\":\"stream\",\"state\":\"completed\",\"has_exit_status\":true,\"exit_code\":0}],\"has_more\":false}");
    }
    if (has_method(request, "process.read_output")) {
        return format_success(request_id,
            "{\"start_offset\":5,\"next_offset\":10,\"end_of_stream\":false,\"data\":\"hello\"}");
    }
    if (has_method(request, "workspace.key.add")) {
        return format_success(request_id,
            "{\"key_id\":\"dddddddddddddddddddddddddddddddd\",\"fingerprint\":\"fp\",\"label\":\"owner\",\"capabilities\":257,\"created_at_ms\":1}");
    }
    if (has_method(request, "workspace.key.list")) {
        return format_success(request_id,
            "{\"keys\":[{\"key_id\":\"dddddddddddddddddddddddddddddddd\",\"fingerprint\":\"fp\",\"label\":\"owner\",\"capabilities\":257}]}");
    }
    if (has_method(request, "attachment.request")) {
        size_t length = strlen(controller_uri) + 600;
        char *result = malloc(length);
        assert(result != NULL);
        snprintf(result, length,
                 "{\"grant_id\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\",\"manager_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"workspace_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"process_id\":\"cccccccccccccccccccccccccccccccc\",\"client_key_id\":\"dddddddddddddddddddddddddddddddd\",\"endpoint\":{\"uri\":\"%s\",\"server_identity\":\"controller\"},\"token\":\"tok\",\"issued_at_ms\":1,\"expires_at_ms\":2,\"connection_limit\":1,\"granted_channels\":\"stdout,stdin\",\"mode\":\"interactive\"}",
                 controller_uri);
        char *response = format_success(request_id, result);
        free(result);
        return response;
    }
    if (has_method(request, "events.list")) {
        return format_success(request_id,
            "{\"events\":[{\"global_sequence\":1,\"workspace_sequence\":2,\"timestamp_ms\":3,\"type\":\"process_started\",\"workspace_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"process_id\":\"cccccccccccccccccccccccccccccccc\",\"payload\":\"ok\"}]}");
    }
    if (has_method(request, "process.signal") ||
        has_method(request, "process.terminate") ||
        has_method(request, "process.kill") ||
        has_method(request, "process.remove") ||
        has_method(request, "workspace.rename") ||
        has_method(request, "workspace.stop") ||
        has_method(request, "workspace.delete") ||
        has_method(request, "workspace.key.update") ||
        has_method(request, "workspace.key.revoke") ||
        has_method(request, "manager.reconcile") ||
        has_method(request, "manager.shutdown")) {
        return format_success(request_id, "{}");
    }
    return format_error(request_id, "unsupported", "unsupported method");
}

static char *controller_response(const char *request, const char *request_id)
{
    if (has_method(request, "controller.status")) {
        return format_success(request_id,
            "{\"controller_id\":\"ffffffffffffffffffffffffffffffff\",\"state\":\"running\",\"mode\":\"stream\"}");
    }
    if (has_method(request, "controller.read")) {
        return format_success(request_id,
            "{\"stream\":\"stdout\",\"offset\":0,\"next_offset\":5,\"data\":\"hello\"}");
    }
    if (has_method(request, "controller.write") ||
        has_method(request, "controller.resize") ||
        has_method(request, "controller.detach")) {
        return format_success(request_id, "{}");
    }
    return format_error(request_id, "unsupported", "unsupported method");
}

static int serve_loop(int listen_fd, const char *log_path,
                      const char *mode, const char *scenario,
                      const char *controller_uri, int max_requests)
{
    FILE *log = fopen(log_path, "a");
    if (log == NULL) return 1;

    int handled = 0;
    while (handled < max_requests) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        while (handled < max_requests) {
            uint32_t size_network = 0;
            if (read_all(client_fd, &size_network, sizeof(size_network)) < 0) {
                break;
            }
            uint32_t size = ntohl(size_network);
            if (size > MAX_FRAME) break;
            char *request = calloc((size_t)size + 1, 1);
            if (request == NULL) break;
            if (read_all(client_fd, request, size) < 0) {
                free(request);
                break;
            }
            fprintf(log, "%s\n", request);
            fflush(log);

            char request_id[64] = "";
            if (json_string_field(request, "request_id", request_id,
                                  sizeof(request_id)) < 0) {
                snprintf(request_id, sizeof(request_id), "missing");
            }

            char *response = NULL;
            if (has_method(request, "auth.challenge")) {
                response = format_error(request_id, "unsupported",
                                        "auth challenge unsupported");
            } else if (has_method(request, "session.local_bootstrap")) {
                response = manager_response(request, request_id, controller_uri);
            } else if (strcmp(scenario, "malformed") == 0) {
                response = strdup("{not-json");
            } else if (strcmp(scenario, "mismatch") == 0) {
                response = manager_response(request, "wrong", controller_uri);
            } else if (strcmp(scenario, "error") == 0) {
                response = format_error(request_id, "unsupported",
                                        "forced mock error");
            } else if (strcmp(mode, "controller") == 0) {
                response = controller_response(request, request_id);
            } else {
                response = manager_response(request, request_id, controller_uri);
            }

            uint32_t response_size = htonl((uint32_t)strlen(response));
            int write_result =
                write_all(client_fd, &response_size, sizeof(response_size)) == 0 &&
                write_all(client_fd, response, strlen(response)) == 0;
            free(response);
            free(request);
            ++handled;
            if (!write_result) break;
        }
        close(client_fd);
    }

    fclose(log);
    return handled == max_requests ? 0 : 1;
}

static int serve_unix(const char *socket_path, const char *log_path,
                      const char *mode, const char *scenario,
                      const char *controller_uri, int max_requests)
{
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path)) return 1;
    strcpy(address.sun_path, socket_path);
    unlink(socket_path);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listen_fd, 8) < 0) {
        close(listen_fd);
        return 1;
    }

    int result = serve_loop(listen_fd, log_path, mode, scenario,
                            controller_uri, max_requests);
    close(listen_fd);
    unlink(socket_path);
    return result;
}

static int write_port_file(const char *path, uint16_t port)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) return -1;
    fprintf(file, "%u\n", (unsigned int)port);
    return fclose(file);
}

static int serve_tcp(const char *host, int port, const char *port_file,
                     const char *log_path, const char *mode,
                     const char *scenario, const char *controller_uri,
                     int max_requests)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        close(listen_fd);
        return 1;
    }
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listen_fd, 8) < 0) {
        close(listen_fd);
        return 1;
    }

    socklen_t address_length = sizeof(address);
    if (getsockname(listen_fd, (struct sockaddr *)&address,
                    &address_length) < 0 ||
        write_port_file(port_file, ntohs(address.sin_port)) < 0) {
        close(listen_fd);
        return 1;
    }

    int result = serve_loop(listen_fd, log_path, mode, scenario,
                            controller_uri, max_requests);
    close(listen_fd);
    return result;
}

int main(int argc, char **argv)
{
    const char *socket_path = NULL;
    const char *tcp_host = NULL;
    const char *tcp_port = NULL;
    const char *port_file = NULL;
    const char *log_path = NULL;
    const char *mode = "manager";
    const char *scenario = "normal";
    const char *controller_uri = "unix:///tmp/mock-controller.sock";
    int max_requests = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--tcp-host") == 0 && i + 1 < argc) {
            tcp_host = argv[++i];
        } else if (strcmp(argv[i], "--tcp-port") == 0 && i + 1 < argc) {
            tcp_port = argv[++i];
        } else if (strcmp(argv[i], "--port-file") == 0 && i + 1 < argc) {
            port_file = argv[++i];
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        } else if (strcmp(argv[i], "--controller-uri") == 0 && i + 1 < argc) {
            controller_uri = argv[++i];
        } else if (strcmp(argv[i], "--max-requests") == 0 && i + 1 < argc) {
            max_requests = atoi(argv[++i]);
        } else {
            return 2;
        }
    }

    if (log_path == NULL || max_requests <= 0) {
        return 2;
    }
    if (tcp_host != NULL || tcp_port != NULL || port_file != NULL) {
        if (tcp_host == NULL || tcp_port == NULL || port_file == NULL ||
            socket_path != NULL) {
            return 2;
        }
        return serve_tcp(tcp_host, atoi(tcp_port), port_file, log_path, mode,
                         scenario, controller_uri, max_requests);
    }
    if (socket_path == NULL) return 2;
    return serve_unix(socket_path, log_path, mode, scenario, controller_uri,
                      max_requests);
}
