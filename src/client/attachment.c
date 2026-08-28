#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include "cubicle/transport_tcp.h"
#include "cubicle/transport_unix.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { CUBICLE_ATTACHMENT_DEFAULT_IDLE_TIMEOUT_MS = 120000 };
enum { CUBICLE_ATTACHMENT_STREAM_HEADER_MAX = 256 };

static uint64_t attachment_stream_offset_value(
    const cubicle_attachment_t *attachment,
    cubicle_stream_kind_t stream);
static uint64_t *attachment_stream_offset(cubicle_attachment_t *attachment,
                                          cubicle_stream_kind_t stream);
static cubicle_channel_mask_t channel_for_stream(cubicle_stream_kind_t stream);
static cubicle_error_code_t attachment_rpc(cubicle_attachment_t *attachment,
                                           const char *method,
                                           const char *params,
                                           char **response_out);

static uint64_t attachment_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static cubicle_error_code_t attachment_set_error(cubicle_attachment_t *attachment,
                                                 cubicle_error_code_t code,
                                                 int system_errno,
                                                 const char *message)
{
    return set_error(attachment == NULL ? NULL : &attachment->last_error,
                     code, system_errno, false, message);
}

static void attachment_close_stream(cubicle_attachment_t *attachment)
{
    if (attachment != NULL && attachment->stream_fd >= 0) {
        close(attachment->stream_fd);
        attachment->stream_fd = -1;
    }
}

static const char *unix_path_from_endpoint(const cubicle_endpoint_t *endpoint)
{
    const char prefix[] = "unix://";
    size_t prefix_length = sizeof(prefix) - 1;
    if (endpoint == NULL ||
        strncmp(endpoint->uri, prefix, prefix_length) != 0) {
        return NULL;
    }
    const char *path = endpoint->uri + prefix_length;
    return path[0] == '/' ? path : NULL;
}

static int attachment_write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t written = send(fd, cursor, length, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            errno = EPIPE;
            return -1;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int attachment_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static cubicle_error_code_t attachment_connect_unix_stream(
    cubicle_attachment_t *attachment,
    cubicle_stream_kind_t stream,
    int *fd_out)
{
    const char *path = unix_path_from_endpoint(&attachment->grant.endpoint);
    if (path == NULL) {
        return attachment_set_error(attachment, CUBICLE_ERR_UNSUPPORTED, 0,
                                    "attachment streaming requires a Unix controller endpoint");
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(path);
    if (path_length >= sizeof(address.sun_path)) {
        return attachment_set_error(attachment, CUBICLE_ERR_INVALID_ARGUMENT,
                                    ENAMETOOLONG,
                                    "controller socket path is too long");
    }
    memcpy(address.sun_path, path, path_length + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return attachment_set_error(attachment, CUBICLE_ERR_IO, errno,
                                    "failed to create attachment stream socket");
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(fd);
        return attachment_set_error(attachment, CUBICLE_ERR_MANAGER_UNAVAILABLE,
                                    saved_errno,
                                    "failed to connect attachment stream socket");
    }

    uint64_t offset = attachment_stream_offset_value(attachment, stream);
    char command[128];
    int command_length = snprintf(command, sizeof(command), "attach %s %llu\n",
                                  stream_name(stream),
                                  (unsigned long long)offset);
    if (command_length < 0 || (size_t)command_length >= sizeof(command) ||
        attachment_write_all(fd, command, (size_t)command_length) < 0) {
        int saved_errno = errno;
        close(fd);
        return attachment_set_error(attachment, CUBICLE_ERR_IO, saved_errno,
                                    "failed to request attachment stream");
    }

    char header[CUBICLE_ATTACHMENT_STREAM_HEADER_MAX];
    size_t used = 0;
    while (used + 1 < sizeof(header)) {
        char ch;
        ssize_t nread = recv(fd, &ch, 1, 0);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved_errno = errno;
            close(fd);
            return attachment_set_error(attachment, CUBICLE_ERR_IO,
                                        saved_errno,
                                        "failed to read attachment stream header");
        }
        if (nread == 0) {
            close(fd);
            return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL,
                                        ECONNRESET,
                                        "attachment stream closed before header");
        }
        if (ch == '\n') {
            header[used] = '\0';
            break;
        }
        header[used++] = ch;
    }
    if (used + 1 >= sizeof(header)) {
        close(fd);
        return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL,
                                    ENOBUFS,
                                    "attachment stream header is too large");
    }
    if (strncmp(header, "ok attached ", 12) != 0) {
        close(fd);
        return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                    header[0] == '\0'
                                        ? "invalid attachment stream header"
                                        : header);
    }
    if (attachment_set_nonblocking(fd) < 0) {
        int saved_errno = errno;
        close(fd);
        return attachment_set_error(attachment, CUBICLE_ERR_IO, saved_errno,
                                    "failed to make attachment stream nonblocking");
    }

    *fd_out = fd;
    return CUBICLE_OK;
}

static cubicle_error_code_t create_controller_client(
    const cubicle_attachment_grant_t *grant,
    cubicle_client_t **client_out,
    cubicle_error_t *error)
{
    cubicle_transport_t *transport = NULL;
    cubicle_error_code_t code;

    if (strncmp(grant->endpoint.uri, "tcp://", 6) == 0) {
        code = cubicle_transport_tcp_create(&transport);
    } else {
        code = cubicle_transport_unix_create(&transport);
    }
    if (code != CUBICLE_OK) {
        set_error(error, code, 0, false, "failed to create controller transport");
        return code;
    }

    cubicle_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        transport->vtable->destroy(transport);
        return set_error(error, CUBICLE_ERR_INTERNAL, ENOMEM, false,
                         "failed to allocate controller client");
    }

    client->endpoint = grant->endpoint;
    client->transport = transport;
    snprintf(client->session.session_id, sizeof(client->session.session_id),
             "local-session");
    client->session.protocol_major = 0;
    client->session.protocol_minor = 1;

    cubicle_library_debug_log("controller.connect", client->endpoint.uri,
                              CUBICLE_OK, 0, 0, NULL);
    code = transport->vtable->connect(transport, &client->endpoint,
                                      &client->last_error);
    if (code != CUBICLE_OK) {
        cubicle_library_debug_log("controller.connect.result",
                                  client->endpoint.uri, code, 0, 0,
                                  &client->last_error);
        if (error != NULL) {
            *error = client->last_error;
        }
        transport->vtable->destroy(transport);
        free(client);
        return code;
    }

    *client_out = client;
    cubicle_library_debug_log("controller.connect.result",
                              client->endpoint.uri, CUBICLE_OK, 0, 0, NULL);
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    return CUBICLE_OK;
}

static void close_controller_client(cubicle_attachment_t *attachment)
{
    if (attachment != NULL && attachment->controller != NULL) {
        cubicle_client_disconnect(attachment->controller);
        attachment->controller = NULL;
    }
}

static cubicle_error_code_t build_attach_params(
    const cubicle_attachment_grant_t *grant,
    char params[2048])
{
    cubicle_json_builder_t builder = {0};
    if (cubicle_json_builder_append(&builder, "{\"token\":") < 0 ||
        cubicle_json_builder_append_string(&builder, grant->token) < 0 ||
        cubicle_json_builder_appendf(
            &builder, ",\"channels\":%u,\"mode\":",
            (unsigned int)grant->granted_channels) < 0 ||
        cubicle_json_builder_append_string(&builder,
            grant->mode == CUBICLE_ATTACHMENT_INTERACTIVE ? "interactive"
                                                          : "observer") < 0 ||
        cubicle_json_builder_append(&builder, "}") < 0 ||
        snprintf(params, 2048, "%s", builder.data) < 0 ||
        strlen(builder.data) >= 2048) {
        cubicle_json_builder_cleanup(&builder);
        return CUBICLE_ERR_RESOURCE_LIMIT;
    }
    cubicle_json_builder_cleanup(&builder);
    return CUBICLE_OK;
}

static cubicle_error_code_t parse_attach_response(
    cubicle_attachment_t *attachment,
    char *response)
{
    const char *result = json_object_field(response, "result");
    uint64_t value = 0;
    if (result == NULL ||
        json_u64_field(result, "accepted_channels", &value) < 0) {
        return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                    "invalid controller attach response");
    }
    attachment->channels = (cubicle_channel_mask_t)value;
    attachment->mode = attachment->grant.mode;
    if (json_u64_field(result, "stdout_offset", &attachment->stdout_offset) < 0) {
        attachment->stdout_offset = 0;
    }
    if (json_u64_field(result, "stderr_offset", &attachment->stderr_offset) < 0) {
        attachment->stderr_offset = 0;
    }
    if (json_u64_field(result, "tty_offset", &attachment->tty_offset) < 0) {
        attachment->tty_offset = 0;
    }
    return CUBICLE_OK;
}

static cubicle_error_code_t attach_controller(cubicle_attachment_t *attachment)
{
    uint64_t stdout_offset = attachment->stdout_offset;
    uint64_t stderr_offset = attachment->stderr_offset;
    uint64_t tty_offset = attachment->tty_offset;
    int preserve_offsets = attachment->attached_once;
    char params[2048];
    cubicle_error_code_t code = build_attach_params(&attachment->grant, params);
    if (code != CUBICLE_OK) {
        return attachment_set_error(attachment, code, 0,
                                    "attachment request is too large");
    }

    char *response = NULL;
    if (attachment->relay) {
        code = attachment_rpc(attachment, "controller.attach", params,
                              &response);
    } else if (attachment->controller != NULL) {
        code = rpc_object(attachment->controller, "controller.attach", params,
                          &response);
        if (code != CUBICLE_OK) {
            attachment->last_error =
                *cubicle_client_last_error(attachment->controller);
        }
    } else {
        code = attachment_set_error(attachment, CUBICLE_ERR_INVALID_STATE, 0,
                                    "controller client is not connected");
    }
    if (code != CUBICLE_OK) {
        free(response);
        return code;
    }
    code = parse_attach_response(attachment, response);
    free(response);
    if (code == CUBICLE_OK) {
        if (preserve_offsets) {
            attachment->stdout_offset = stdout_offset;
            attachment->stderr_offset = stderr_offset;
            attachment->tty_offset = tty_offset;
        }
        attachment->attached_once = 1;
        attachment->last_activity_ms = attachment_now_ms();
    }
    return code;
}

static cubicle_error_code_t ensure_controller_client(
    cubicle_attachment_t *attachment)
{
    if (attachment->relay) {
        if (attachment->manager == NULL) {
            return attachment_set_error(attachment, CUBICLE_ERR_INVALID_STATE,
                                        0,
                                        "relay attachment has no manager client");
        }
        return CUBICLE_OK;
    }

    uint64_t now = attachment_now_ms();
    if (attachment->controller != NULL && attachment->idle_timeout_ms > 0 &&
        now > 0 && attachment->last_activity_ms > 0 &&
        now - attachment->last_activity_ms >
            (uint64_t)attachment->idle_timeout_ms) {
        close_controller_client(attachment);
    }
    if (attachment->controller != NULL) {
        return CUBICLE_OK;
    }

    cubicle_error_code_t code = create_controller_client(
        &attachment->grant, &attachment->controller, &attachment->last_error);
    if (code != CUBICLE_OK) {
        return code;
    }
    code = attach_controller(attachment);
    if (code != CUBICLE_OK) {
        close_controller_client(attachment);
    }
    return code;
}

static bool attachment_rpc_can_retry(const char *method)
{
    return strcmp(method, "controller.read") == 0 ||
           strcmp(method, "controller.write") == 0 ||
           strcmp(method, "controller.status") == 0 ||
           strcmp(method, "controller.snapshot") == 0 ||
           strcmp(method, "controller.resize") == 0 ||
           strcmp(method, "controller.detach") == 0;
}

static bool attachment_is_transport_error(cubicle_error_code_t code)
{
    return code == CUBICLE_ERR_IO ||
           code == CUBICLE_ERR_MANAGER_UNAVAILABLE ||
           code == CUBICLE_ERR_CONTROLLER_UNAVAILABLE ||
           code == CUBICLE_ERR_TIMEOUT;
}

static cubicle_error_code_t attachment_refresh_relay_grant(
    cubicle_attachment_t *attachment)
{
    if (attachment == NULL || !attachment->relay ||
        attachment->manager == NULL) {
        return attachment_set_error(attachment, CUBICLE_ERR_INVALID_STATE, 0,
                                    "relay attachment cannot refresh grant");
    }

    uint64_t stdout_offset = attachment->stdout_offset;
    uint64_t stderr_offset = attachment->stderr_offset;
    uint64_t tty_offset = attachment->tty_offset;

    cubicle_attachment_request_t request;
    memset(&request, 0, sizeof(request));
    request.process_id = attachment->grant.process_id;
    request.channels = attachment->grant.granted_channels;
    request.mode = attachment->grant.mode;
    request.stdout_offset = stdout_offset;
    request.stderr_offset = stderr_offset;
    request.tty_offset = tty_offset;

    cubicle_attachment_grant_t grant;
    cubicle_error_code_t code =
        cubicle_attachment_request(attachment->manager, &request, &grant);
    if (code != CUBICLE_OK && attachment_is_transport_error(code)) {
        if (cubicle_client_reconnect(attachment->manager) == CUBICLE_OK) {
            code = cubicle_attachment_request(attachment->manager, &request,
                                              &grant);
        } else {
            const cubicle_error_t *error =
                cubicle_client_last_error(attachment->manager);
            if (error != NULL) {
                attachment->last_error = *error;
            }
        }
    }
    if (code != CUBICLE_OK) {
        const cubicle_error_t *error =
            cubicle_client_last_error(attachment->manager);
        if (error != NULL) {
            attachment->last_error = *error;
        }
        return code;
    }

    attachment->grant = grant;
    attachment->channels = CUBICLE_CHANNEL_NONE;
    attachment->stdout_offset = stdout_offset;
    attachment->stderr_offset = stderr_offset;
    attachment->tty_offset = tty_offset;
    attachment->attached_once = 0;
    code = attach_controller(attachment);
    if (code == CUBICLE_OK) {
        attachment->stdout_offset = stdout_offset;
        attachment->stderr_offset = stderr_offset;
        attachment->tty_offset = tty_offset;
    }
    return code;
}

static cubicle_stream_kind_t attachment_read_stream(
    const cubicle_attachment_t *attachment)
{
    if ((attachment->channels & CUBICLE_CHANNEL_TTY) != 0) {
        return CUBICLE_STREAM_TTY;
    }
    if ((attachment->channels & CUBICLE_CHANNEL_STDOUT) != 0) {
        return CUBICLE_STREAM_STDOUT;
    }
    return CUBICLE_STREAM_STDERR;
}

static uint64_t *attachment_stream_offset(cubicle_attachment_t *attachment,
                                          cubicle_stream_kind_t stream)
{
    if (stream == CUBICLE_STREAM_TTY) {
        return &attachment->tty_offset;
    }
    if (stream == CUBICLE_STREAM_STDERR) {
        return &attachment->stderr_offset;
    }
    return &attachment->stdout_offset;
}

static uint64_t attachment_stream_offset_value(
    const cubicle_attachment_t *attachment,
    cubicle_stream_kind_t stream)
{
    if (attachment == NULL) {
        return 0;
    }
    if (stream == CUBICLE_STREAM_TTY) {
        return attachment->tty_offset;
    }
    if (stream == CUBICLE_STREAM_STDERR) {
        return attachment->stderr_offset;
    }
    return attachment->stdout_offset;
}

static cubicle_channel_mask_t channel_for_stream(cubicle_stream_kind_t stream)
{
    if (stream == CUBICLE_STREAM_TTY) {
        return CUBICLE_CHANNEL_TTY;
    }
    if (stream == CUBICLE_STREAM_STDERR) {
        return CUBICLE_CHANNEL_STDERR;
    }
    return CUBICLE_CHANNEL_STDOUT;
}

static int attachment_build_relay_proxy_params(
    const cubicle_attachment_t *attachment,
    const char *method,
    const char *params,
    cubicle_json_builder_t *builder)
{
    if (cubicle_json_builder_append(builder, "{\"grant_id\":") < 0 ||
        cubicle_json_builder_append_string(builder,
                                           attachment->grant.grant_id) < 0 ||
        cubicle_json_builder_append(builder, ",\"process_id\":") < 0 ||
        cubicle_json_builder_append_string(builder,
                                           attachment->grant.process_id) < 0 ||
        cubicle_json_builder_append(builder, ",\"token\":") < 0 ||
        cubicle_json_builder_append_string(builder, attachment->grant.token) <
            0 ||
        cubicle_json_builder_appendf(
            builder, ",\"channels\":%u,\"mode\":",
            (unsigned int)attachment->grant.granted_channels) < 0 ||
        cubicle_json_builder_append_string(
            builder,
            attachment->grant.mode == CUBICLE_ATTACHMENT_INTERACTIVE
                ? "interactive"
                : "observer") < 0 ||
        cubicle_json_builder_append(builder, ",\"method\":") < 0 ||
        cubicle_json_builder_append_string(builder, method) < 0 ||
        cubicle_json_builder_append(builder, ",\"params\":") < 0 ||
        cubicle_json_builder_append(builder, params == NULL ? "{}" : params) <
            0 ||
        cubicle_json_builder_append(builder, "}") < 0) {
        return -1;
    }
    return 0;
}

static cubicle_error_code_t attachment_rpc(cubicle_attachment_t *attachment,
                                           const char *method,
                                           const char *params,
                                           char **response_out)
{
    if (attachment->relay) {
        cubicle_json_builder_t builder = {0};
        if (attachment_build_relay_proxy_params(attachment, method, params,
                                                &builder) < 0) {
            cubicle_json_builder_cleanup(&builder);
            return attachment_set_error(attachment,
                                        CUBICLE_ERR_RESOURCE_LIMIT, 0,
                                        "relay request is too large");
        }

        cubicle_error_code_t code = rpc_object(
            attachment->manager, "attachment.proxy", builder.data,
            response_out);
        if (code != CUBICLE_OK) {
            const cubicle_error_t *error =
                cubicle_client_last_error(attachment->manager);
            if (error != NULL) {
                attachment->last_error = *error;
            }
            if (attachment_rpc_can_retry(method) &&
                attachment_is_transport_error(code) &&
                attachment_refresh_relay_grant(attachment) == CUBICLE_OK) {
                cubicle_json_builder_cleanup(&builder);
                memset(&builder, 0, sizeof(builder));
                if (attachment_build_relay_proxy_params(attachment, method,
                                                        params,
                                                        &builder) < 0) {
                    cubicle_json_builder_cleanup(&builder);
                    return attachment_set_error(
                        attachment, CUBICLE_ERR_RESOURCE_LIMIT, 0,
                        "relay request is too large");
                }
                code = rpc_object(attachment->manager, "attachment.proxy",
                                  builder.data, response_out);
                if (code != CUBICLE_OK) {
                    error = cubicle_client_last_error(attachment->manager);
                    if (error != NULL) {
                        attachment->last_error = *error;
                    }
                }
            }
        }
        cubicle_json_builder_cleanup(&builder);
        if (code != CUBICLE_OK) {
            return code;
        }
        attachment->last_activity_ms = attachment_now_ms();
        return CUBICLE_OK;
    }

    if (attachment->persistent_unsupported) {
        cubicle_client_t *client = NULL;
        cubicle_error_code_t code = create_controller_client(
            &attachment->grant, &client, &attachment->last_error);
        if (code != CUBICLE_OK) {
            return code;
        }
        code = rpc_object(client, method, params, response_out);
        if (code != CUBICLE_OK) {
            attachment->last_error = *cubicle_client_last_error(client);
        }
        cubicle_client_disconnect(client);
        return code;
    }

    cubicle_error_code_t code = ensure_controller_client(attachment);
    if (code != CUBICLE_OK) {
        return code;
    }
    code = rpc_object(attachment->controller, method, params, response_out);
    if (code != CUBICLE_OK) {
        attachment->last_error = *cubicle_client_last_error(attachment->controller);
        if (attachment_rpc_can_retry(method) &&
            attachment_is_transport_error(code)) {
            close_controller_client(attachment);
            if (ensure_controller_client(attachment) == CUBICLE_OK) {
                code = rpc_object(attachment->controller, method, params,
                                  response_out);
                if (code != CUBICLE_OK) {
                    attachment->last_error =
                        *cubicle_client_last_error(attachment->controller);
                }
            }
        }
        if (attachment_rpc_can_retry(method) &&
            attachment_is_transport_error(code)) {
            close_controller_client(attachment);
            attachment->persistent_unsupported = 1;
            cubicle_client_t *client = NULL;
            cubicle_error_code_t legacy_code = create_controller_client(
                &attachment->grant, &client, &attachment->last_error);
            if (legacy_code == CUBICLE_OK) {
                code = rpc_object(client, method, params, response_out);
                if (code != CUBICLE_OK) {
                    attachment->last_error = *cubicle_client_last_error(client);
                }
                cubicle_client_disconnect(client);
            } else {
                code = legacy_code;
            }
        }
    }
    if (code == CUBICLE_OK) {
        attachment->last_activity_ms = attachment_now_ms();
    }
    return code;
}

cubicle_error_code_t cubicle_attachment_request(cubicle_client_t *client,
    const cubicle_attachment_request_t *request,
    cubicle_attachment_grant_t *grant_out)
{
    if (client == NULL || request == NULL || request->process_id == NULL ||
        request->process_id[0] == '\0' || grant_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_json_builder_t params = {0}; cubicle_json_builder_append(&params, "{\"process_id\":"); cubicle_json_builder_append_string(&params, request->process_id);
    cubicle_json_builder_appendf(&params, ",\"channels\":%u,\"mode\":", (unsigned)request->channels);
    cubicle_json_builder_append_string(&params, attachment_mode_name(request->mode));
    cubicle_json_builder_appendf(&params, ",\"stdout_offset\":%llu,\"stderr_offset\":%llu,\"tty_offset\":%llu,\"rows\":%u,\"cols\":%u}",
                   (unsigned long long)request->stdout_offset, (unsigned long long)request->stderr_offset,
                   (unsigned long long)request->tty_offset, request->rows, request->cols);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "attachment.request", params.data, &response);
    cubicle_json_builder_cleanup(&params); if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(grant_out, 0, sizeof(*grant_out));
    json_string_field(result, "grant_id", grant_out->grant_id, sizeof(grant_out->grant_id));
    json_string_field(result, "manager_id", grant_out->manager_id, sizeof(grant_out->manager_id));
    json_string_field(result, "workspace_id", grant_out->workspace_id, sizeof(grant_out->workspace_id));
    json_string_field(result, "process_id", grant_out->process_id, sizeof(grant_out->process_id));
    json_string_field(result, "client_key_id", grant_out->client_key_id, sizeof(grant_out->client_key_id));
    json_string_field(result, "token", grant_out->token, sizeof(grant_out->token));
    json_u64_field(result, "issued_at_ms", &grant_out->issued_at_ms);
    json_u64_field(result, "expires_at_ms", &grant_out->expires_at_ms);
    uint64_t value = 0; if (json_u64_field(result, "connection_limit", &value) == 0) grant_out->connection_limit = (uint32_t)value;
    char channels[128]; if (json_string_field(result, "granted_channels", channels, sizeof(channels)) == 0) channel_mask_from_string(channels, &grant_out->granted_channels);
    char mode[32]; if (json_string_field(result, "mode", mode, sizeof(mode)) == 0 && strcmp(mode, "interactive") == 0) grant_out->mode = CUBICLE_ATTACHMENT_INTERACTIVE;
    const char *endpoint = json_object_field(result, "endpoint"); if (endpoint != NULL) parse_endpoint(endpoint, &grant_out->endpoint);
    free(response); return grant_out->grant_id[0] == '\0' ? set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid attachment grant") : CUBICLE_OK;
}

cubicle_error_code_t cubicle_attachment_connect(const cubicle_attachment_grant_t *grant,
    const cubicle_attachment_options_t *options, cubicle_attachment_t **attachment_out)
{
    if (grant == NULL || attachment_out == NULL || grant->grant_id[0] == '\0') {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    cubicle_attachment_t *attachment = calloc(1, sizeof(*attachment));
    if (attachment == NULL) {
        return CUBICLE_ERR_INTERNAL;
    }

    attachment->stream_fd = -1;
    attachment->grant = *grant;
    attachment->idle_timeout_ms =
        options != NULL && options->io_timeout_ms > 0
            ? options->io_timeout_ms
            : CUBICLE_ATTACHMENT_DEFAULT_IDLE_TIMEOUT_MS;
    cubicle_error_code_t code = ensure_controller_client(attachment);
    if (code != CUBICLE_OK) {
        cubicle_attachment_disconnect(attachment);
        return code;
    }

    *attachment_out = attachment;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_attachment_connect_relay(
    cubicle_client_t *manager,
    const cubicle_attachment_grant_t *grant,
    const cubicle_attachment_options_t *options,
    cubicle_attachment_t **attachment_out)
{
    if (manager == NULL || grant == NULL || attachment_out == NULL ||
        grant->grant_id[0] == '\0') {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    cubicle_attachment_t *attachment = calloc(1, sizeof(*attachment));
    if (attachment == NULL) {
        return CUBICLE_ERR_INTERNAL;
    }

    attachment->stream_fd = -1;
    attachment->grant = *grant;
    attachment->manager = manager;
    attachment->relay = 1;
    attachment->idle_timeout_ms =
        options != NULL && options->io_timeout_ms > 0
            ? options->io_timeout_ms
            : CUBICLE_ATTACHMENT_DEFAULT_IDLE_TIMEOUT_MS;

    cubicle_error_code_t code = attach_controller(attachment);
    if (code != CUBICLE_OK) {
        cubicle_attachment_disconnect(attachment);
        return code;
    }

    *attachment_out = attachment;
    return CUBICLE_OK;
}

cubicle_channel_mask_t cubicle_attachment_channels(
    const cubicle_attachment_t *attachment)
{
    return attachment == NULL ? CUBICLE_CHANNEL_NONE : attachment->channels;
}

void cubicle_attachment_replay(cubicle_attachment_t *attachment,
                               uint64_t replay_bytes)
{
    if (attachment == NULL) {
        return;
    }
    cubicle_library_debug_log("attachment.replay",
                              attachment->grant.process_id, CUBICLE_OK,
                              (size_t)replay_bytes, 0, NULL);
    attachment->stdout_offset = attachment->stdout_offset > replay_bytes
                                    ? attachment->stdout_offset - replay_bytes
                                    : 0;
    attachment->stderr_offset = attachment->stderr_offset > replay_bytes
                                    ? attachment->stderr_offset - replay_bytes
                                    : 0;
    attachment->tty_offset = attachment->tty_offset > replay_bytes
                                 ? attachment->tty_offset - replay_bytes
                                 : 0;
}

uint64_t cubicle_attachment_read_offset(
    const cubicle_attachment_t *attachment,
    cubicle_stream_kind_t stream)
{
    return attachment_stream_offset_value(attachment, stream);
}

cubicle_error_code_t cubicle_attachment_stream_start(
    cubicle_attachment_t *attachment,
    cubicle_stream_kind_t stream)
{
    if (attachment == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    if ((attachment->channels & channel_for_stream(stream)) == 0) {
        return attachment_set_error(attachment, CUBICLE_ERR_INVALID_STATE, 0,
                                    "attachment stream is not readable");
    }

    if (attachment->stream_fd >= 0 && attachment->stream_fd_stream == stream) {
        return CUBICLE_OK;
    }
    if (attachment->relay) {
        return CUBICLE_OK;
    }

    attachment_close_stream(attachment);
    int fd = -1;
    cubicle_error_code_t code =
        attachment_connect_unix_stream(attachment, stream, &fd);
    if (code != CUBICLE_OK) {
        return code;
    }
    attachment->stream_fd = fd;
    attachment->stream_fd_stream = stream;
    cubicle_library_debug_log("attachment.stream_start", stream_name(stream),
                              CUBICLE_OK, 0, 0, NULL);
    return CUBICLE_OK;
}

int cubicle_attachment_stream_fd(const cubicle_attachment_t *attachment,
                                 cubicle_stream_kind_t stream)
{
    if (attachment == NULL || attachment->stream_fd < 0 ||
        attachment->stream_fd_stream != stream) {
        return -1;
    }
    return attachment->stream_fd;
}

ssize_t cubicle_attachment_read(cubicle_attachment_t *attachment, void *buffer, size_t length)
{
    if (attachment == NULL) {
        errno = EINVAL;
        return -1;
    }
    return cubicle_attachment_read_stream(
        attachment, attachment_read_stream(attachment), buffer, length, NULL);
}

ssize_t cubicle_attachment_read_stream(cubicle_attachment_t *attachment,
                                       cubicle_stream_kind_t stream,
                                       void *buffer,
                                       size_t length,
                                       bool *end_of_stream_out)
{
    if (attachment == NULL || buffer == NULL || length == 0) {
        errno = EINVAL;
        cubicle_library_debug_log("attachment.read", stream_name(stream),
                                  CUBICLE_ERR_INVALID_ARGUMENT, length, 0,
                                  NULL);
        return -1;
    }
    if ((attachment->channels & channel_for_stream(stream)) == 0) {
        (void)attachment_set_error(attachment, CUBICLE_ERR_INVALID_STATE, 0,
                                   "attachment is not readable");
        errno = EINVAL;
        cubicle_library_debug_log("attachment.read", stream_name(stream),
                                  CUBICLE_ERR_INVALID_STATE, length, 0,
                                  &attachment->last_error);
        return -1;
    }

    uint64_t *offset = attachment_stream_offset(attachment, stream);
    if (attachment->stream_fd >= 0 && attachment->stream_fd_stream == stream) {
        ssize_t nread = recv(attachment->stream_fd, buffer, length, 0);
        if (nread < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            int saved_errno = errno;
            attachment_close_stream(attachment);
            (void)attachment_set_error(attachment, CUBICLE_ERR_IO,
                                       saved_errno,
                                       "attachment stream read failed");
            errno = saved_errno;
            return -1;
        }
        if (nread == 0) {
            attachment_close_stream(attachment);
            (void)attachment_set_error(attachment, CUBICLE_ERR_IO, EPIPE,
                                       "attachment stream closed");
            errno = EPIPE;
            return -1;
        }
        *offset += (uint64_t)nread;
        if (end_of_stream_out != NULL) {
            *end_of_stream_out = false;
        }
        cubicle_library_debug_log("attachment.stream_read", stream_name(stream),
                                  CUBICLE_OK, length, (size_t)nread, NULL);
        return nread;
    }

    size_t maximum_length = length > 8192 ? 8192 : length;
    if (maximum_length > 1) {
        maximum_length /= 2;
    }
    char params[256];
    int params_length = snprintf(
        params, sizeof(params),
        "{\"stream\":\"%s\",\"offset\":%llu,\"maximum_length\":%zu}",
        stream_name(stream), (unsigned long long)*offset, maximum_length);
    if (params_length < 0 || (size_t)params_length >= sizeof(params)) {
        (void)attachment_set_error(attachment, CUBICLE_ERR_RESOURCE_LIMIT, 0,
                                   "read request is too large");
        errno = ENOSPC;
        return -1;
    }

    char *response = NULL;
    cubicle_error_code_t code = attachment_rpc(attachment, "controller.read",
                                               params, &response);
    if (code != CUBICLE_OK) {
        errno = EIO;
        cubicle_library_debug_log("attachment.read", stream_name(stream),
                                  code, length, 0, &attachment->last_error);
        return -1;
    }

    const char *result = json_object_field(response, "result");
    uint64_t next_offset = 0;
    bool end_of_stream = false;
    if (result == NULL ||
        json_u64_field(result, "next_offset", &next_offset) < 0 ||
        json_bool_field(result, "end_of_stream", &end_of_stream) < 0 ||
        next_offset < *offset) {
        free(response);
        (void)attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                   "invalid read response");
        errno = EPROTO;
        return -1;
    }

    cubicle_json_doc_t document;
    if (cubicle_json_parse(&document, result) < 0) {
        free(response);
        (void)attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                   "invalid read response");
        errno = EPROTO;
        return -1;
    }
    yyjson_val *data_value = yyjson_obj_get(document.root, "data");
    const char *text = yyjson_is_str(data_value) ? yyjson_get_str(data_value)
                                                : NULL;
    size_t data_length = yyjson_is_str(data_value)
                             ? yyjson_get_len(data_value)
                             : 0;
    if (text == NULL || data_length > length) {
        cubicle_json_cleanup(&document);
        free(response);
        (void)attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                   "invalid read data");
        errno = EPROTO;
        return -1;
    }
    memcpy(buffer, text, data_length);
    cubicle_json_cleanup(&document);
    free(response);
    *offset = next_offset;
    if (end_of_stream_out != NULL) {
        *end_of_stream_out = end_of_stream;
    }
    cubicle_library_debug_log("attachment.read", stream_name(stream),
                              CUBICLE_OK, length, data_length, NULL);
    return (ssize_t)data_length;
}

ssize_t cubicle_attachment_write(cubicle_attachment_t *attachment, const void *buffer, size_t length)
{
    if (attachment == NULL || (buffer == NULL && length > 0)) {
        errno = EINVAL;
        cubicle_library_debug_log("attachment.write", "stdin",
                                  CUBICLE_ERR_INVALID_ARGUMENT, length, 0,
                                  NULL);
        return -1;
    }
    if ((attachment->channels & CUBICLE_CHANNEL_STDIN) == 0) {
        (void)attachment_set_error(attachment, CUBICLE_ERR_INVALID_STATE, 0,
                                   "attachment is not writable");
        errno = EINVAL;
        cubicle_library_debug_log("attachment.write", "stdin",
                                  CUBICLE_ERR_INVALID_STATE, length, 0,
                                  &attachment->last_error);
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    size_t chunk_length = length > 4095 ? 4095 : length;
    cubicle_json_builder_t params = {0};
    if (cubicle_json_builder_append(&params, "{\"data\":") < 0 ||
        cubicle_json_builder_append_string_n(&params, buffer,
                                             chunk_length) < 0 ||
        cubicle_json_builder_append(&params, "}") < 0) {
        cubicle_json_builder_cleanup(&params);
        (void)attachment_set_error(attachment, CUBICLE_ERR_RESOURCE_LIMIT, 0,
                                   "write chunk is too large");
        errno = ENOSPC;
        return -1;
    }

    char *response = NULL;
    cubicle_error_code_t code = attachment_rpc(attachment, "controller.write",
                                               params.data, &response);
    cubicle_json_builder_cleanup(&params);
    free(response);
    if (code != CUBICLE_OK) {
        errno = EIO;
        cubicle_library_debug_log("attachment.write", "stdin", code,
                                  chunk_length, 0, &attachment->last_error);
        return -1;
    }
    cubicle_library_debug_log("attachment.write", "stdin", CUBICLE_OK,
                              chunk_length, 0, NULL);
    return (ssize_t)chunk_length;
}

cubicle_error_code_t cubicle_attachment_resize(cubicle_attachment_t *attachment,
                                               unsigned int rows, unsigned int cols)
{
    if (attachment == NULL || rows == 0 || cols == 0) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    char params[128];
    int length = snprintf(params, sizeof(params),
                          "{\"rows\":%u,\"columns\":%u}", rows, cols);
    if (length < 0 || (size_t)length >= sizeof(params)) {
        return attachment_set_error(attachment, CUBICLE_ERR_RESOURCE_LIMIT, 0,
                                    "resize request is too large");
    }

    char *response = NULL;
    cubicle_error_code_t code = attachment_rpc(attachment,
                                               "controller.resize", params,
                                               &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_attachment_resize_tracked(
    cubicle_attachment_t *attachment,
    cubicle_resize_tracker_t *tracker,
    unsigned int rows,
    unsigned int cols,
    bool force,
    bool *sent_out)
{
    if (sent_out != NULL) {
        *sent_out = false;
    }
    if (attachment == NULL || tracker == NULL || rows == 0 || cols == 0) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    if (!force && tracker->has_size && tracker->rows == rows &&
        tracker->cols == cols) {
        return CUBICLE_OK;
    }

    cubicle_error_code_t code = cubicle_attachment_resize(attachment, rows,
                                                          cols);
    if (code != CUBICLE_OK) {
        return code;
    }
    tracker->rows = rows;
    tracker->cols = cols;
    tracker->has_size = true;
    if (sent_out != NULL) {
        *sent_out = true;
    }
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_attachment_close_input(cubicle_attachment_t *attachment)
{
    if (attachment == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = attachment_rpc(attachment,
                                               "controller.close_input", "{}",
                                               &response);
    free(response);
    if (code != CUBICLE_OK) {
        return code;
    }
    attachment->channels &= (cubicle_channel_mask_t)~CUBICLE_CHANNEL_STDIN;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_attachment_status(
    cubicle_attachment_t *attachment,
    cubicle_attachment_status_t *status_out)
{
    if (attachment == NULL || status_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }

    char *response = NULL;
    cubicle_error_code_t code = attachment_rpc(attachment,
                                               "controller.status", "{}",
                                               &response);
    if (code != CUBICLE_OK) {
        return code;
    }

    const char *result = json_object_field(response, "result");
    char state[32];
    memset(status_out, 0, sizeof(*status_out));
    if (result == NULL ||
        json_string_field(result, "state", state, sizeof(state)) < 0 ||
        process_state_from_string(state, &status_out->state) < 0) {
        free(response);
        return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                    "invalid controller status");
    }
    (void)json_i64_field(result, "pid", &status_out->local_pid);
    (void)json_i64_field(result, "pgid", &status_out->local_pgid);
    uint64_t value = 0;
    if (json_u64_field(result, "result", &value) == 0) {
        status_out->exit_code = (int)value;
        status_out->has_exit_status = true;
    }
    (void)json_u64_field(result, "stdout_offset", &status_out->stdout_offset);
    (void)json_u64_field(result, "stderr_offset", &status_out->stderr_offset);
    (void)json_u64_field(result, "tty_offset", &status_out->tty_offset);
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_attachment_snapshot(
    cubicle_attachment_t *attachment,
    cubicle_terminal_snapshot_t *snapshot_out)
{
    if (attachment == NULL || snapshot_out == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));

    char *response = NULL;
    cubicle_error_code_t code = attachment_rpc(attachment,
                                               "controller.snapshot", "{}",
                                               &response);
    if (code != CUBICLE_OK) {
        return code;
    }

    const char *result = json_object_field(response, "result");
    cubicle_json_doc_t document;
    if (result == NULL || cubicle_json_parse(&document, result) < 0) {
        free(response);
        return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                    "invalid snapshot response");
    }

    uint64_t rows = 0;
    uint64_t cols = 0;
    uint64_t cursor_row = 0;
    uint64_t cursor_col = 0;
    uint64_t offset = 0;
    bool cursor_visible = true;
    bool application_cursor = false;
    yyjson_val *cells = NULL;
    cubicle_validation_error_t error;
    if (cubicle_json_get_required_u64(document.root, "rows", &rows, &error) < 0 ||
        cubicle_json_get_required_u64(document.root, "columns", &cols, &error) < 0 ||
        cubicle_json_get_required_u64(document.root, "cursor_row", &cursor_row, &error) < 0 ||
        cubicle_json_get_required_u64(document.root, "cursor_column", &cursor_col, &error) < 0 ||
        cubicle_json_get_optional_bool(document.root, "cursor_visible",
                                       &cursor_visible, NULL, &error) < 0 ||
        cubicle_json_get_optional_bool(document.root, "application_cursor",
                                       &application_cursor, NULL, &error) < 0 ||
        cubicle_json_get_required_u64(document.root, "offset", &offset, &error) < 0 ||
        cubicle_json_get_required_array(document.root, "cells", &cells,
                                        &error) < 0 ||
        rows == 0 || cols == 0 || rows > 1000 || cols > 1000 ||
        rows > SIZE_MAX / cols) {
        cubicle_json_cleanup(&document);
        free(response);
        return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                    "invalid snapshot response");
    }

    size_t cell_count = (size_t)rows * (size_t)cols;
    if (cubicle_json_array_size(cells) != cell_count) {
        cubicle_json_cleanup(&document);
        free(response);
        return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                    "invalid snapshot cell count");
    }

    cubicle_terminal_cell_t *parsed_cells =
        calloc(cell_count, sizeof(*parsed_cells));
    if (parsed_cells == NULL) {
        cubicle_json_cleanup(&document);
        free(response);
        return attachment_set_error(attachment, CUBICLE_ERR_INTERNAL, ENOMEM,
                                    "failed to allocate snapshot");
    }

    for (size_t i = 0; i < cell_count; ++i) {
        yyjson_val *cell = cubicle_json_array_get(cells, i);
        if (!yyjson_is_obj(cell) ||
            cubicle_json_get_required_string(cell, "t", parsed_cells[i].text,
                                             sizeof(parsed_cells[i].text),
                                             &error) < 0 ||
            cubicle_json_get_required_string(cell, "sgr", parsed_cells[i].sgr,
                                             sizeof(parsed_cells[i].sgr),
                                             &error) < 0) {
            free(parsed_cells);
            cubicle_json_cleanup(&document);
            free(response);
            return attachment_set_error(attachment, CUBICLE_ERR_PROTOCOL, 0,
                                        "invalid snapshot cell");
        }
    }

    snapshot_out->rows = (unsigned int)rows;
    snapshot_out->cols = (unsigned int)cols;
    snapshot_out->cursor_row = (unsigned int)cursor_row;
    snapshot_out->cursor_col = (unsigned int)cursor_col;
    snapshot_out->cursor_visible = cursor_visible;
    snapshot_out->application_cursor = application_cursor;
    snapshot_out->offset = offset;
    snapshot_out->cells = parsed_cells;
    attachment->tty_offset = offset;
    cubicle_library_debug_log("attachment.snapshot", "tty", CUBICLE_OK,
                              (size_t)offset, 0, NULL);

    cubicle_json_cleanup(&document);
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_attachment_detach(cubicle_attachment_t *attachment)
{
    if (attachment == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    char *response = NULL;
    cubicle_error_code_t code = attachment_rpc(attachment,
                                               "controller.detach", "{}",
                                               &response);
    free(response);
    return code;
}

const cubicle_error_t *cubicle_attachment_last_error(const cubicle_attachment_t *attachment)
{
    return attachment == NULL ? NULL : &attachment->last_error;
}

void cubicle_attachment_disconnect(cubicle_attachment_t *attachment)
{
    if (attachment == NULL) {
        return;
    }
    attachment_close_stream(attachment);
    close_controller_client(attachment);
    free(attachment);
}
