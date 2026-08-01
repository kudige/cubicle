#define _POSIX_C_SOURCE 200809L

#include "cubicle/rpc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        char escape[8];
        switch (*cursor) {
        case '"':
            if (cubicle_json_builder_append(builder, "\\\"") < 0) return -1;
            break;
        case '\\':
            if (cubicle_json_builder_append(builder, "\\\\") < 0) return -1;
            break;
        case '\b':
            if (cubicle_json_builder_append(builder, "\\b") < 0) return -1;
            break;
        case '\f':
            if (cubicle_json_builder_append(builder, "\\f") < 0) return -1;
            break;
        case '\n':
            if (cubicle_json_builder_append(builder, "\\n") < 0) return -1;
            break;
        case '\r':
            if (cubicle_json_builder_append(builder, "\\r") < 0) return -1;
            break;
        case '\t':
            if (cubicle_json_builder_append(builder, "\\t") < 0) return -1;
            break;
        default:
            if (*cursor < 0x20) {
                snprintf(escape, sizeof(escape), "\\u%04x", *cursor);
                if (cubicle_json_builder_append(builder, escape) < 0) {
                    return -1;
                }
            } else {
                if (cubicle_json_builder_reserve(builder, 1) < 0) {
                    return -1;
                }
                builder->data[builder->length++] = (char)*cursor;
                builder->data[builder->length] = '\0';
            }
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

    cubicle_json_builder_t builder;
    if (cubicle_json_builder_init(&builder) < 0 ||
        cubicle_json_builder_append_escaped(&builder, value) < 0) {
        cubicle_json_builder_cleanup(&builder);
        return -1;
    }
    if (builder.length >= buffer_size) {
        cubicle_json_builder_cleanup(&builder);
        errno = ENOSPC;
        return -1;
    }
    if (builder.data == NULL) {
        buffer[0] = '\0';
    } else {
        memcpy(buffer, builder.data, builder.length + 1);
    }
    cubicle_json_builder_cleanup(&builder);
    return 0;
}
