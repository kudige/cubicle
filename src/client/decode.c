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
    else if (strcmp(text, "term") == 0 ||
             strcmp(text, "tty-captured-stderr") == 0) *out = CUBICLE_PROCESS_TTY_CAPTURED_STDERR;
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
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    cubicle_validation_error_t error;
    int result =
        cubicle_json_get_required_string(parsed.root, "uri", endpoint->uri,
                                         sizeof(endpoint->uri), &error);
    if (result == 0 &&
        cubicle_json_get_optional_string(parsed.root, "server_identity",
                                         endpoint->server_identity,
                                         sizeof(endpoint->server_identity),
                                         NULL, &error) < 0) {
        result = -1;
    }
    cubicle_json_cleanup(&parsed);
    return result;
}

int parse_page(const char *json, cubicle_page_info_t *page)
{
    if (page == NULL) {
        return 0;
    }
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    memset(page, 0, sizeof(*page));
    cubicle_validation_error_t error;
    int result =
        cubicle_json_get_optional_string(parsed.root, "continuation_token",
                                         page->continuation_token,
                                         sizeof(page->continuation_token),
                                         NULL, &error) == 0 &&
                cubicle_json_get_optional_bool(parsed.root, "has_more",
                                               &page->has_more, NULL,
                                               &error) == 0
            ? 0
            : -1;
    cubicle_json_cleanup(&parsed);
    return result;
}

int parse_workspace_info(const char *json, cubicle_workspace_info_t *out)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    cubicle_validation_error_t error;
    int result =
        cubicle_json_get_required_string(parsed.root, "id", out->id,
                                         sizeof(out->id), &error) == 0 &&
                cubicle_json_get_required_string(parsed.root, "name",
                                                 out->name, sizeof(out->name),
                                                 &error) == 0
            ? 0
            : -1;
    if (result == 0 &&
        (cubicle_json_get_optional_string(parsed.root, "manager_id",
                                          out->manager_id,
                                          sizeof(out->manager_id), NULL,
                                          &error) < 0 ||
         cubicle_json_get_optional_string(parsed.root, "directory",
                                          out->directory,
                                          sizeof(out->directory), NULL,
                                          &error) < 0 ||
         cubicle_json_get_optional_u64(parsed.root, "created_at_ms",
                                       &out->created_at_ms, NULL, &error) < 0 ||
         cubicle_json_get_optional_u64(parsed.root, "updated_at_ms",
                                       &out->updated_at_ms, NULL, &error) < 0 ||
         cubicle_json_get_optional_u64(parsed.root, "process_count",
                                       &out->process_count, NULL, &error) < 0 ||
         cubicle_json_get_optional_u64(parsed.root, "running_process_count",
                                       &out->running_process_count, NULL,
                                       &error) < 0)) {
        result = -1;
    }
    cubicle_json_cleanup(&parsed);
    return result;
}

int parse_process_info(const char *json, cubicle_process_info_t *out)
{
    char text[64];
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    cubicle_validation_error_t error;
    if (cubicle_json_get_required_string(parsed.root, "id", out->id,
                                         sizeof(out->id), &error) < 0) {
        cubicle_json_cleanup(&parsed);
        return -1;
    }
    if (cubicle_json_get_optional_string(parsed.root, "manager_id",
                                         out->manager_id,
                                         sizeof(out->manager_id), NULL,
                                         &error) < 0 ||
        cubicle_json_get_optional_string(parsed.root, "workspace_id",
                                         out->workspace_id,
                                         sizeof(out->workspace_id), NULL,
                                         &error) < 0 ||
        cubicle_json_get_optional_string(parsed.root, "friendly_name",
                                         out->friendly_name,
                                         sizeof(out->friendly_name), NULL,
                                         &error) < 0 ||
        cubicle_json_get_optional_string(parsed.root, "cwd",
                                         out->cwd, sizeof(out->cwd), NULL,
                                         &error) < 0) {
        cubicle_json_cleanup(&parsed);
        return -1;
    }
    text[0] = '\0';
    if (cubicle_json_get_optional_string(parsed.root, "mode", text,
                                         sizeof(text), NULL, &error) == 0 &&
        text[0] != '\0' &&
        process_mode_from_string(text, &out->mode) < 0) {
        cubicle_json_cleanup(&parsed);
        return -1;
    }
    text[0] = '\0';
    if (cubicle_json_get_optional_string(parsed.root, "state", text,
                                         sizeof(text), NULL, &error) == 0 &&
        text[0] != '\0' &&
        process_state_from_string(text, &out->state) < 0) {
        cubicle_json_cleanup(&parsed);
        return -1;
    }
    yyjson_val *value = cubicle_json_object_get(parsed.root, "exit_code");
    if (yyjson_is_int(value)) out->exit_code = (int)yyjson_get_sint(value);
    value = cubicle_json_object_get(parsed.root, "termination_signal");
    if (yyjson_is_int(value)) {
        out->termination_signal = (int)yyjson_get_sint(value);
    }
    if (cubicle_json_get_optional_bool(parsed.root, "saved",
                                       &out->saved, NULL, &error) < 0 ||
        cubicle_json_get_optional_bool(parsed.root, "has_exit_status",
                                       &out->has_exit_status, NULL, &error) <
            0 ||
        cubicle_json_get_optional_u64(parsed.root, "stdout_offset",
                                      &out->stdout_offset, NULL, &error) < 0 ||
        cubicle_json_get_optional_u64(parsed.root, "stderr_offset",
                                      &out->stderr_offset, NULL, &error) < 0 ||
        cubicle_json_get_optional_u64(parsed.root, "tty_offset",
                                      &out->tty_offset, NULL, &error) < 0 ||
        cubicle_json_get_optional_u64(parsed.root, "created_at_ms",
                                      &out->created_at_ms, NULL, &error) < 0 ||
        cubicle_json_get_optional_u64(parsed.root, "started_at_ms",
                                      &out->started_at_ms, NULL, &error) < 0 ||
        cubicle_json_get_optional_u64(parsed.root, "exited_at_ms",
                                      &out->exited_at_ms, NULL, &error) < 0) {
        cubicle_json_cleanup(&parsed);
        return -1;
    }
    value = cubicle_json_object_get(parsed.root, "local_pid");
    if (yyjson_is_int(value)) out->local_pid = yyjson_get_sint(value);
    value = cubicle_json_object_get(parsed.root, "local_pgid");
    if (yyjson_is_int(value)) out->local_pgid = yyjson_get_sint(value);
    cubicle_json_cleanup(&parsed);
    return 0;
}

int parse_key_info(const char *json, cubicle_workspace_key_info_t *out)
{
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    cubicle_validation_error_t error;
    int result = cubicle_json_get_required_string(
        parsed.root, "key_id", out->key_id, sizeof(out->key_id), &error);
    if (result == 0 &&
        (cubicle_json_get_optional_string(parsed.root, "fingerprint",
                                          out->fingerprint,
                                          sizeof(out->fingerprint), NULL,
                                          &error) < 0 ||
         cubicle_json_get_optional_string(parsed.root, "label", out->label,
                                          sizeof(out->label), NULL, &error) <
             0 ||
         cubicle_json_get_optional_u64(parsed.root, "capabilities",
                                       &out->capabilities, NULL, &error) < 0 ||
         cubicle_json_get_optional_u64(parsed.root, "created_at_ms",
                                       &out->created_at_ms, NULL, &error) < 0 ||
         cubicle_json_get_optional_u64(parsed.root, "revoked_at_ms",
                                       &out->revoked_at_ms, NULL, &error) <
             0)) {
        result = -1;
    }
    cubicle_json_cleanup(&parsed);
    return result;
}

int parse_event(const char *json, cubicle_event_t *out)
{
    char type[64];
    cubicle_json_doc_t parsed;
    if (parse_object_root(json, &parsed) < 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    cubicle_validation_error_t error;
    if (cubicle_json_get_optional_u64(parsed.root, "global_sequence",
                                      &out->global_sequence, NULL, &error) <
            0 ||
        cubicle_json_get_optional_u64(parsed.root, "workspace_sequence",
                                      &out->workspace_sequence, NULL,
                                      &error) < 0 ||
        cubicle_json_get_optional_u64(parsed.root, "timestamp_ms",
                                      &out->timestamp_ms, NULL, &error) < 0) {
        cubicle_json_cleanup(&parsed);
        return -1;
    }
    type[0] = '\0';
    if (cubicle_json_get_optional_string(parsed.root, "type", type,
                                         sizeof(type), NULL, &error) == 0 &&
        type[0] != '\0' &&
        event_type_from_string(type, &out->type) < 0) {
        cubicle_json_cleanup(&parsed);
        return -1;
    }
    if (cubicle_json_get_optional_string(parsed.root, "workspace_id",
                                         out->workspace_id,
                                         sizeof(out->workspace_id), NULL,
                                         &error) < 0 ||
        cubicle_json_get_optional_string(parsed.root, "process_id",
                                         out->process_id,
                                         sizeof(out->process_id), NULL,
                                         &error) < 0 ||
        cubicle_json_get_optional_string(parsed.root, "payload",
                                         out->payload,
                                         sizeof(out->payload), NULL,
                                         &error) < 0) {
        cubicle_json_cleanup(&parsed);
        return -1;
    }
    cubicle_json_cleanup(&parsed);
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
