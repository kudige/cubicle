#include "json.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int validate_json_value(yyjson_val *value, unsigned depth);

static int validate_json_object(yyjson_val *object, unsigned depth)
{
    size_t count = yyjson_obj_size(object);
    if (count > CUBICLE_JSON_MAX_OBJECT_MEMBERS) {
        errno = E2BIG;
        return -1;
    }

    yyjson_obj_iter outer = yyjson_obj_iter_with(object);
    yyjson_val *key = NULL;
    size_t index = 0;
    while ((key = yyjson_obj_iter_next(&outer)) != NULL) {
        if (!yyjson_is_str(key) ||
            yyjson_get_len(key) > CUBICLE_JSON_MAX_STRING_BYTES) {
            errno = EINVAL;
            return -1;
        }

        yyjson_obj_iter inner = yyjson_obj_iter_with(object);
        yyjson_val *candidate = NULL;
        size_t candidate_index = 0;
        while ((candidate = yyjson_obj_iter_next(&inner)) != NULL) {
            if (candidate_index > index &&
                yyjson_get_len(candidate) == yyjson_get_len(key) &&
                memcmp(yyjson_get_str(candidate), yyjson_get_str(key),
                       yyjson_get_len(key)) == 0) {
                errno = EINVAL;
                return -1;
            }
            ++candidate_index;
        }

        if (validate_json_value(yyjson_obj_iter_get_val(key),
                                depth + 1) < 0) {
            return -1;
        }
        ++index;
    }
    return 0;
}

static int validate_json_array(yyjson_val *array, unsigned depth)
{
    if (yyjson_arr_size(array) > CUBICLE_JSON_MAX_ARRAY_ELEMENTS) {
        errno = E2BIG;
        return -1;
    }

    yyjson_arr_iter iter = yyjson_arr_iter_with(array);
    yyjson_val *item = NULL;
    while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
        if (validate_json_value(item, depth + 1) < 0) {
            return -1;
        }
    }
    return 0;
}

static int validate_json_value(yyjson_val *value, unsigned depth)
{
    if (value == NULL || depth > CUBICLE_JSON_MAX_DEPTH) {
        errno = EINVAL;
        return -1;
    }
    if (yyjson_is_str(value) &&
        yyjson_get_len(value) > CUBICLE_JSON_MAX_STRING_BYTES) {
        errno = E2BIG;
        return -1;
    }
    if (yyjson_is_obj(value)) {
        return validate_json_object(value, depth);
    }
    if (yyjson_is_arr(value)) {
        return validate_json_array(value, depth);
    }
    return 0;
}

int cubicle_json_parse(cubicle_json_doc_t *parsed, const char *json)
{
    if (parsed == NULL || json == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t length = strlen(json);
    if (length > CUBICLE_JSON_MAX_DOCUMENT_BYTES) {
        errno = E2BIG;
        return -1;
    }

    memset(parsed, 0, sizeof(*parsed));
    yyjson_read_err error;
    parsed->doc = yyjson_read_opts((char *)(void *)json, length,
                                   YYJSON_READ_NOFLAG, NULL, &error);
    if (parsed->doc == NULL) {
        errno = EPROTO;
        return -1;
    }

    parsed->root = yyjson_doc_get_root(parsed->doc);
    if (parsed->root == NULL) {
        cubicle_json_cleanup(parsed);
        errno = EPROTO;
        return -1;
    }
    if (validate_json_value(parsed->root, 1) < 0) {
        cubicle_json_cleanup(parsed);
        return -1;
    }
    return 0;
}

void cubicle_json_cleanup(cubicle_json_doc_t *parsed)
{
    if (parsed == NULL) {
        return;
    }
    yyjson_doc_free(parsed->doc);
    memset(parsed, 0, sizeof(*parsed));
}

yyjson_val *cubicle_json_object_get(yyjson_val *object, const char *field)
{
    if (!yyjson_is_obj(object) || field == NULL) {
        errno = EINVAL;
        return NULL;
    }
    yyjson_val *value = yyjson_obj_get(object, field);
    if (value == NULL) {
        errno = ENOENT;
    }
    return value;
}

int cubicle_json_get_string(yyjson_val *object, const char *field,
                            char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }
    yyjson_val *value = cubicle_json_object_get(object, field);
    if (!yyjson_is_str(value)) {
        errno = EINVAL;
        return -1;
    }
    const char *text = yyjson_get_str(value);
    size_t length = strlen(text);
    if (length >= buffer_size) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(buffer, text, length + 1);
    return 0;
}

int cubicle_json_get_u64(yyjson_val *object, const char *field,
                         uint64_t *value_out)
{
    if (value_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    yyjson_val *value = cubicle_json_object_get(object, field);
    if (!yyjson_is_uint(value)) {
        errno = EINVAL;
        return -1;
    }
    *value_out = yyjson_get_uint(value);
    return 0;
}

int cubicle_json_get_i64(yyjson_val *object, const char *field,
                         int64_t *value_out)
{
    if (value_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    yyjson_val *value = cubicle_json_object_get(object, field);
    if (!yyjson_is_int(value)) {
        errno = EINVAL;
        return -1;
    }
    *value_out = yyjson_get_sint(value);
    return 0;
}

int cubicle_json_get_bool(yyjson_val *object, const char *field,
                          bool *value_out)
{
    if (value_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    yyjson_val *value = cubicle_json_object_get(object, field);
    if (!yyjson_is_bool(value)) {
        errno = EINVAL;
        return -1;
    }
    *value_out = yyjson_get_bool(value);
    return 0;
}

yyjson_val *cubicle_json_get_object(yyjson_val *object, const char *field)
{
    yyjson_val *value = cubicle_json_object_get(object, field);
    if (!yyjson_is_obj(value)) {
        errno = EINVAL;
        return NULL;
    }
    return value;
}

yyjson_val *cubicle_json_get_array(yyjson_val *object, const char *field)
{
    yyjson_val *value = cubicle_json_object_get(object, field);
    if (!yyjson_is_arr(value)) {
        errno = EINVAL;
        return NULL;
    }
    return value;
}

char *cubicle_json_copy_value(yyjson_val *value)
{
    if (value == NULL) {
        errno = EINVAL;
        return NULL;
    }
    char *copy = yyjson_val_write(value, YYJSON_WRITE_NOFLAG, NULL);
    if (copy == NULL) {
        errno = ENOMEM;
    }
    return copy;
}

char *cubicle_json_copy_field(yyjson_val *object, const char *field)
{
    return cubicle_json_copy_value(cubicle_json_object_get(object, field));
}

size_t cubicle_json_array_size(yyjson_val *array)
{
    return yyjson_is_arr(array) ? yyjson_arr_size(array) : 0;
}

yyjson_val *cubicle_json_array_get(yyjson_val *array, size_t index)
{
    if (!yyjson_is_arr(array)) {
        errno = EINVAL;
        return NULL;
    }
    yyjson_val *value = yyjson_arr_get(array, index);
    if (value == NULL) {
        errno = ENOENT;
    }
    return value;
}
