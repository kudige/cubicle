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

int cubicle_json_escape(char *buffer, size_t buffer_size, const char *value);
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
