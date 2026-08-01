#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *skip_ws(const char *cursor)
{
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

const char *find_json_key(const char *json, const char *key)
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

int json_bool_field(const char *json, const char *key, bool *value_out)
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

int json_u64_field(const char *json, const char *key, uint64_t *value_out)
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

int json_i64_field(const char *json, const char *key, int64_t *value_out)
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

int json_string_field(const char *json, const char *key,
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

const char *json_object_field(const char *json, const char *key)
{
    const char *value = find_json_key(json, key);
    return value != NULL && *value == '{' ? value : NULL;
}

const char *json_array_field(const char *json, const char *key)
{
    const char *value = find_json_key(json, key);
    return value != NULL && *value == '[' ? value : NULL;
}

cubicle_error_code_t error_code_from_name(const char *name)
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

const char *mode_name(cubicle_process_mode_t mode)
{
    return cubicle_process_mode_name(mode);
}

const char *state_name(cubicle_process_state_t state)
{
    return cubicle_process_state_name(state);
}

const char *stream_name(cubicle_stream_kind_t stream)
{
    switch (stream) {
    case CUBICLE_STREAM_STDOUT: return "stdout";
    case CUBICLE_STREAM_STDERR: return "stderr";
    case CUBICLE_STREAM_TTY: return "tty";
    default: return "unknown";
    }
}

const char *attachment_mode_name(cubicle_attachment_mode_t mode)
{
    return mode == CUBICLE_ATTACHMENT_INTERACTIVE ? "interactive" : "observer";
}

int process_mode_from_string(const char *text, cubicle_process_mode_t *out)
{
    if (strcmp(text, "stream") == 0) *out = CUBICLE_PROCESS_STREAM;
    else if (strcmp(text, "tty") == 0) *out = CUBICLE_PROCESS_TTY;
    else if (strcmp(text, "tty-captured-stderr") == 0) *out = CUBICLE_PROCESS_TTY_CAPTURED_STDERR;
    else return -1;
    return 0;
}

int process_state_from_string(const char *text, cubicle_process_state_t *out)
{
    for (int state = CUBICLE_PROCESS_ALLOCATED; state <= CUBICLE_PROCESS_REMOVED; ++state) {
        if (strcmp(text, state_name((cubicle_process_state_t)state)) == 0) {
            *out = (cubicle_process_state_t)state;
            return 0;
        }
    }
    return -1;
}

int event_type_from_string(const char *text, cubicle_event_type_t *out)
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

int channel_mask_from_string(const char *text, cubicle_channel_mask_t *out)
{
    cubicle_channel_mask_t mask = CUBICLE_CHANNEL_NONE;
    if (strstr(text, "stdin") != NULL) mask |= CUBICLE_CHANNEL_STDIN;
    if (strstr(text, "stdout") != NULL) mask |= CUBICLE_CHANNEL_STDOUT;
    if (strstr(text, "stderr") != NULL) mask |= CUBICLE_CHANNEL_STDERR;
    if (strstr(text, "tty") != NULL) mask |= CUBICLE_CHANNEL_TTY;
    *out = mask;
    return 0;
}

int parse_endpoint(const char *json, cubicle_endpoint_t *endpoint)
{
    memset(endpoint, 0, sizeof(*endpoint));
    json_string_field(json, "uri", endpoint->uri, sizeof(endpoint->uri));
    json_string_field(json, "server_identity", endpoint->server_identity,
                      sizeof(endpoint->server_identity));
    return endpoint->uri[0] == '\0' ? -1 : 0;
}

int parse_page(const char *json, cubicle_page_info_t *page)
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

int parse_workspace_info(const char *json, cubicle_workspace_info_t *out)
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

int parse_process_info(const char *json, cubicle_process_info_t *out)
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

int parse_key_info(const char *json, cubicle_workspace_key_info_t *out)
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

int parse_event(const char *json, cubicle_event_t *out)
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

size_t count_array_objects(const char *array)
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

const char *next_array_object(const char *cursor, size_t *length_out)
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

char *copy_object_slice(const char *object, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, object, length);
    copy[length] = '\0';
    return copy;
}

