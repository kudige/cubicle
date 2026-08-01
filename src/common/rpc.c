#include "cubicle/rpc.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *cursor)
{
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

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

    char escaped_message[512];
    if (cubicle_json_escape(escaped_message, sizeof(escaped_message),
                            message) < 0) {
        return -1;
    }

    int length = snprintf(buffer, buffer_size,
                          "{\"request_id\":\"%s\",\"success\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"retryable\":%s,\"system_errno\":%d}}",
                          request_id, cubicle_error_code_name(code),
                          escaped_message, retryable ? "true" : "false",
                          system_errno);
    if (length < 0 || (size_t)length >= buffer_size) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static const char *find_field_value(const char *json, const char *field)
{
    if (json == NULL || field == NULL) {
        return NULL;
    }

    const char *cursor = json;
    char pattern[128];
    int length = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    if (length < 0 || (size_t)length >= sizeof(pattern)) {
        return NULL;
    }

    while ((cursor = strstr(cursor, pattern)) != NULL) {
        cursor += strlen(pattern);
        cursor = skip_ws(cursor);
        if (*cursor != ':') {
            continue;
        }
        return skip_ws(cursor + 1);
    }
    return NULL;
}

static int read_json_string(const char *cursor, char *value, size_t value_size)
{
    if (cursor == NULL || *cursor != '"' || value == NULL ||
        value_size == 0) {
        errno = EINVAL;
        return -1;
    }

    ++cursor;
    size_t used = 0;
    while (*cursor != '\0' && *cursor != '"') {
        char decoded = *cursor++;
        if (decoded == '\\') {
            decoded = *cursor++;
            switch (decoded) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                decoded = '\b';
                break;
            case 'f':
                decoded = '\f';
                break;
            case 'n':
                decoded = '\n';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 't':
                decoded = '\t';
                break;
            default:
                errno = EPROTO;
                return -1;
            }
        }
        if (used + 1 >= value_size) {
            errno = ENOSPC;
            return -1;
        }
        value[used++] = decoded;
    }
    if (*cursor != '"') {
        errno = EPROTO;
        return -1;
    }
    value[used] = '\0';
    return 0;
}

int cubicle_rpc_get_string(const char *json, const char *field,
                           char *value, size_t value_size)
{
    return read_json_string(find_field_value(json, field), value, value_size);
}

int cubicle_rpc_get_uint64(const char *json, const char *field,
                           uint64_t *value_out)
{
    if (value_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    const char *value = find_field_value(json, field);
    if (value == NULL || !isdigit((unsigned char)*value)) {
        errno = EINVAL;
        return -1;
    }
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value) {
        errno = EINVAL;
        return -1;
    }
    *value_out = (uint64_t)parsed;
    return 0;
}

int cubicle_rpc_get_bool(const char *json, const char *field,
                         int *value_out)
{
    if (value_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    const char *value = find_field_value(json, field);
    if (value == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (strncmp(value, "true", 4) == 0) {
        *value_out = 1;
        return 0;
    }
    if (strncmp(value, "false", 5) == 0) {
        *value_out = 0;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int cubicle_rpc_get_object(const char *json, const char *field,
                           char *object, size_t object_size)
{
    const char *value = find_field_value(json, field);
    if (value == NULL || *value != '{' || object == NULL ||
        object_size == 0) {
        errno = EINVAL;
        return -1;
    }

    int depth = 0;
    int in_string = 0;
    int escape = 0;
    size_t used = 0;
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (used + 1 >= object_size) {
            errno = ENOSPC;
            return -1;
        }
        object[used++] = *cursor;

        if (escape) {
            escape = 0;
            continue;
        }
        if (*cursor == '\\' && in_string) {
            escape = 1;
            continue;
        }
        if (*cursor == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (*cursor == '{') {
            ++depth;
        } else if (*cursor == '}') {
            --depth;
            if (depth == 0) {
                object[used] = '\0';
                return 0;
            }
        }
    }
    errno = EPROTO;
    return -1;
}

int cubicle_rpc_response_ok(const char *json, int *ok_out)
{
    return cubicle_rpc_get_bool(json, "success", ok_out);
}

static cubicle_error_code_t error_code_from_name(const char *name)
{
    for (int code = CUBICLE_OK; code <= CUBICLE_ERR_INTERNAL; ++code) {
        if (strcmp(cubicle_error_code_name((cubicle_error_code_t)code),
                   name) == 0) {
            return (cubicle_error_code_t)code;
        }
    }
    return CUBICLE_ERR_PROTOCOL;
}

int cubicle_rpc_response_error(const char *json, cubicle_error_t *error)
{
    if (error == NULL) {
        errno = EINVAL;
        return -1;
    }

    char error_object[1024];
    char code_name[64];
    uint64_t system_errno = 0;
    int retryable = 0;
    if (cubicle_rpc_get_object(json, "error", error_object,
                               sizeof(error_object)) < 0 ||
        cubicle_rpc_get_string(error_object, "code", code_name,
                               sizeof(code_name)) < 0 ||
        cubicle_rpc_get_string(error_object, "message", error->message,
                               sizeof(error->message)) < 0 ||
        cubicle_rpc_get_bool(error_object, "retryable", &retryable) < 0 ||
        cubicle_rpc_get_uint64(error_object, "system_errno",
                               &system_errno) < 0) {
        return -1;
    }

    error->code = error_code_from_name(code_name);
    error->retryable = retryable != 0;
    error->system_errno = (int)system_errno;
    return 0;
}
