#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int validate_json_value(yyjson_val *value, unsigned depth);

static yyjson_val *find_object_member(yyjson_val *object, const char *field,
                                      int *found_out)
{
    if (found_out != NULL) {
        *found_out = 0;
    }
    if (!yyjson_is_obj(object) || field == NULL) {
        errno = EINVAL;
        return NULL;
    }

    size_t field_length = strlen(field);
    yyjson_obj_iter iter = yyjson_obj_iter_with(object);
    yyjson_val *key = NULL;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        if (yyjson_get_len(key) == field_length &&
            memcmp(yyjson_get_str(key), field, field_length) == 0) {
            if (found_out != NULL) {
                *found_out = 1;
            }
            return yyjson_obj_iter_get_val(key);
        }
    }
    errno = ENOENT;
    return NULL;
}

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
    int found = 0;
    yyjson_val *value = find_object_member(object, field, &found);
    if (!found) {
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

static int set_validation_error(cubicle_validation_error_t *error, int code,
                                const char *field, const char *expected,
                                const char *message)
{
    errno = code;
    if (error != NULL) {
        error->code = code;
        snprintf(error->field_path, sizeof(error->field_path), "%s",
                 field == NULL ? "" : field);
        snprintf(error->expected, sizeof(error->expected), "%s",
                 expected == NULL ? "" : expected);
        snprintf(error->message, sizeof(error->message), "%s",
                 message == NULL ? "" : message);
    }
    return -1;
}

static yyjson_val *required_field(yyjson_val *object, const char *field,
                                  const char *expected,
                                  cubicle_validation_error_t *error)
{
    int found = 0;
    yyjson_val *value = find_object_member(object, field, &found);
    if (!found) {
        set_validation_error(error, ENOENT, field, expected,
                             "required field is missing");
    } else if (value == NULL) {
        set_validation_error(error, EINVAL, field, expected,
                             "null is not permitted");
    }
    return value;
}

static int optional_field(yyjson_val *object, const char *field,
                          yyjson_val **value_out, int *present_out,
                          cubicle_validation_error_t *error)
{
    if (value_out == NULL) {
        return set_validation_error(error, EINVAL, field, "value",
                                    "invalid output pointer");
    }
    int found = 0;
    yyjson_val *value = find_object_member(object, field, &found);
    if (!found) {
        *value_out = NULL;
        if (present_out != NULL) {
            *present_out = 0;
        }
        errno = 0;
        return 0;
    }
    if (value == NULL || yyjson_is_null(value)) {
        return set_validation_error(error, EINVAL, field, "non-null",
                                    "null is not permitted");
    }
    *value_out = value;
    if (present_out != NULL) {
        *present_out = 1;
    }
    return 0;
}

int cubicle_json_get_required_string(yyjson_val *object, const char *field,
                                     char *buffer, size_t buffer_size,
                                     cubicle_validation_error_t *error)
{
    yyjson_val *value = required_field(object, field, "string", error);
    if (value == NULL) {
        return -1;
    }
    if (!yyjson_is_str(value)) {
        return set_validation_error(error, EINVAL, field, "string",
                                    "field has the wrong type");
    }
    size_t length = yyjson_get_len(value);
    if (buffer == NULL || buffer_size == 0 || length >= buffer_size) {
        return set_validation_error(error, ENOSPC, field, "string",
                                    "string does not fit destination");
    }
    memcpy(buffer, yyjson_get_str(value), length);
    buffer[length] = '\0';
    return 0;
}

int cubicle_json_get_optional_string(yyjson_val *object, const char *field,
                                     char *buffer, size_t buffer_size,
                                     int *present_out,
                                     cubicle_validation_error_t *error)
{
    yyjson_val *value = NULL;
    if (optional_field(object, field, &value, present_out, error) < 0) {
        return -1;
    }
    if (value == NULL) {
        return 0;
    }
    if (!yyjson_is_str(value)) {
        return set_validation_error(error, EINVAL, field, "string",
                                    "field has the wrong type");
    }
    size_t length = yyjson_get_len(value);
    if (buffer == NULL || buffer_size == 0 || length >= buffer_size) {
        return set_validation_error(error, ENOSPC, field, "string",
                                    "string does not fit destination");
    }
    memcpy(buffer, yyjson_get_str(value), length);
    buffer[length] = '\0';
    return 0;
}

int cubicle_json_get_required_u64(yyjson_val *object, const char *field,
                                  uint64_t *value_out,
                                  cubicle_validation_error_t *error)
{
    yyjson_val *value = required_field(object, field, "uint64", error);
    if (value == NULL) {
        return -1;
    }
    if (!yyjson_is_uint(value) || value_out == NULL) {
        return set_validation_error(error, EINVAL, field, "uint64",
                                    "field has the wrong type");
    }
    *value_out = yyjson_get_uint(value);
    return 0;
}

int cubicle_json_get_optional_u64(yyjson_val *object, const char *field,
                                  uint64_t *value_out, int *present_out,
                                  cubicle_validation_error_t *error)
{
    yyjson_val *value = NULL;
    if (optional_field(object, field, &value, present_out, error) < 0) {
        return -1;
    }
    if (value == NULL) {
        return 0;
    }
    if (!yyjson_is_uint(value) || value_out == NULL) {
        return set_validation_error(error, EINVAL, field, "uint64",
                                    "field has the wrong type");
    }
    *value_out = yyjson_get_uint(value);
    return 0;
}

int cubicle_json_get_optional_bool(yyjson_val *object, const char *field,
                                   bool *value_out, int *present_out,
                                   cubicle_validation_error_t *error)
{
    yyjson_val *value = NULL;
    if (optional_field(object, field, &value, present_out, error) < 0) {
        return -1;
    }
    if (value == NULL) {
        return 0;
    }
    if (!yyjson_is_bool(value) || value_out == NULL) {
        return set_validation_error(error, EINVAL, field, "bool",
                                    "field has the wrong type");
    }
    *value_out = yyjson_get_bool(value);
    return 0;
}

int cubicle_json_get_required_object(yyjson_val *object, const char *field,
                                     yyjson_val **value_out,
                                     cubicle_validation_error_t *error)
{
    yyjson_val *value = required_field(object, field, "object", error);
    if (value == NULL) {
        return -1;
    }
    if (!yyjson_is_obj(value) || value_out == NULL) {
        return set_validation_error(error, EINVAL, field, "object",
                                    "field has the wrong type");
    }
    *value_out = value;
    return 0;
}

int cubicle_json_get_optional_object(yyjson_val *object, const char *field,
                                     yyjson_val **value_out, int *present_out,
                                     cubicle_validation_error_t *error)
{
    yyjson_val *value = NULL;
    if (optional_field(object, field, &value, present_out, error) < 0) {
        return -1;
    }
    if (value == NULL) {
        return 0;
    }
    if (!yyjson_is_obj(value)) {
        return set_validation_error(error, EINVAL, field, "object",
                                    "field has the wrong type");
    }
    *value_out = value;
    return 0;
}

int cubicle_json_get_required_array(yyjson_val *object, const char *field,
                                    yyjson_val **value_out,
                                    cubicle_validation_error_t *error)
{
    yyjson_val *value = required_field(object, field, "array", error);
    if (value == NULL) {
        return -1;
    }
    if (!yyjson_is_arr(value) || value_out == NULL) {
        return set_validation_error(error, EINVAL, field, "array",
                                    "field has the wrong type");
    }
    *value_out = value;
    return 0;
}

int cubicle_json_validate_ascii_identifier(const char *value,
                                           const char *field,
                                           cubicle_validation_error_t *error)
{
    if (value == NULL || value[0] == '\0') {
        return set_validation_error(error, EINVAL, field, "ASCII identifier",
                                    "identifier is empty");
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor) {
        if (*cursor < 0x21 || *cursor > 0x7e) {
            return set_validation_error(error, EINVAL, field,
                                        "ASCII identifier",
                                        "identifier contains invalid bytes");
        }
    }
    return 0;
}
