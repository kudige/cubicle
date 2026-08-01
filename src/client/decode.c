#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_object_root(const char *json, cubicle_json_doc_t *parsed)
{
    if (cubicle_json_parse(parsed, json) < 0 || !yyjson_is_obj(parsed->root)) {
        cubicle_json_cleanup(parsed);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int json_bool_field(const char *json, const char *key, bool *value_out)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    int result = cubicle_json_get_bool(parsed.root, key, value_out);
    cubicle_json_cleanup(&parsed);
    return result;
}

int json_u64_field(const char *json, const char *key, uint64_t *value_out)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    int result = cubicle_json_get_u64(parsed.root, key, value_out);
    cubicle_json_cleanup(&parsed);
    return result;
}

int json_i64_field(const char *json, const char *key, int64_t *value_out)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    int result = cubicle_json_get_i64(parsed.root, key, value_out);
    cubicle_json_cleanup(&parsed);
    return result;
}

int json_string_field(const char *json, const char *key,
                             char *buffer, size_t buffer_size)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    int result = cubicle_json_get_string(parsed.root, key, buffer, buffer_size);
    cubicle_json_cleanup(&parsed);
    return result;
}

const char *json_object_field(const char *json, const char *key)
{
    static char *cached;
    char *source_copy = NULL;
    const char *source = json;
    if (json == cached && json != NULL) {
        size_t length = strlen(json) + 1;
        source_copy = malloc(length);
        if (source_copy == NULL) {
            return NULL;
        }
        memcpy(source_copy, json, length);
        source = source_copy;
    }
    free(cached);
    cached = json_object_field_copy(source, key);
    free(source_copy);
    return cached;
}

const char *json_array_field(const char *json, const char *key)
{
    static char *cached;
    cubicle_json_doc_t parsed;
    free(cached);
    cached = NULL;
    if (parse_object_root(json, &parsed) < 0) {
        return NULL;
    }
    yyjson_val *array = cubicle_json_get_array(parsed.root, key);
    cached = cubicle_json_copy_value(array);
    cubicle_json_cleanup(&parsed);
    return cached;
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

size_t json_array_field_count(const char *json, const char *key)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return 0;
    }
    yyjson_val *array = cubicle_json_get_array(parsed.root, key);
    size_t count = cubicle_json_array_size(array);
    cubicle_json_cleanup(&parsed);
    return count;
}

char *json_array_field_object_copy(const char *json, const char *key,
                                   size_t index)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return NULL;
    }
    yyjson_val *array = cubicle_json_get_array(parsed.root, key);
    yyjson_val *item = cubicle_json_array_get(array, index);
    if (!yyjson_is_obj(item)) {
        cubicle_json_cleanup(&parsed);
        errno = EINVAL;
        return NULL;
    }
    char *copy = cubicle_json_copy_value(item);
    cubicle_json_cleanup(&parsed);
    return copy;
}

char *json_object_field_copy(const char *json, const char *key)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return NULL;
    }
    yyjson_val *object = cubicle_json_get_object(parsed.root, key);
    char *copy = cubicle_json_copy_value(object);
    cubicle_json_cleanup(&parsed);
    return copy;
}
