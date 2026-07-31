#include "cubicle/attachment.h"
#include "cubicle/auth.h"
#include "cubicle/client.h"
#include "cubicle/events.h"
#include "cubicle/manager.h"
#include "cubicle/process.h"
#include "cubicle/workspace.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

typedef struct json_buffer {
    char *data;
    size_t length;
    size_t capacity;
} json_buffer_t;

struct cubicle_client {
    cubicle_endpoint_t endpoint;
    int connect_timeout_ms;
    int request_timeout_ms;
    uint64_t next_request_id;
    cubicle_transport_t *transport;
    cubicle_error_t last_error;
    cubicle_session_info_t session;
};

struct cubicle_signer {
    cubicle_signer_callbacks_t callbacks;
    void *context;
};

struct cubicle_attachment {
    cubicle_error_t last_error;
};

struct cubicle_event_subscription {
    cubicle_error_t last_error;
};

static cubicle_error_code_t set_error(cubicle_error_t *error,
                                      cubicle_error_code_t code,
                                      int system_errno,
                                      bool retryable,
                                      const char *message)
{
    if (error != NULL) {
        error->code = code;
        error->system_errno = system_errno;
        error->retryable = retryable;
        snprintf(error->message, sizeof(error->message), "%s",
                 message == NULL ? "" : message);
    }
    return code;
}

static cubicle_error_code_t set_client_error(cubicle_client_t *client,
                                             cubicle_error_code_t code,
                                             int system_errno,
                                             const char *message)
{
    return set_error(client == NULL ? NULL : &client->last_error, code,
                     system_errno, false, message);
}

static cubicle_error_code_t unsupported_client(cubicle_client_t *client,
                                               const char *message)
{
    return set_client_error(client, CUBICLE_ERR_UNSUPPORTED, 0, message);
}

static int buffer_reserve(json_buffer_t *buffer, size_t extra)
{
    if (buffer->length + extra + 1 <= buffer->capacity) {
        return 0;
    }
    size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
    while (buffer->length + extra + 1 > capacity) {
        if (capacity > ((size_t)-1) / 2) {
            return -1;
        }
        capacity *= 2;
    }
    char *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        return -1;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 0;
}

static int buffer_append(json_buffer_t *buffer, const char *text)
{
    size_t length = strlen(text);
    if (buffer_reserve(buffer, length) < 0) {
        return -1;
    }
    memcpy(buffer->data + buffer->length, text, length + 1);
    buffer->length += length;
    return 0;
}

static int buffer_appendf(json_buffer_t *buffer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || buffer_reserve(buffer, (size_t)needed) < 0) {
        va_end(args);
        return -1;
    }
    vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length,
              format, args);
    va_end(args);
    buffer->length += (size_t)needed;
    return 0;
}

static int buffer_append_json_string(json_buffer_t *buffer, const char *value)
{
    if (buffer_append(buffer, "\"") < 0) {
        return -1;
    }
    if (value != NULL) {
        for (const unsigned char *cursor = (const unsigned char *)value;
             *cursor != '\0'; ++cursor) {
            char escape[8];
            switch (*cursor) {
            case '"':
                if (buffer_append(buffer, "\\\"") < 0) return -1;
                break;
            case '\\':
                if (buffer_append(buffer, "\\\\") < 0) return -1;
                break;
            case '\n':
                if (buffer_append(buffer, "\\n") < 0) return -1;
                break;
            case '\r':
                if (buffer_append(buffer, "\\r") < 0) return -1;
                break;
            case '\t':
                if (buffer_append(buffer, "\\t") < 0) return -1;
                break;
            default:
                if (*cursor < 0x20) {
                    snprintf(escape, sizeof(escape), "\\u%04x", *cursor);
                    if (buffer_append(buffer, escape) < 0) return -1;
                } else {
                    if (buffer_reserve(buffer, 1) < 0) return -1;
                    buffer->data[buffer->length++] = (char)*cursor;
                    buffer->data[buffer->length] = '\0';
                }
            }
        }
    }
    return buffer_append(buffer, "\"");
}

static const char *skip_ws(const char *cursor)
{
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

static const char *find_json_key(const char *json, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *found = strstr(json, pattern);
    if (found == NULL) {
        return NULL;
    }
    found += strlen(pattern);
    found = skip_ws(found);
    if (*found != ':') {
        return NULL;
    }
    return skip_ws(found + 1);
}

static int json_bool_field(const char *json, const char *key, bool *value_out)
{
    const char *value = find_json_key(json, key);
    if (value == NULL) {
        return -1;
    }
    if (strncmp(value, "true", 4) == 0) {
        *value_out = true;
        return 0;
    }
    if (strncmp(value, "false", 5) == 0) {
        *value_out = false;
        return 0;
    }
    return -1;
}

static int json_u64_field(const char *json, const char *key, uint64_t *value_out)
{
    const char *value = find_json_key(json, key);
    char *end = NULL;
    if (value == NULL || *value == '-') {
        return -1;
    }
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value) {
        return -1;
    }
    *value_out = (uint64_t)parsed;
    return 0;
}

static int json_i64_field(const char *json, const char *key, int64_t *value_out)
{
    const char *value = find_json_key(json, key);
    char *end = NULL;
    if (value == NULL) {
        return -1;
    }
    errno = 0;
    long long parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value) {
        return -1;
    }
    *value_out = (int64_t)parsed;
    return 0;
}

static int json_string_field(const char *json, const char *key,
                             char *buffer, size_t buffer_size)
{
    const char *value = find_json_key(json, key);
    if (value == NULL || *value != '"' || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    ++value;
    size_t used = 0;
    while (*value != '\0' && *value != '"') {
        char ch = *value++;
        if (ch == '\\') {
            ch = *value++;
            if (ch == 'n') ch = '\n';
            else if (ch == 'r') ch = '\r';
            else if (ch == 't') ch = '\t';
            else if (ch == '\0') return -1;
        }
        if (used + 1 >= buffer_size) {
            return -1;
        }
        buffer[used++] = ch;
    }
    if (*value != '"') {
        return -1;
    }
    buffer[used] = '\0';
    return 0;
}

static const char *json_object_field(const char *json, const char *key)
{
    const char *value = find_json_key(json, key);
    return value != NULL && *value == '{' ? value : NULL;
}

static const char *json_array_field(const char *json, const char *key)
{
    const char *value = find_json_key(json, key);
    return value != NULL && *value == '[' ? value : NULL;
}

static cubicle_error_code_t error_code_from_name(const char *name)
{
    for (int code = CUBICLE_OK; code <= CUBICLE_ERR_INTERNAL; ++code) {
        if (strcmp(cubicle_error_code_name((cubicle_error_code_t)code),
                   name) == 0) {
            return (cubicle_error_code_t)code;
        }
    }
    if (strcmp(name, "authentication") == 0) {
        return CUBICLE_ERR_AUTHENTICATION_FAILED;
    }
    return CUBICLE_ERR_PROTOCOL;
}

static const char *mode_name(cubicle_process_mode_t mode)
{
    return cubicle_process_mode_name(mode);
}

static const char *state_name(cubicle_process_state_t state)
{
    return cubicle_process_state_name(state);
}

static const char *stream_name(cubicle_stream_kind_t stream)
{
    switch (stream) {
    case CUBICLE_STREAM_STDOUT: return "stdout";
    case CUBICLE_STREAM_STDERR: return "stderr";
    case CUBICLE_STREAM_TTY: return "tty";
    default: return "unknown";
    }
}

static const char *attachment_mode_name(cubicle_attachment_mode_t mode)
{
    return mode == CUBICLE_ATTACHMENT_INTERACTIVE ? "interactive" : "observer";
}

static int process_mode_from_string(const char *text, cubicle_process_mode_t *out)
{
    if (strcmp(text, "stream") == 0) *out = CUBICLE_PROCESS_STREAM;
    else if (strcmp(text, "tty") == 0) *out = CUBICLE_PROCESS_TTY;
    else if (strcmp(text, "tty-captured-stderr") == 0) *out = CUBICLE_PROCESS_TTY_CAPTURED_STDERR;
    else return -1;
    return 0;
}

static int process_state_from_string(const char *text, cubicle_process_state_t *out)
{
    for (int state = CUBICLE_PROCESS_ALLOCATED; state <= CUBICLE_PROCESS_REMOVED; ++state) {
        if (strcmp(text, state_name((cubicle_process_state_t)state)) == 0) {
            *out = (cubicle_process_state_t)state;
            return 0;
        }
    }
    return -1;
}

static int event_type_from_string(const char *text, cubicle_event_type_t *out)
{
    static const char *names[] = {
        "workspace_created", "workspace_updated", "workspace_stopping",
        "workspace_stopped", "workspace_deleted", "process_started",
        "process_state_changed", "process_exited", "output_available",
        "client_attached", "client_detached", "controller_lost",
        "controller_recovered", "manager_recovered",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(text, names[i]) == 0) {
            *out = (cubicle_event_type_t)i;
            return 0;
        }
    }
    return -1;
}

static int channel_mask_from_string(const char *text, cubicle_channel_mask_t *out)
{
    cubicle_channel_mask_t mask = CUBICLE_CHANNEL_NONE;
    if (strstr(text, "stdin") != NULL) mask |= CUBICLE_CHANNEL_STDIN;
    if (strstr(text, "stdout") != NULL) mask |= CUBICLE_CHANNEL_STDOUT;
    if (strstr(text, "stderr") != NULL) mask |= CUBICLE_CHANNEL_STDERR;
    if (strstr(text, "tty") != NULL) mask |= CUBICLE_CHANNEL_TTY;
    *out = mask;
    return 0;
}

static int append_common_options(json_buffer_t *params,
                                 const cubicle_request_options_t *options)
{
    if (options == NULL) {
        return 0;
    }
    if (options->idempotency_key != NULL) {
        if (buffer_append(params, ",\"idempotency_key\":") < 0 ||
            buffer_append_json_string(params, options->idempotency_key) < 0) {
            return -1;
        }
    }
    if (options->timeout_ms > 0 &&
        buffer_appendf(params, ",\"timeout_ms\":%d", options->timeout_ms) < 0) {
        return -1;
    }
    if (options->deadline_ms > 0 &&
        buffer_appendf(params, ",\"deadline_ms\":%llu",
                       (unsigned long long)options->deadline_ms) < 0) {
        return -1;
    }
    return 0;
}

static cubicle_error_code_t rpc_call(cubicle_client_t *client,
                                     const char *method,
                                     const char *params,
                                     char **response_out)
{
    if (client == NULL || method == NULL || response_out == NULL ||
        client->transport == NULL || client->transport->vtable == NULL ||
        client->transport->vtable->request == NULL) {
        return set_client_error(client, CUBICLE_ERR_INVALID_ARGUMENT, 0,
                                "client transport does not support requests");
    }

    char request_id[32];
    snprintf(request_id, sizeof(request_id), "req-%llu",
             (unsigned long long)++client->next_request_id);

    json_buffer_t request = {0};
    if (buffer_append(&request, "{\"request_id\":") < 0 ||
        buffer_append_json_string(&request, request_id) < 0 ||
        buffer_append(&request, ",\"method\":") < 0 ||
        buffer_append_json_string(&request, method) < 0 ||
        buffer_append(&request, ",\"params\":") < 0 ||
        buffer_append(&request, params == NULL ? "{}" : params) < 0 ||
        buffer_append(&request, "}") < 0) {
        free(request.data);
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to build request");
    }

    void *response_data = NULL;
    size_t response_length = 0;
    cubicle_error_code_t result = client->transport->vtable->request(
        client->transport, request.data, request.length, &response_data,
        &response_length, &client->last_error);
    free(request.data);
    if (result != CUBICLE_OK) {
        return result;
    }

    char *response = calloc(response_length + 1, 1);
    if (response == NULL) {
        if (client->transport->vtable->response_free != NULL) {
            client->transport->vtable->response_free(client->transport,
                                                     response_data);
        }
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM,
                                "failed to allocate response");
    }
    if (response_length > 0) {
        memcpy(response, response_data, response_length);
    }
    if (client->transport->vtable->response_free != NULL) {
        client->transport->vtable->response_free(client->transport,
                                                 response_data);
    }

    char response_request_id[32];
    bool success = false;
    if (json_string_field(response, "request_id", response_request_id,
                          sizeof(response_request_id)) < 0 ||
        strcmp(response_request_id, request_id) != 0 ||
        json_bool_field(response, "success", &success) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "malformed response envelope");
    }

    if (!success) {
        const char *error_object = json_object_field(response, "error");
        char code_name[64] = "protocol";
        char message[CUBICLE_ERROR_MESSAGE_MAX] = "";
        bool retryable = false;
        int64_t system_errno = 0;
        if (error_object != NULL) {
            json_string_field(error_object, "code", code_name,
                              sizeof(code_name));
            json_string_field(error_object, "message", message,
                              sizeof(message));
            json_bool_field(error_object, "retryable", &retryable);
            json_i64_field(error_object, "system_errno", &system_errno);
        }
        cubicle_error_code_t code = error_code_from_name(code_name);
        set_error(&client->last_error, code, (int)system_errno, retryable,
                  message);
        free(response);
        return code;
    }

    *response_out = response;
    memset(&client->last_error, 0, sizeof(client->last_error));
    return CUBICLE_OK;
}

static const char *result_object(cubicle_client_t *client, const char *response)
{
    const char *result = json_object_field(response, "result");
    if (result == NULL) {
        set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                         "response missing result object");
    }
    return result;
}

static int parse_endpoint(const char *json, cubicle_endpoint_t *endpoint)
{
    memset(endpoint, 0, sizeof(*endpoint));
    json_string_field(json, "uri", endpoint->uri, sizeof(endpoint->uri));
    json_string_field(json, "server_identity", endpoint->server_identity,
                      sizeof(endpoint->server_identity));
    return endpoint->uri[0] == '\0' ? -1 : 0;
}

static int parse_page(const char *json, cubicle_page_info_t *page)
{
    if (page == NULL) {
        return 0;
    }
    memset(page, 0, sizeof(*page));
    json_string_field(json, "continuation_token", page->continuation_token,
                      sizeof(page->continuation_token));
    json_bool_field(json, "has_more", &page->has_more);
    return 0;
}

static int parse_workspace_info(const char *json, cubicle_workspace_info_t *out)
{
    memset(out, 0, sizeof(*out));
    json_string_field(json, "manager_id", out->manager_id, sizeof(out->manager_id));
    if (json_string_field(json, "id", out->id, sizeof(out->id)) < 0 ||
        json_string_field(json, "name", out->name, sizeof(out->name)) < 0) {
        return -1;
    }
    json_u64_field(json, "created_at_ms", &out->created_at_ms);
    json_u64_field(json, "updated_at_ms", &out->updated_at_ms);
    json_u64_field(json, "process_count", &out->process_count);
    json_u64_field(json, "running_process_count", &out->running_process_count);
    return 0;
}

static int parse_process_info(const char *json, cubicle_process_info_t *out)
{
    char text[64];
    memset(out, 0, sizeof(*out));
    json_string_field(json, "manager_id", out->manager_id, sizeof(out->manager_id));
    json_string_field(json, "workspace_id", out->workspace_id, sizeof(out->workspace_id));
    if (json_string_field(json, "id", out->id, sizeof(out->id)) < 0) return -1;
    json_string_field(json, "friendly_name", out->friendly_name,
                      sizeof(out->friendly_name));
    if (json_string_field(json, "mode", text, sizeof(text)) == 0) {
        process_mode_from_string(text, &out->mode);
    }
    if (json_string_field(json, "state", text, sizeof(text)) == 0) {
        process_state_from_string(text, &out->state);
    }
    int64_t parsed = 0;
    if (json_i64_field(json, "exit_code", &parsed) == 0) out->exit_code = (int)parsed;
    if (json_i64_field(json, "termination_signal", &parsed) == 0) out->termination_signal = (int)parsed;
    json_bool_field(json, "has_exit_status", &out->has_exit_status);
    json_u64_field(json, "stdout_offset", &out->stdout_offset);
    json_u64_field(json, "stderr_offset", &out->stderr_offset);
    json_u64_field(json, "tty_offset", &out->tty_offset);
    json_u64_field(json, "created_at_ms", &out->created_at_ms);
    json_u64_field(json, "started_at_ms", &out->started_at_ms);
    json_u64_field(json, "exited_at_ms", &out->exited_at_ms);
    json_i64_field(json, "local_pid", &out->local_pid);
    json_i64_field(json, "local_pgid", &out->local_pgid);
    return 0;
}

static int parse_key_info(const char *json, cubicle_workspace_key_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (json_string_field(json, "key_id", out->key_id, sizeof(out->key_id)) < 0) return -1;
    json_string_field(json, "fingerprint", out->fingerprint, sizeof(out->fingerprint));
    json_string_field(json, "label", out->label, sizeof(out->label));
    json_u64_field(json, "capabilities", &out->capabilities);
    json_u64_field(json, "created_at_ms", &out->created_at_ms);
    json_u64_field(json, "revoked_at_ms", &out->revoked_at_ms);
    return 0;
}

static int parse_event(const char *json, cubicle_event_t *out)
{
    char type[64];
    memset(out, 0, sizeof(*out));
    json_u64_field(json, "global_sequence", &out->global_sequence);
    json_u64_field(json, "workspace_sequence", &out->workspace_sequence);
    json_u64_field(json, "timestamp_ms", &out->timestamp_ms);
    if (json_string_field(json, "type", type, sizeof(type)) == 0) {
        event_type_from_string(type, &out->type);
    }
    json_string_field(json, "workspace_id", out->workspace_id,
                      sizeof(out->workspace_id));
    json_string_field(json, "process_id", out->process_id,
                      sizeof(out->process_id));
    json_string_field(json, "payload", out->payload, sizeof(out->payload));
    return 0;
}

static size_t count_array_objects(const char *array)
{
    size_t count = 0;
    int depth = 0;
    bool in_string = false;
    for (const char *cursor = array; *cursor != '\0'; ++cursor) {
        if (*cursor == '"' && (cursor == array || cursor[-1] != '\\')) {
            in_string = !in_string;
        } else if (!in_string && *cursor == '{') {
            if (depth++ == 0) ++count;
        } else if (!in_string && *cursor == '}') {
            --depth;
        } else if (!in_string && *cursor == ']' && depth == 0) {
            break;
        }
    }
    return count;
}

static const char *next_array_object(const char *cursor, size_t *length_out)
{
    cursor = strchr(cursor, '{');
    if (cursor == NULL) {
        return NULL;
    }
    int depth = 0;
    bool in_string = false;
    for (const char *end = cursor; *end != '\0'; ++end) {
        if (*end == '"' && (end == cursor || end[-1] != '\\')) {
            in_string = !in_string;
        } else if (!in_string && *end == '{') {
            ++depth;
        } else if (!in_string && *end == '}') {
            if (--depth == 0) {
                *length_out = (size_t)(end - cursor + 1);
                return cursor;
            }
        }
    }
    return NULL;
}

static char *copy_object_slice(const char *object, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, object, length);
    copy[length] = '\0';
    return copy;
}

static cubicle_error_code_t rpc_object(cubicle_client_t *client,
                                       const char *method,
                                       const char *params,
                                       char **object_out)
{
    char *response = NULL;
    cubicle_error_code_t code = rpc_call(client, method, params, &response);
    if (code != CUBICLE_OK) {
        return code;
    }
    const char *object = result_object(client, response);
    if (object == NULL) {
        free(response);
        return CUBICLE_ERR_PROTOCOL;
    }
    *object_out = response;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_client_connect(const cubicle_client_options_t *options,
                                            cubicle_client_t **client_out)
{
    if (options == NULL || client_out == NULL || options->transport == NULL ||
        options->transport->vtable == NULL ||
        options->transport->vtable->connect == NULL) {
        return CUBICLE_ERR_INVALID_ARGUMENT;
    }
    cubicle_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return CUBICLE_ERR_INTERNAL;
    }
    client->endpoint = options->endpoint;
    client->connect_timeout_ms = options->connect_timeout_ms;
    client->request_timeout_ms = options->request_timeout_ms;
    client->transport = options->transport;
    client->next_request_id = 0;
    cubicle_error_code_t result = client->transport->vtable->connect(
        client->transport, &client->endpoint, &client->last_error);
    if (result != CUBICLE_OK) {
        free(client);
        return result;
    }
    *client_out = client;
    return CUBICLE_OK;
}

void cubicle_client_disconnect(cubicle_client_t *client)
{
    if (client == NULL) return;
    if (client->transport != NULL && client->transport->vtable != NULL) {
        if (client->transport->vtable->close != NULL) client->transport->vtable->close(client->transport);
        if (client->transport->vtable->destroy != NULL) client->transport->vtable->destroy(client->transport);
    }
    free(client);
}

const cubicle_error_t *cubicle_client_last_error(const cubicle_client_t *client)
{
    return client == NULL ? NULL : &client->last_error;
}

cubicle_error_code_t cubicle_client_session_info(const cubicle_client_t *client,
                                                 cubicle_session_info_t *session_out)
{
    if (client == NULL || session_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    *session_out = client->session;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_ping(cubicle_client_t *client,
                                          cubicle_manager_ping_result_t *result_out)
{
    if (client == NULL || result_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.ping", "{}", &response);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(result_out, 0, sizeof(*result_out));
    if (json_string_field(result, "manager_id", result_out->manager_id,
                          sizeof(result_out->manager_id)) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "manager.ping result missing manager_id");
    }
    uint64_t value = 0;
    if (json_u64_field(result, "protocol_major", &value) == 0) result_out->protocol_major = (uint32_t)value;
    if (json_u64_field(result, "protocol_minor", &value) == 0) result_out->protocol_minor = (uint32_t)value;
    json_u64_field(result, "server_time_ms", &result_out->server_time_ms);
    json_u64_field(result, "uptime_ms", &result_out->uptime_ms);
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_status(cubicle_client_t *client,
                                            cubicle_manager_status_t *status_out)
{
    if (client == NULL || status_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.status", "{}", &response);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(status_out, 0, sizeof(*status_out));
    if (json_string_field(result, "manager_id", status_out->manager_id,
                          sizeof(status_out->manager_id)) < 0) {
        free(response);
        return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0,
                                "manager.status result missing manager_id");
    }
    uint64_t value = 0;
    if (json_u64_field(result, "protocol_major", &value) == 0) status_out->protocol_major = (uint32_t)value;
    if (json_u64_field(result, "protocol_minor", &value) == 0) status_out->protocol_minor = (uint32_t)value;
    json_u64_field(result, "capabilities", &status_out->capabilities);
    json_u64_field(result, "started_at_ms", &status_out->started_at_ms);
    json_u64_field(result, "server_time_ms", &status_out->server_time_ms);
    json_u64_field(result, "workspace_count", &status_out->workspace_count);
    json_u64_field(result, "process_count", &status_out->process_count);
    json_u64_field(result, "controller_count", &status_out->controller_count);
    json_u64_field(result, "active_client_sessions", &status_out->active_client_sessions);
    free(response);
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_manager_reconcile(cubicle_client_t *client)
{
    if (client == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.reconcile", "{}", &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_manager_shutdown(cubicle_client_t *client,
                                              bool stop_managed_processes)
{
    if (client == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[64];
    snprintf(params, sizeof(params), "{\"stop_managed_processes\":%s}",
             stop_managed_processes ? "true" : "false");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "manager.shutdown", params, &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_create(cubicle_client_t *client,
    const cubicle_workspace_create_options_t *options,
    cubicle_workspace_info_t *workspace_out)
{
    if (client == NULL || options == NULL || options->name == NULL ||
        options->name[0] == '\0' || workspace_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    if (buffer_append(&params, "{\"name\":") < 0 ||
        buffer_append_json_string(&params, options->name) < 0 ||
        (options->initial_owner_label != NULL &&
         (buffer_append(&params, ",\"initial_owner_label\":") < 0 ||
          buffer_append_json_string(&params, options->initial_owner_label) < 0)) ||
        append_common_options(&params, &options->request) < 0 ||
        buffer_append(&params, "}") < 0) {
        free(params.data);
        return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to build request");
    }
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.create", params.data, &response);
    free(params.data);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    code = parse_workspace_info(result, workspace_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid workspace result");
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_get(cubicle_client_t *client,
    const char *workspace_id_or_name, cubicle_workspace_info_t *workspace_out)
{
    if (client == NULL || workspace_id_or_name == NULL ||
        workspace_id_or_name[0] == '\0' || workspace_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{\"workspace\":");
    buffer_append_json_string(&params, workspace_id_or_name);
    buffer_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.get", params.data, &response);
    free(params.data);
    if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    code = parse_workspace_info(result, workspace_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid workspace result");
    free(response);
    return code;
}

static cubicle_error_code_t parse_workspace_list(cubicle_client_t *client,
    const char *result, cubicle_workspace_info_t **workspaces_out,
    size_t *count_out, cubicle_page_info_t *page_out)
{
    const char *array = json_array_field(result, "workspaces");
    if (array == NULL) return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing workspaces array");
    size_t count = count_array_objects(array);
    cubicle_workspace_info_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count > 0 && items == NULL) return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to allocate workspaces");
    const char *cursor = array;
    for (size_t i = 0; i < count; ++i) {
        size_t length = 0;
        const char *object = next_array_object(cursor, &length);
        char *copy = copy_object_slice(object, length);
        if (copy == NULL || parse_workspace_info(copy, &items[i]) < 0) {
            free(copy); free(items);
            return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid workspace item");
        }
        free(copy);
        cursor = object + length;
    }
    parse_page(result, page_out);
    *workspaces_out = items;
    *count_out = count;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_workspace_list(cubicle_client_t *client,
    const cubicle_workspace_list_options_t *options,
    cubicle_workspace_info_t **workspaces_out, size_t *count_out,
    cubicle_page_info_t *page_out)
{
    if (client == NULL || workspaces_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{");
    bool comma = false;
    if (options != NULL && options->name_prefix != NULL) {
        buffer_append(&params, "\"name_prefix\":");
        buffer_append_json_string(&params, options->name_prefix);
        comma = true;
    }
    if (options != NULL && options->page.limit > 0) {
        buffer_appendf(&params, "%s\"limit\":%zu", comma ? "," : "", options->page.limit);
        comma = true;
    }
    if (options != NULL && options->page.continuation_token != NULL) {
        buffer_append(&params, comma ? ",\"continuation_token\":" : "\"continuation_token\":");
        buffer_append_json_string(&params, options->page.continuation_token);
    }
    buffer_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.list", params.data, &response);
    free(params.data);
    if (code != CUBICLE_OK) return code;
    code = parse_workspace_list(client, result_object(client, response), workspaces_out, count_out, page_out);
    free(response);
    return code;
}

static cubicle_error_code_t simple_string_rpc(cubicle_client_t *client,
    const char *method, const char *key, const char *value,
    const cubicle_request_options_t *request_options)
{
    if (client == NULL || value == NULL || value[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{");
    buffer_append_json_string(&params, key);
    buffer_append(&params, ":");
    buffer_append_json_string(&params, value);
    append_common_options(&params, request_options);
    buffer_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, method, params.data, &response);
    free(params.data);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_rename(cubicle_client_t *client,
    const char *workspace_id, const char *new_name,
    const cubicle_request_options_t *request_options)
{
    if (client == NULL || workspace_id == NULL || new_name == NULL ||
        workspace_id[0] == '\0' || new_name[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{\"workspace_id\":");
    buffer_append_json_string(&params, workspace_id);
    buffer_append(&params, ",\"new_name\":");
    buffer_append_json_string(&params, new_name);
    append_common_options(&params, request_options);
    buffer_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.rename", params.data, &response);
    free(params.data); free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_stop(cubicle_client_t *client,
    const char *workspace_id, const cubicle_workspace_stop_options_t *options)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[256];
    snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\",\"grace_period_ms\":%d,\"force_after_grace\":%s}",
             workspace_id, options == NULL ? 0 : options->grace_period_ms,
             options != NULL && options->force_after_grace ? "true" : "false");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.stop", params, &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_delete(cubicle_client_t *client,
    const char *workspace_id, const cubicle_workspace_delete_options_t *options)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[256];
    snprintf(params, sizeof(params), "{\"workspace_id\":\"%s\",\"stop_running_processes\":%s,\"remove_retained_processes\":%s}",
             workspace_id, options != NULL && options->stop_running_processes ? "true" : "false",
             options != NULL && options->remove_retained_processes ? "true" : "false");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.delete", params, &response);
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_key_add(cubicle_client_t *client,
    const char *workspace_id, const unsigned char *public_key,
    size_t public_key_length, const char *label,
    cubicle_capability_mask_t capabilities, cubicle_workspace_key_info_t *key_out)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0' ||
        public_key == NULL || public_key_length == 0 || key_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{\"workspace_id\":");
    buffer_append_json_string(&params, workspace_id);
    buffer_append(&params, ",\"public_key\":\"");
    for (size_t i = 0; i < public_key_length; ++i) buffer_appendf(&params, "%02x", public_key[i]);
    buffer_append(&params, "\",\"label\":");
    buffer_append_json_string(&params, label == NULL ? "" : label);
    buffer_appendf(&params, ",\"capabilities\":%llu}", (unsigned long long)capabilities);
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.key.add", params.data, &response);
    free(params.data);
    if (code != CUBICLE_OK) return code;
    code = parse_key_info(result_object(client, response), key_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid key result");
    free(response);
    return code;
}

cubicle_error_code_t cubicle_workspace_key_list(cubicle_client_t *client,
    const char *workspace_id, cubicle_workspace_key_info_t **keys_out,
    size_t *count_out)
{
    if (client == NULL || workspace_id == NULL || workspace_id[0] == '\0' ||
        keys_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{\"workspace_id\":");
    buffer_append_json_string(&params, workspace_id);
    buffer_append(&params, "}");
    char *response = NULL;
    cubicle_error_code_t code = rpc_object(client, "workspace.key.list", params.data, &response);
    free(params.data);
    if (code != CUBICLE_OK) return code;
    const char *array = json_array_field(result_object(client, response), "keys");
    if (array == NULL) { free(response); return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing keys array"); }
    size_t count = count_array_objects(array);
    cubicle_workspace_key_info_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    const char *cursor = array;
    for (size_t i = 0; i < count; ++i) {
        size_t length = 0; const char *object = next_array_object(cursor, &length);
        char *copy = copy_object_slice(object, length);
        if (copy == NULL || parse_key_info(copy, &items[i]) < 0) {
            free(copy); free(items); free(response);
            return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid key item");
        }
        free(copy); cursor = object + length;
    }
    *keys_out = items; *count_out = count; free(response); return CUBICLE_OK;
}

cubicle_error_code_t cubicle_workspace_key_set_capabilities(cubicle_client_t *client,
    const char *workspace_id, const char *key_id, cubicle_capability_mask_t capabilities)
{
    if (client == NULL || workspace_id == NULL || key_id == NULL ||
        workspace_id[0] == '\0' || key_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{\"workspace_id\":"); buffer_append_json_string(&params, workspace_id);
    buffer_append(&params, ",\"key_id\":"); buffer_append_json_string(&params, key_id);
    buffer_appendf(&params, ",\"capabilities\":%llu}", (unsigned long long)capabilities);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "workspace.key.update", params.data, &response);
    free(params.data); free(response); return code;
}

cubicle_error_code_t cubicle_workspace_key_revoke(cubicle_client_t *client,
    const char *workspace_id, const char *key_id)
{
    if (client == NULL || workspace_id == NULL || key_id == NULL ||
        workspace_id[0] == '\0' || key_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{\"workspace_id\":"); buffer_append_json_string(&params, workspace_id);
    buffer_append(&params, ",\"key_id\":"); buffer_append_json_string(&params, key_id);
    buffer_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "workspace.key.revoke", params.data, &response);
    free(params.data); free(response); return code;
}

static cubicle_error_code_t parse_process_list(cubicle_client_t *client,
    const char *result, cubicle_process_info_t **processes_out,
    size_t *count_out, cubicle_page_info_t *page_out)
{
    const char *array = json_array_field(result, "processes");
    if (array == NULL) return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing processes array");
    size_t count = count_array_objects(array);
    cubicle_process_info_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    const char *cursor = array;
    for (size_t i = 0; i < count; ++i) {
        size_t length = 0; const char *object = next_array_object(cursor, &length);
        char *copy = copy_object_slice(object, length);
        if (copy == NULL || parse_process_info(copy, &items[i]) < 0) {
            free(copy); free(items); return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid process item");
        }
        free(copy); cursor = object + length;
    }
    parse_page(result, page_out);
    *processes_out = items; *count_out = count; return CUBICLE_OK;
}

cubicle_error_code_t cubicle_process_start(cubicle_client_t *client,
    const cubicle_process_start_options_t *options,
    cubicle_process_info_t *process_out)
{
    if (client == NULL || options == NULL || process_out == NULL ||
        options->workspace_id == NULL || options->argv == NULL ||
        options->argc == 0) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0};
    buffer_append(&params, "{\"workspace_id\":"); buffer_append_json_string(&params, options->workspace_id);
    if (options->friendly_name != NULL) { buffer_append(&params, ",\"friendly_name\":"); buffer_append_json_string(&params, options->friendly_name); }
    buffer_append(&params, ",\"mode\":"); buffer_append_json_string(&params, mode_name(options->mode));
    buffer_append(&params, ",\"stdin_policy\":"); buffer_append_json_string(&params, options->stdin_policy == CUBICLE_STDIN_EOF ? "eof" : "open");
    if (options->cwd != NULL) { buffer_append(&params, ",\"cwd\":"); buffer_append_json_string(&params, options->cwd); }
    buffer_append(&params, ",\"argv\":[");
    for (size_t i = 0; i < options->argc; ++i) {
        if (i > 0) buffer_append(&params, ",");
        buffer_append_json_string(&params, options->argv[i]);
    }
    buffer_append(&params, "]");
    if (options->tty_rows > 0) buffer_appendf(&params, ",\"tty_rows\":%u", options->tty_rows);
    if (options->tty_cols > 0) buffer_appendf(&params, ",\"tty_cols\":%u", options->tty_cols);
    append_common_options(&params, &options->request);
    buffer_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.start", params.data, &response);
    free(params.data); if (code != CUBICLE_OK) return code;
    code = parse_process_info(result_object(client, response), process_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid process result");
    free(response); return code;
}

cubicle_error_code_t cubicle_process_get(cubicle_client_t *client,
    const char *process_id_or_name, const char *workspace_id,
    cubicle_process_info_t *process_out)
{
    if (client == NULL || process_id_or_name == NULL || process_id_or_name[0] == '\0' || process_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0}; buffer_append(&params, "{\"process\":"); buffer_append_json_string(&params, process_id_or_name);
    if (workspace_id != NULL) { buffer_append(&params, ",\"workspace_id\":"); buffer_append_json_string(&params, workspace_id); }
    buffer_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.get", params.data, &response);
    free(params.data); if (code != CUBICLE_OK) return code;
    code = parse_process_info(result_object(client, response), process_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid process result");
    free(response); return code;
}

cubicle_error_code_t cubicle_process_list(cubicle_client_t *client,
    const cubicle_process_filter_t *filter, cubicle_process_info_t **processes_out,
    size_t *count_out, cubicle_page_info_t *page_out)
{
    if (client == NULL || processes_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0}; buffer_append(&params, "{"); bool comma = false;
    if (filter != NULL && filter->workspace_id != NULL) { buffer_append(&params, "\"workspace_id\":"); buffer_append_json_string(&params, filter->workspace_id); comma = true; }
    if (filter != NULL && filter->name_prefix != NULL) { buffer_append(&params, comma ? ",\"name_prefix\":" : "\"name_prefix\":"); buffer_append_json_string(&params, filter->name_prefix); comma = true; }
    if (filter != NULL && filter->include_completed) { buffer_append(&params, comma ? ",\"include_completed\":true" : "\"include_completed\":true"); }
    buffer_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.list", params.data, &response);
    free(params.data); if (code != CUBICLE_OK) return code;
    code = parse_process_list(client, result_object(client, response), processes_out, count_out, page_out);
    free(response); return code;
}

cubicle_error_code_t cubicle_process_signal(cubicle_client_t *client,
    const char *process_id, int signal_number)
{
    if (client == NULL || process_id == NULL || process_id[0] == '\0' || signal_number <= 0) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0}; buffer_append(&params, "{\"process_id\":"); buffer_append_json_string(&params, process_id);
    buffer_appendf(&params, ",\"signal_number\":%d}", signal_number);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.signal", params.data, &response);
    free(params.data); free(response); return code;
}

cubicle_error_code_t cubicle_process_terminate(cubicle_client_t *client,
    const char *process_id, const cubicle_process_terminate_options_t *options)
{
    if (client == NULL || process_id == NULL || process_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    char params[256]; snprintf(params, sizeof(params), "{\"process_id\":\"%s\",\"grace_period_ms\":%d,\"force_after_grace\":%s}",
                               process_id, options == NULL ? 0 : options->grace_period_ms,
                               options != NULL && options->force_after_grace ? "true" : "false");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.terminate", params, &response);
    free(response); return code;
}

cubicle_error_code_t cubicle_process_kill(cubicle_client_t *client,
                                          const char *process_id)
{
    return simple_string_rpc(client, "process.kill", "process_id", process_id, NULL);
}

cubicle_error_code_t cubicle_process_wait(cubicle_client_t *client,
    const char *process_id, int timeout_ms, cubicle_process_info_t *process_out)
{
    if (client == NULL || process_id == NULL || process_id[0] == '\0' || process_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0}; buffer_append(&params, "{\"process_id\":"); buffer_append_json_string(&params, process_id);
    buffer_appendf(&params, ",\"timeout_ms\":%d}", timeout_ms);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.wait", params.data, &response);
    free(params.data); if (code != CUBICLE_OK) return code;
    code = parse_process_info(result_object(client, response), process_out) == 0 ? CUBICLE_OK :
           set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "invalid process result");
    free(response); return code;
}

cubicle_error_code_t cubicle_process_remove(cubicle_client_t *client,
                                            const char *process_id)
{
    return simple_string_rpc(client, "process.remove", "process_id", process_id, NULL);
}

cubicle_error_code_t cubicle_process_read_output(cubicle_client_t *client,
    const char *process_id, cubicle_stream_kind_t stream, uint64_t offset,
    size_t maximum_length, cubicle_output_chunk_t *chunk_out)
{
    if (client == NULL || process_id == NULL || process_id[0] == '\0' || chunk_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0}; buffer_append(&params, "{\"process_id\":"); buffer_append_json_string(&params, process_id);
    buffer_append(&params, ",\"stream\":"); buffer_append_json_string(&params, stream_name(stream));
    buffer_appendf(&params, ",\"offset\":%llu,\"maximum_length\":%zu}", (unsigned long long)offset, maximum_length);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "process.read_output", params.data, &response);
    free(params.data); if (code != CUBICLE_OK) return code;
    const char *result = result_object(client, response);
    memset(chunk_out, 0, sizeof(*chunk_out));
    json_u64_field(result, "start_offset", &chunk_out->start_offset);
    json_u64_field(result, "next_offset", &chunk_out->next_offset);
    json_bool_field(result, "end_of_stream", &chunk_out->end_of_stream);
    char data[4096];
    if (json_string_field(result, "data", data, sizeof(data)) == 0) {
        chunk_out->length = strlen(data);
        chunk_out->data = malloc(chunk_out->length);
        if (chunk_out->length > 0 && chunk_out->data == NULL) { free(response); return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to allocate output"); }
        memcpy(chunk_out->data, data, chunk_out->length);
    }
    free(response); return CUBICLE_OK;
}

cubicle_error_code_t cubicle_attachment_request(cubicle_client_t *client,
    const cubicle_attachment_request_t *request,
    cubicle_attachment_grant_t *grant_out)
{
    if (client == NULL || request == NULL || request->process_id == NULL ||
        request->process_id[0] == '\0' || grant_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0}; buffer_append(&params, "{\"process_id\":"); buffer_append_json_string(&params, request->process_id);
    buffer_appendf(&params, ",\"channels\":%u,\"mode\":", (unsigned)request->channels);
    buffer_append_json_string(&params, attachment_mode_name(request->mode));
    buffer_appendf(&params, ",\"stdout_offset\":%llu,\"stderr_offset\":%llu,\"tty_offset\":%llu,\"rows\":%u,\"cols\":%u}",
                   (unsigned long long)request->stdout_offset, (unsigned long long)request->stderr_offset,
                   (unsigned long long)request->tty_offset, request->rows, request->cols);
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "attachment.request", params.data, &response);
    free(params.data); if (code != CUBICLE_OK) return code;
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

cubicle_error_code_t cubicle_events_list(cubicle_client_t *client,
    const cubicle_event_query_t *query, cubicle_event_t **events_out,
    size_t *count_out)
{
    if (client == NULL || events_out == NULL || count_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    json_buffer_t params = {0}; buffer_append(&params, "{"); bool comma = false;
    if (query != NULL && query->workspace_id != NULL) { buffer_append(&params, "\"workspace_id\":"); buffer_append_json_string(&params, query->workspace_id); comma = true; }
    if (query != NULL && query->process_id != NULL) { buffer_append(&params, comma ? ",\"process_id\":" : "\"process_id\":"); buffer_append_json_string(&params, query->process_id); comma = true; }
    if (query != NULL) buffer_appendf(&params, "%s\"after_sequence\":%llu,\"limit\":%zu", comma ? "," : "", (unsigned long long)query->after_sequence, query->limit);
    buffer_append(&params, "}");
    char *response = NULL; cubicle_error_code_t code = rpc_object(client, "events.list", params.data, &response);
    free(params.data); if (code != CUBICLE_OK) return code;
    const char *array = json_array_field(result_object(client, response), "events");
    if (array == NULL) { free(response); return set_client_error(client, CUBICLE_ERR_PROTOCOL, 0, "missing events array"); }
    size_t count = count_array_objects(array);
    cubicle_event_t *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    const char *cursor = array;
    for (size_t i = 0; i < count; ++i) {
        size_t length = 0; const char *object = next_array_object(cursor, &length);
        char *copy = copy_object_slice(object, length);
        if (copy == NULL) { free(items); free(response); return set_client_error(client, CUBICLE_ERR_INTERNAL, ENOMEM, "failed to parse events"); }
        parse_event(copy, &items[i]); free(copy); cursor = object + length;
    }
    *events_out = items; *count_out = count; free(response); return CUBICLE_OK;
}

cubicle_error_code_t cubicle_signer_create(const cubicle_signer_callbacks_t *callbacks,
                                           void *context,
                                           cubicle_signer_t **signer_out)
{
    if (callbacks == NULL || callbacks->sign == NULL || signer_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    cubicle_signer_t *signer = calloc(1, sizeof(*signer));
    if (signer == NULL) return CUBICLE_ERR_INTERNAL;
    signer->callbacks = *callbacks;
    signer->context = context;
    *signer_out = signer;
    return CUBICLE_OK;
}

cubicle_error_code_t cubicle_signer_from_private_key_file(const char *path,
    cubicle_signer_t **signer_out, cubicle_error_t *error)
{
    if (path == NULL || signer_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(error, CUBICLE_ERR_UNSUPPORTED, 0, false,
                     "private key file signers are not implemented");
}

cubicle_error_code_t cubicle_signer_from_ssh_agent(const char *public_key_fingerprint,
    cubicle_signer_t **signer_out, cubicle_error_t *error)
{
    if (public_key_fingerprint == NULL || signer_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(error, CUBICLE_ERR_UNSUPPORTED, 0, false,
                     "SSH agent signers are not implemented");
}

void cubicle_signer_destroy(cubicle_signer_t *signer)
{
    if (signer == NULL) return;
    if (signer->callbacks.destroy != NULL) signer->callbacks.destroy(signer->context);
    free(signer);
}

cubicle_error_code_t cubicle_attachment_connect(const cubicle_attachment_grant_t *grant,
    const cubicle_attachment_options_t *options, cubicle_attachment_t **attachment_out)
{
    (void)options;
    if (grant == NULL || attachment_out == NULL || grant->grant_id[0] == '\0') return CUBICLE_ERR_INVALID_ARGUMENT;
    return CUBICLE_ERR_UNSUPPORTED;
}

ssize_t cubicle_attachment_read(cubicle_attachment_t *attachment, void *buffer, size_t length)
{
    (void)buffer; (void)length;
    if (attachment == NULL) return -1;
    set_error(&attachment->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "attachment read is not implemented");
    return -1;
}

ssize_t cubicle_attachment_write(cubicle_attachment_t *attachment, const void *buffer, size_t length)
{
    (void)buffer; (void)length;
    if (attachment == NULL) return -1;
    set_error(&attachment->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "attachment write is not implemented");
    return -1;
}

cubicle_error_code_t cubicle_attachment_resize(cubicle_attachment_t *attachment,
                                               unsigned int rows, unsigned int cols)
{
    (void)rows; (void)cols;
    if (attachment == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(&attachment->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "attachment resize is not implemented");
}

cubicle_error_code_t cubicle_attachment_close_input(cubicle_attachment_t *attachment)
{
    if (attachment == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(&attachment->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "attachment close input is not implemented");
}

const cubicle_error_t *cubicle_attachment_last_error(const cubicle_attachment_t *attachment)
{
    return attachment == NULL ? NULL : &attachment->last_error;
}

void cubicle_attachment_disconnect(cubicle_attachment_t *attachment)
{
    free(attachment);
}

cubicle_error_code_t cubicle_events_subscribe(cubicle_client_t *client,
    const cubicle_event_query_t *query,
    cubicle_event_subscription_t **subscription_out)
{
    (void)query;
    if (client == NULL || subscription_out == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return unsupported_client(client, "event subscriptions are not implemented");
}

cubicle_error_code_t cubicle_events_next(cubicle_event_subscription_t *subscription,
                                         int timeout_ms,
                                         cubicle_event_t *event_out)
{
    (void)timeout_ms; (void)event_out;
    if (subscription == NULL) return CUBICLE_ERR_INVALID_ARGUMENT;
    return set_error(&subscription->last_error, CUBICLE_ERR_UNSUPPORTED, 0, false, "event subscriptions are not implemented");
}

const cubicle_error_t *cubicle_events_subscription_last_error(
    const cubicle_event_subscription_t *subscription)
{
    return subscription == NULL ? NULL : &subscription->last_error;
}

void cubicle_events_unsubscribe(cubicle_event_subscription_t *subscription)
{
    free(subscription);
}

void cubicle_workspace_list_free(cubicle_workspace_info_t *workspaces) { free(workspaces); }
void cubicle_workspace_key_list_free(cubicle_workspace_key_info_t *keys) { free(keys); }
void cubicle_process_list_free(cubicle_process_info_t *processes) { free(processes); }
void cubicle_output_chunk_free(cubicle_output_chunk_t *chunk) { if (chunk != NULL) { free(chunk->data); memset(chunk, 0, sizeof(*chunk)); } }
void cubicle_events_free(cubicle_event_t *events) { free(events); }
