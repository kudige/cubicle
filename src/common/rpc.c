#include "cubicle/rpc.h"

#include "json.h"
#include "rpc_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int append_raw(char *buffer, size_t buffer_size, size_t *used,
                      const char *value)
{
    size_t length = strlen(value);
    if (*used + length >= buffer_size) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(buffer + *used, value, length);
    *used += length;
    buffer[*used] = '\0';
    return 0;
}

int cubicle_json_builder_init(cubicle_json_builder_t *builder)
{
    if (builder == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(builder, 0, sizeof(*builder));
    return 0;
}

void cubicle_json_builder_cleanup(cubicle_json_builder_t *builder)
{
    if (builder == NULL) {
        return;
    }
    free(builder->data);
    memset(builder, 0, sizeof(*builder));
}

int cubicle_json_builder_reserve(cubicle_json_builder_t *builder,
                                 size_t extra)
{
    if (builder == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (extra > (size_t)-1 - builder->length - 1) {
        errno = EOVERFLOW;
        return -1;
    }
    if (builder->length + extra + 1 <= builder->capacity) {
        return 0;
    }

    size_t capacity = builder->capacity == 0 ? 256 : builder->capacity;
    while (builder->length + extra + 1 > capacity) {
        if (capacity > ((size_t)-1) / 2) {
            errno = EOVERFLOW;
            return -1;
        }
        capacity *= 2;
    }

    char *data = realloc(builder->data, capacity);
    if (data == NULL) {
        errno = ENOMEM;
        return -1;
    }
    builder->data = data;
    builder->capacity = capacity;
    if (builder->length == 0) {
        builder->data[0] = '\0';
    }
    return 0;
}

int cubicle_json_builder_append(cubicle_json_builder_t *builder,
                                const char *text)
{
    if (builder == NULL || text == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t length = strlen(text);
    if (cubicle_json_builder_reserve(builder, length) < 0) {
        return -1;
    }
    memcpy(builder->data + builder->length, text, length + 1);
    builder->length += length;
    return 0;
}

int cubicle_json_builder_appendf(cubicle_json_builder_t *builder,
                                 const char *format, ...)
{
    if (builder == NULL || format == NULL) {
        errno = EINVAL;
        return -1;
    }

    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 ||
        cubicle_json_builder_reserve(builder, (size_t)needed) < 0) {
        va_end(args);
        return -1;
    }
    vsnprintf(builder->data + builder->length,
              builder->capacity - builder->length, format, args);
    va_end(args);
    builder->length += (size_t)needed;
    return 0;
}

int cubicle_json_builder_append_escaped(cubicle_json_builder_t *builder,
                                        const char *value)
{
    if (builder == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (value == NULL) {
        value = "";
    }

    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor) {
        char escaped[8];
        const char *chunk = NULL;
        switch (*cursor) {
        case '"':
            chunk = "\\\"";
            break;
        case '\\':
            chunk = "\\\\";
            break;
        case '\b':
            chunk = "\\b";
            break;
        case '\f':
            chunk = "\\f";
            break;
        case '\n':
            chunk = "\\n";
            break;
        case '\r':
            chunk = "\\r";
            break;
        case '\t':
            chunk = "\\t";
            break;
        default:
            if (*cursor < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                chunk = escaped;
            } else {
                if (cubicle_json_builder_reserve(builder, 1) < 0) {
                    return -1;
                }
                builder->data[builder->length++] = (char)*cursor;
                builder->data[builder->length] = '\0';
                continue;
            }
            break;
        }
        if (cubicle_json_builder_append(builder, chunk) < 0) {
            return -1;
        }
    }
    return 0;
}

int cubicle_json_builder_append_string(cubicle_json_builder_t *builder,
                                       const char *value)
{
    if (builder == NULL) {
        errno = EINVAL;
        return -1;
    }
    return cubicle_json_builder_append(builder, "\"") == 0 &&
                   cubicle_json_builder_append_escaped(builder, value) == 0 &&
                   cubicle_json_builder_append(builder, "\"") == 0
               ? 0
               : -1;
}

int cubicle_json_escape(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0 || value == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t used = 0;
    buffer[0] = '\0';
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor) {
        char escaped[8];
        const char *chunk = NULL;
        switch (*cursor) {
        case '"':
            chunk = "\\\"";
            break;
        case '\\':
            chunk = "\\\\";
            break;
        case '\b':
            chunk = "\\b";
            break;
        case '\f':
            chunk = "\\f";
            break;
        case '\n':
            chunk = "\\n";
            break;
        case '\r':
            chunk = "\\r";
            break;
        case '\t':
            chunk = "\\t";
            break;
        default:
            if (*cursor < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                chunk = escaped;
            } else {
                escaped[0] = (char)*cursor;
                escaped[1] = '\0';
                chunk = escaped;
            }
            break;
        }

        if (append_raw(buffer, buffer_size, &used, chunk) < 0) {
            return -1;
        }
    }

    return 0;
}

static int append_json_string(char *buffer, size_t buffer_size, size_t *used,
                              const char *value)
{
    char escaped[1024];
    if (cubicle_json_escape(escaped, sizeof(escaped), value) < 0) {
        return -1;
    }
    return append_raw(buffer, buffer_size, used, "\"") == 0 &&
                   append_raw(buffer, buffer_size, used, escaped) == 0 &&
                   append_raw(buffer, buffer_size, used, "\"") == 0
               ? 0
               : -1;
}

static int json_fragment_valid(const char *json)
{
    cubicle_json_doc_t parsed;
    if (cubicle_json_parse(&parsed, json) < 0) {
        return -1;
    }
    cubicle_json_cleanup(&parsed);
    return 0;
}

static int field_allowed(const char *field, const char *const *allowed,
                         size_t allowed_count)
{
    for (size_t i = 0; i < allowed_count; ++i) {
        if (strcmp(field, allowed[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int validate_top_level_fields(yyjson_val *object,
                                     const char *const *allowed,
                                     size_t allowed_count)
{
    if (!yyjson_is_obj(object)) {
        errno = EINVAL;
        return -1;
    }

    yyjson_obj_iter iter = yyjson_obj_iter_with(object);
    yyjson_val *key = NULL;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *field = yyjson_get_str(key);
        if (!field_allowed(field, allowed, allowed_count)) {
            errno = EINVAL;
            return -1;
        }
        if (strcmp(field, "extensions") == 0 &&
            !yyjson_is_obj(yyjson_obj_iter_get_val(key))) {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static int get_required_u32(yyjson_val *object, const char *field,
                            uint32_t *value_out)
{
    uint64_t value = 0;
    if (cubicle_json_get_u64(object, field, &value) < 0 ||
        value > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    *value_out = (uint32_t)value;
    return 0;
}

int cubicle_rpc_decode_request(cubicle_rpc_request_envelope_t *envelope,
                               const char *json)
{
    if (json == NULL) {
        errno = EINVAL;
        return -1;
    }
    return cubicle_rpc_decode_request_n(envelope, json, strlen(json));
}

int cubicle_rpc_decode_request_n(cubicle_rpc_request_envelope_t *envelope,
                                 const char *json, size_t length)
{
    if (envelope == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(envelope, 0, sizeof(*envelope));

    if (cubicle_json_parse_n(&envelope->document, json, length) < 0 ||
        !yyjson_is_obj(envelope->document.root)) {
        cubicle_rpc_request_envelope_cleanup(envelope);
        errno = EINVAL;
        return -1;
    }

    static const char *const allowed[] = {
        "protocol_major", "protocol_minor", "request_id", "session_id",
        "method", "params", "extensions",
    };
    if (validate_top_level_fields(envelope->document.root, allowed,
                                  sizeof(allowed) / sizeof(allowed[0])) < 0 ||
        get_required_u32(envelope->document.root, "protocol_major",
                         &envelope->protocol_major) < 0 ||
        get_required_u32(envelope->document.root, "protocol_minor",
                         &envelope->protocol_minor) < 0 ||
        cubicle_json_get_string(envelope->document.root, "request_id",
                                envelope->request_id,
                                sizeof(envelope->request_id)) < 0 ||
        cubicle_json_get_string(envelope->document.root, "method",
                                envelope->method,
                                sizeof(envelope->method)) < 0) {
        cubicle_rpc_request_envelope_cleanup(envelope);
        return -1;
    }

    (void)cubicle_json_get_string(envelope->document.root, "session_id",
                                  envelope->session_id,
                                  sizeof(envelope->session_id));

    if (envelope->protocol_major != CUBICLE_PROTOCOL_MAJOR ||
        envelope->protocol_minor > CUBICLE_PROTOCOL_MINOR ||
        envelope->request_id[0] == '\0' ||
        envelope->method[0] == '\0' ||
        strlen(envelope->method) > CUBICLE_JSON_MAX_METHOD_BYTES) {
        cubicle_rpc_request_envelope_cleanup(envelope);
        errno = EINVAL;
        return -1;
    }

    envelope->params = cubicle_json_get_object(envelope->document.root,
                                               "params");
    if (envelope->params == NULL) {
        cubicle_rpc_request_envelope_cleanup(envelope);
        return -1;
    }
    return 0;
}

void cubicle_rpc_request_envelope_cleanup(
    cubicle_rpc_request_envelope_t *envelope)
{
    if (envelope == NULL) {
        return;
    }
    cubicle_json_cleanup(&envelope->document);
    memset(envelope, 0, sizeof(*envelope));
}

int cubicle_rpc_decode_response(cubicle_rpc_response_envelope_t *envelope,
                                const char *json,
                                const char *expected_request_id)
{
    if (json == NULL) {
        errno = EINVAL;
        return -1;
    }
    return cubicle_rpc_decode_response_n(envelope, json, strlen(json),
                                         expected_request_id);
}

int cubicle_rpc_decode_response_n(cubicle_rpc_response_envelope_t *envelope,
                                  const char *json, size_t length,
                                  const char *expected_request_id)
{
    if (envelope == NULL || expected_request_id == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(envelope, 0, sizeof(*envelope));

    if (cubicle_json_parse_n(&envelope->document, json, length) < 0 ||
        !yyjson_is_obj(envelope->document.root)) {
        cubicle_rpc_response_envelope_cleanup(envelope);
        errno = EINVAL;
        return -1;
    }

    static const char *const allowed[] = {
        "request_id", "success", "result", "error", "extensions",
    };
    if (validate_top_level_fields(envelope->document.root, allowed,
                                  sizeof(allowed) / sizeof(allowed[0])) < 0 ||
        cubicle_json_get_string(envelope->document.root, "request_id",
                                envelope->request_id,
                                sizeof(envelope->request_id)) < 0 ||
        cubicle_json_get_bool(envelope->document.root, "success",
                              &envelope->success) < 0 ||
        strcmp(envelope->request_id, expected_request_id) != 0) {
        cubicle_rpc_response_envelope_cleanup(envelope);
        return -1;
    }

    envelope->result = cubicle_json_object_get(envelope->document.root,
                                               "result");
    envelope->error = cubicle_json_get_object(envelope->document.root,
                                              "error");
    if (envelope->success) {
        if (envelope->result == NULL || envelope->error != NULL) {
            cubicle_rpc_response_envelope_cleanup(envelope);
            errno = EINVAL;
            return -1;
        }
    } else if (envelope->error == NULL || envelope->result != NULL) {
        cubicle_rpc_response_envelope_cleanup(envelope);
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    return 0;
}

void cubicle_rpc_response_envelope_cleanup(
    cubicle_rpc_response_envelope_t *envelope)
{
    if (envelope == NULL) {
        return;
    }
    cubicle_json_cleanup(&envelope->document);
    memset(envelope, 0, sizeof(*envelope));
}

int cubicle_rpc_request(char *buffer, size_t buffer_size,
                        const char *request_id,
                        const char *session_id,
                        const char *method,
                        const char *params_json)
{
    if (buffer == NULL || buffer_size == 0 || request_id == NULL ||
        method == NULL || params_json == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (json_fragment_valid(params_json) < 0) {
        return -1;
    }

    size_t used = 0;
    buffer[0] = '\0';
    char prefix[96];
    int prefix_length = snprintf(prefix, sizeof(prefix),
                                 "{\"protocol_major\":%u,\"protocol_minor\":%u,\"request_id\":",
                                 CUBICLE_PROTOCOL_MAJOR,
                                 CUBICLE_PROTOCOL_MINOR);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix) ||
        append_raw(buffer, buffer_size, &used, prefix) < 0 ||
        append_json_string(buffer, buffer_size, &used, request_id) < 0 ||
        append_raw(buffer, buffer_size, &used, ",\"session_id\":") < 0 ||
        append_json_string(buffer, buffer_size, &used,
                           session_id == NULL ? "" : session_id) < 0 ||
        append_raw(buffer, buffer_size, &used, ",\"method\":") < 0 ||
        append_json_string(buffer, buffer_size, &used, method) < 0 ||
        append_raw(buffer, buffer_size, &used, ",\"params\":") < 0 ||
        append_raw(buffer, buffer_size, &used, params_json) < 0 ||
        append_raw(buffer, buffer_size, &used, "}") < 0) {
        return -1;
    }
    return 0;
}

int cubicle_rpc_success(char *buffer, size_t buffer_size,
                        const char *request_id,
                        const char *result_json)
{
    if (buffer == NULL || buffer_size == 0 || request_id == NULL ||
        result_json == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (json_fragment_valid(result_json) < 0) {
        return -1;
    }

    size_t used = 0;
    buffer[0] = '\0';
    return append_raw(buffer, buffer_size, &used, "{\"request_id\":") == 0 &&
                   append_json_string(buffer, buffer_size, &used,
                                      request_id) == 0 &&
                   append_raw(buffer, buffer_size, &used,
                              ",\"success\":true,\"result\":") == 0 &&
                   append_raw(buffer, buffer_size, &used, result_json) == 0 &&
                   append_raw(buffer, buffer_size, &used, "}") == 0
               ? 0
               : -1;
}

int cubicle_rpc_error(char *buffer, size_t buffer_size,
                      const char *request_id,
                      cubicle_error_code_t code,
                      const char *message,
                      int retryable,
                      int system_errno)
{
    if (buffer == NULL || buffer_size == 0 || request_id == NULL ||
        message == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t used = 0;
    buffer[0] = '\0';
    char suffix[128];
    int suffix_length = snprintf(suffix, sizeof(suffix),
                                 ",\"retryable\":%s,\"system_errno\":%d}}",
                                 retryable ? "true" : "false", system_errno);
    if (suffix_length < 0 || (size_t)suffix_length >= sizeof(suffix)) {
        errno = ENOSPC;
        return -1;
    }

    return append_raw(buffer, buffer_size, &used, "{\"request_id\":") == 0 &&
                   append_json_string(buffer, buffer_size, &used,
                                      request_id) == 0 &&
                   append_raw(buffer, buffer_size, &used,
                              ",\"success\":false,\"error\":{\"code\":") == 0 &&
                   append_json_string(buffer, buffer_size, &used,
                                      cubicle_error_code_name(code)) == 0 &&
                   append_raw(buffer, buffer_size, &used, ",\"message\":") == 0 &&
                   append_json_string(buffer, buffer_size, &used, message) == 0 &&
                   append_raw(buffer, buffer_size, &used, suffix) == 0
               ? 0
               : -1;
}

int cubicle_rpc_get_string(const char *json, const char *field,
                           char *value, size_t value_size)
{
    cubicle_json_doc_t parsed;
    if (cubicle_json_parse(&parsed, json) < 0) {
        return -1;
    }
    int result = cubicle_json_get_string(parsed.root, field, value, value_size);
    cubicle_json_cleanup(&parsed);
    return result;
}

int cubicle_rpc_get_uint64(const char *json, const char *field,
                           uint64_t *value_out)
{
    cubicle_json_doc_t parsed;
    if (cubicle_json_parse(&parsed, json) < 0) {
        return -1;
    }
    int result = cubicle_json_get_u64(parsed.root, field, value_out);
    cubicle_json_cleanup(&parsed);
    return result;
}

int cubicle_rpc_get_bool(const char *json, const char *field,
                         int *value_out)
{
    if (value_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    bool value = false;
    cubicle_json_doc_t parsed;
    if (cubicle_json_parse(&parsed, json) < 0) {
        return -1;
    }
    int result = cubicle_json_get_bool(parsed.root, field, &value);
    cubicle_json_cleanup(&parsed);
    if (result == 0) {
        *value_out = value ? 1 : 0;
    }
    return result;
}

int cubicle_rpc_get_object(const char *json, const char *field,
                           char *object, size_t object_size)
{
    if (object == NULL || object_size == 0) {
        errno = EINVAL;
        return -1;
    }

    cubicle_json_doc_t parsed;
    if (cubicle_json_parse(&parsed, json) < 0) {
        return -1;
    }
    yyjson_val *value = cubicle_json_get_object(parsed.root, field);
    char *copy = cubicle_json_copy_value(value);
    cubicle_json_cleanup(&parsed);
    if (copy == NULL) {
        return -1;
    }
    size_t length = strlen(copy);
    if (length >= object_size) {
        free(copy);
        errno = ENOSPC;
        return -1;
    }
    memcpy(object, copy, length + 1);
    free(copy);
    return 0;
}

int cubicle_rpc_response_ok(const char *json, int *ok_out)
{
    if (ok_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    cubicle_json_doc_t parsed;
    if (cubicle_json_parse(&parsed, json) < 0 || !yyjson_is_obj(parsed.root)) {
        cubicle_json_cleanup(&parsed);
        errno = EINVAL;
        return -1;
    }

    bool success = false;
    int result = cubicle_json_get_bool(parsed.root, "success", &success);
    cubicle_json_cleanup(&parsed);
    if (result == 0) {
        *ok_out = success ? 1 : 0;
    }
    return result;
}

static cubicle_error_code_t rpc_error_code_from_name(const char *name)
{
    for (int code = CUBICLE_OK; code <= CUBICLE_ERR_INTERNAL; ++code) {
        if (strcmp(cubicle_error_code_name((cubicle_error_code_t)code),
                   name) == 0) {
            return (cubicle_error_code_t)code;
        }
    }
    return CUBICLE_ERR_PROTOCOL;
}

int cubicle_rpc_decode_error_value(yyjson_val *error_value,
                                   cubicle_error_t *error)
{
    if (error == NULL || !yyjson_is_obj(error_value)) {
        errno = EINVAL;
        return -1;
    }

    char code_name[64];
    bool retryable = false;
    int64_t system_errno = 0;
    if (cubicle_json_get_string(error_value, "code", code_name,
                                sizeof(code_name)) < 0 ||
        cubicle_json_get_string(error_value, "message", error->message,
                                sizeof(error->message)) < 0 ||
        cubicle_json_get_bool(error_value, "retryable", &retryable) < 0 ||
        cubicle_json_get_i64(error_value, "system_errno",
                             &system_errno) < 0 ||
        system_errno < INT_MIN || system_errno > INT_MAX) {
        return -1;
    }

    error->code = rpc_error_code_from_name(code_name);
    error->retryable = retryable;
    error->system_errno = (int)system_errno;
    return 0;
}

int cubicle_rpc_response_error(const char *json, cubicle_error_t *error)
{
    if (error == NULL) {
        errno = EINVAL;
        return -1;
    }

    cubicle_json_doc_t parsed;
    if (cubicle_json_parse(&parsed, json) < 0 || !yyjson_is_obj(parsed.root)) {
        cubicle_json_cleanup(&parsed);
        errno = EINVAL;
        return -1;
    }

    static const char *const allowed[] = {
        "request_id", "success", "result", "error", "extensions",
    };
    char request_id[64];
    bool success = false;
    yyjson_val *result_value = cubicle_json_object_get(parsed.root, "result");
    yyjson_val *error_value = cubicle_json_get_object(parsed.root, "error");
    int decode_result = -1;
    if (validate_top_level_fields(parsed.root, allowed,
                                  sizeof(allowed) / sizeof(allowed[0])) == 0 &&
        cubicle_json_get_string(parsed.root, "request_id", request_id,
                                sizeof(request_id)) == 0 &&
        cubicle_json_get_bool(parsed.root, "success", &success) == 0 &&
        !success && result_value == NULL && error_value != NULL) {
        decode_result = cubicle_rpc_decode_error_value(error_value, error);
    }

    cubicle_json_cleanup(&parsed);
    if (decode_result < 0) {
        errno = EINVAL;
    }
    return decode_result;
}
