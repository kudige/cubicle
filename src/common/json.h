#ifndef CUBICLE_COMMON_JSON_H
#define CUBICLE_COMMON_JSON_H

#include "yyjson.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CUBICLE_JSON_MAX_DOCUMENT_BYTES (16U * 1024U * 1024U)
#define CUBICLE_JSON_MAX_DEPTH 32U
#define CUBICLE_JSON_MAX_OBJECT_MEMBERS 256U
#define CUBICLE_JSON_MAX_ARRAY_ELEMENTS 4096U
#define CUBICLE_JSON_MAX_STRING_BYTES (1024U * 1024U)
#define CUBICLE_JSON_MAX_METHOD_BYTES 128U
#define CUBICLE_JSON_MAX_ERROR_MESSAGE_BYTES 4096U
#define CUBICLE_JSON_MAX_ARGC 4096U
#define CUBICLE_JSON_MAX_ENV_COUNT 4096U

typedef struct cubicle_json_doc {
    yyjson_doc *doc;
    yyjson_val *root;
} cubicle_json_doc_t;

int cubicle_json_parse(cubicle_json_doc_t *parsed, const char *json);
void cubicle_json_cleanup(cubicle_json_doc_t *parsed);

yyjson_val *cubicle_json_object_get(yyjson_val *object, const char *field);
int cubicle_json_get_string(yyjson_val *object, const char *field,
                            char *buffer, size_t buffer_size);
int cubicle_json_get_u64(yyjson_val *object, const char *field,
                         uint64_t *value_out);
int cubicle_json_get_i64(yyjson_val *object, const char *field,
                         int64_t *value_out);
int cubicle_json_get_bool(yyjson_val *object, const char *field,
                          bool *value_out);
yyjson_val *cubicle_json_get_object(yyjson_val *object, const char *field);
yyjson_val *cubicle_json_get_array(yyjson_val *object, const char *field);
char *cubicle_json_copy_value(yyjson_val *value);
char *cubicle_json_copy_field(yyjson_val *object, const char *field);
size_t cubicle_json_array_size(yyjson_val *array);
yyjson_val *cubicle_json_array_get(yyjson_val *array, size_t index);

#endif
