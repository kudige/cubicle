#ifndef CUBICLE_RPC_H
#define CUBICLE_RPC_H

#include "cubicle/client_error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUBICLE_PROTOCOL_MAJOR 0
#define CUBICLE_PROTOCOL_MINOR 1

typedef struct cubicle_json_builder {
    char *data;
    size_t length;
    size_t capacity;
} cubicle_json_builder_t;

int cubicle_json_escape(char *buffer, size_t buffer_size, const char *value);

int cubicle_json_builder_init(cubicle_json_builder_t *builder);
void cubicle_json_builder_cleanup(cubicle_json_builder_t *builder);
int cubicle_json_builder_reserve(cubicle_json_builder_t *builder,
                                 size_t extra);
int cubicle_json_builder_append(cubicle_json_builder_t *builder,
                                const char *text);
int cubicle_json_builder_appendf(cubicle_json_builder_t *builder,
                                 const char *format, ...);
int cubicle_json_builder_append_escaped(cubicle_json_builder_t *builder,
                                        const char *value);
int cubicle_json_builder_append_string(cubicle_json_builder_t *builder,
                                       const char *value);
int cubicle_json_builder_append_string_n(cubicle_json_builder_t *builder,
                                         const void *value,
                                         size_t value_length);
size_t cubicle_json_safe_utf8_prefix_length(const void *value,
                                            size_t value_length);

int cubicle_rpc_request(char *buffer, size_t buffer_size,
                        const char *request_id,
                        const char *session_id,
                        const char *method,
                        const char *params_json);
int cubicle_rpc_success(char *buffer, size_t buffer_size,
                        const char *request_id,
                        const char *result_json);
int cubicle_rpc_error(char *buffer, size_t buffer_size,
                      const char *request_id,
                      cubicle_error_code_t code,
                      const char *message,
                      int retryable,
                      int system_errno);
int cubicle_rpc_get_string(const char *json, const char *field,
                           char *value, size_t value_size);
int cubicle_rpc_get_uint64(const char *json, const char *field,
                           uint64_t *value_out);
int cubicle_rpc_get_bool(const char *json, const char *field,
                         int *value_out);
int cubicle_rpc_get_object(const char *json, const char *field,
                           char *object, size_t object_size);
int cubicle_rpc_response_ok(const char *json, int *ok_out);
int cubicle_rpc_response_error(const char *json, cubicle_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
